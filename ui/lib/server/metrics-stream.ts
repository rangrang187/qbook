import 'server-only';
import { Subscriber } from 'zeromq';
import type { Buffer } from 'node:buffer';
import { decode, type MetricsDto } from '../metrics';

/** 窄范围的 socket 工厂边界供故障测试使用；生产 GET 固定使用真实 Subscriber。 */
export interface MetricsSubscriber extends AsyncIterable<Buffer[]> {
  conflate: boolean;
  linger: number;
  subscribe(prefix: string): void;
  connect(endpoint: string): void;
  close(): void;
}

export interface MetricsStreamOptions {
  endpoint?: string;
  createSubscriber?: () => MetricsSubscriber;
  warn?: (message: string, error?: unknown) => void;
}

type CloseReason = 'request-abort' | 'stream-cancel' | 'receive-error' |
  'unexpected-end' | 'setup-error' | 'server-stop';

const HEADERS = {
  'Content-Type': 'text/event-stream; charset=utf-8',
  'Cache-Control': 'no-cache, no-store, no-transform',
  'X-Accel-Buffering': 'no',
};
const encoder = new TextEncoder();

// 仅共享停止通知，不共享 socket、接收任务或样本。无活动请求时完全卸载。
const activeClosers = new Set<() => void>();
function broadcastServerStop(): void {
  for (const close of [...activeClosers]) close();
}
function registerServerStop(close: () => void): () => void {
  if (activeClosers.size === 0) {
    process.on('SIGINT', broadcastServerStop);
    try { process.on('SIGTERM', broadcastServerStop); }
    catch (error) { process.off('SIGINT', broadcastServerStop); throw error; }
  }
  activeClosers.add(close);
  return () => {
    activeClosers.delete(close);
    if (activeClosers.size === 0) {
      try { process.off('SIGINT', broadcastServerStop); }
      finally { process.off('SIGTERM', broadcastServerStop); }
    }
  };
}

/** 只使用服务端配置。空值与格式错误必须显式失败，不能退回默认端点。 */
export function metricsEndpoint(raw: string | undefined): string {
  const endpoint = raw === undefined ? 'tcp://127.0.0.1:5556' : raw;
  if (!endpoint || /\s|\0/.test(endpoint)) throw new Error('invalid QBOOK_METRICS_ENDPOINT');
  if (endpoint.startsWith('tcp://')) {
    const url = new URL(endpoint);
    const port = Number(url.port);
    if (!url.hostname || !Number.isInteger(port) || port < 1 || port > 65535 ||
        url.username || url.password || url.search || url.hash || url.pathname) {
      throw new Error('invalid TCP QBOOK_METRICS_ENDPOINT');
    }
  } else if (!/^ipc:\/\/(\/|@).+/.test(endpoint)) {
    throw new Error('QBOOK_METRICS_ENDPOINT must be a TCP or absolute IPC endpoint');
  }
  return endpoint;
}

/**
 * 每个调用独占一个 SUB 和一个接收任务。HTTP 应用队列最多一个完整事件，
 * 队列外仅留一份最新 DTO；这不限制 HTTP/操作系统内部缓冲。
 */
export function createMetricsResponse(signal: AbortSignal, options: MetricsStreamOptions = {}): Response {
  const endpoint = metricsEndpoint(options.endpoint ?? process.env.QBOOK_METRICS_ENDPOINT);
  const createSubscriber = options.createSubscriber ?? (() => new Subscriber());
  const logger = options.warn ?? ((message, error) => console.warn(message, error ?? ''));
  let sub: MetricsSubscriber | undefined;
  let controller: ReadableStreamDefaultController<Uint8Array> | undefined;
  let closed = false;
  let latestPending: MetricsDto | undefined;
  let receiveTask: Promise<void> | undefined;
  let unregisterServerStop: (() => void) | undefined;
  let lastRejectionLog = -Infinity;

  // 日志/二次清理失败不能打断其余资源释放或覆盖首次失败。
  function report(message: string, error?: unknown): void {
    try { logger(message, error); } catch { /* 诊断不可破坏清理。 */ }
  }

  function releaseSocket(): void {
    const owned = sub;
    sub = undefined;
    if (owned) {
      try { owned.close(); } catch (error) { report('metrics socket cleanup failed', error); }
    }
  }

  function closeOnce(reason: CloseReason, error?: unknown): void {
    if (closed) return;
    closed = true; // 先夺取关闭职责，随后所有可能重入的动作都观察到 closed。
    try { signal.removeEventListener('abort', onAbort); }
    catch (cleanupError) { report('metrics abort listener cleanup failed', cleanupError); }
    try { unregisterServerStop?.(); }
    catch (cleanupError) { report('metrics shutdown listener cleanup failed', cleanupError); }
    unregisterServerStop = undefined;
    latestPending = undefined;
    if (reason === 'receive-error' || reason === 'unexpected-end' || reason === 'setup-error') {
      report(`metrics stream ${reason}`, error);
    }
    releaseSocket();
    if (controller && reason !== 'stream-cancel') {
      try {
        if (reason === 'request-abort') controller.close();
        else controller.error(error ?? new Error(`metrics stream ${reason}`));
      } catch (cleanupError) { report('metrics stream cleanup failed', cleanupError); }
    }
    controller = undefined;
  }

  function onAbort(): void { closeOnce('request-abort'); }

  // Next production 先等待全部 HTTP 响应结束再退出。长连接必须参与停止，
  // error 会中断 HTTP 写等待，不让慢 SSE 客户端阻止整个 Node 进程退出。
  function onServerStop(): void { closeOnce('server-stop', new Error('metrics server stopping')); }

  function drainLatest(): void {
    if (closed || !latestPending || !controller || (controller.desiredSize ?? 0) <= 0) return;
    try {
      const chunk = encoder.encode(`data: ${JSON.stringify(latestPending)}\n\n`);
      controller.enqueue(chunk);
      latestPending = undefined;
    } catch (error) { closeOnce('receive-error', error); }
  }

  async function receiveLoop(socket: MetricsSubscriber): Promise<void> {
    try {
      for await (const parts of socket) {
        if (closed) return;
        let sample: MetricsDto;
        try {
          if (parts.length !== 1) throw new Error('metrics must be one frame');
          sample = decode(parts[0]);
        } catch (error) {
          const now = Date.now();
          if (now - lastRejectionLog >= 1000) {
            lastRejectionLog = now;
            report('rejected metrics frame', error);
          }
          continue;
        }
        latestPending = sample;
        drainLatest();
      }
      if (!closed) closeOnce('unexpected-end', new Error('metrics subscription ended unexpectedly'));
    } catch (error) {
      if (!closed) closeOnce('receive-error', error);
    }
  }

  function requireOpen(): void {
    if (closed) throw new DOMException('metrics request aborted during setup', 'AbortError');
  }

  try {
    unregisterServerStop = registerServerStop(onServerStop);
    signal.addEventListener('abort', onAbort);
    if (signal.aborted) onAbort(); // 防止检查与注册之间的取消竞态。
    if (closed) return new Response(null, { status: 499 });
    sub = createSubscriber();
    // 工厂/选项设置也可能触发同步取消；刚交付的资源仍须立即回收。
    if (closed) releaseSocket();
    requireOpen();
    sub!.conflate = true;
    requireOpen();
    sub!.linger = 0;
    requireOpen();
    sub!.subscribe('');
    requireOpen();
    sub!.connect(endpoint);
    requireOpen();
    const socket = sub!;
    const stream = new ReadableStream<Uint8Array>({
      start(value) {
        controller = value;
        if (closed) { controller.close(); controller = undefined; return; }
        controller.enqueue(encoder.encode(': qbook metrics; latest samples only\n\n'));
        // 唯一任务立刻安装拒绝处理；取消只从外部等待它，不让循环等待自身。
        receiveTask = receiveLoop(socket).catch(error => closeOnce('receive-error', error));
      },
      pull() { drainLatest(); },
      cancel() {
        closeOnce('stream-cancel');
        return receiveTask;
      },
    }, { highWaterMark: 1, size: () => 1 });
    return new Response(stream, { headers: HEADERS });
  } catch (error) {
    closeOnce('setup-error', error);
    if (signal.aborted) return new Response(null, { status: 499 });
    throw error;
  }
}
