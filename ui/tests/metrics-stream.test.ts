import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { Subscriber } from 'zeromq';
import { createMetricsResponse, metricsEndpoint, type MetricsSubscriber } from '../lib/server/metrics-stream';

let cachedFixture: Buffer | undefined;
const fixture = () => Buffer.from(cachedFixture ??= readFileSync(fileURLToPath(new URL('../../tests/fixtures/metrics/layout.bin', import.meta.url))));
const decoder = new TextDecoder();
const flush = () => new Promise<void>(resolve => setImmediate(resolve));

class FakeSubscriber implements MetricsSubscriber, AsyncIterator<Buffer[]> {
  conflate = false;
  linger = -1;
  closed = false;
  closeCalls = 0;
  received = 0;
  returned = false;
  subscribed: string | undefined;
  endpoint: string | undefined;
  failAt: string | undefined;
  private pending?: { resolve: (value: IteratorResult<Buffer[]>) => void; reject: (error: unknown) => void };

  [Symbol.asyncIterator](): AsyncIterator<Buffer[]> { return this; }
  subscribe(prefix: string) { this.subscribed = prefix; if (this.failAt === 'subscribe') throw new Error('subscribe failure'); }
  connect(endpoint: string) { this.endpoint = endpoint; if (this.failAt === 'connect') throw new Error('connect failure'); }
  next(): Promise<IteratorResult<Buffer[]>> {
    if (this.closed) { this.returned = true; return Promise.resolve({ done: true, value: undefined }); }
    if (this.pending) throw new Error('concurrent receive');
    return new Promise((resolve, reject) => { this.pending = { resolve, reject }; });
  }
  async return(): Promise<IteratorResult<Buffer[]>> { this.returned = true; return { done: true, value: undefined }; }
  frame(parts: Buffer[]) {
    const pending = this.pending;
    if (!pending) throw new Error('test sent without an active receive');
    this.pending = undefined;
    this.received++;
    pending.resolve({ done: false, value: parts });
  }
  fail(error: Error) { const pending = this.pending!; this.pending = undefined; pending.reject(error); }
  end() { const pending = this.pending!; this.pending = undefined; pending.resolve({ done: true, value: undefined }); }
  close() {
    this.closeCalls++;
    this.closed = true;
    if (this.pending) { const pending = this.pending; this.pending = undefined; this.returned = true; pending.resolve({ done: true, value: undefined }); }
    if (this.failAt === 'close') throw new Error('secondary close failure');
  }
}

function start(socket = new FakeSubscriber()) {
  const abort = new AbortController();
  const add = vi.spyOn(abort.signal, 'addEventListener');
  const remove = vi.spyOn(abort.signal, 'removeEventListener');
  const warn = vi.fn();
  const response = createMetricsResponse(abort.signal, { createSubscriber: () => socket, warn });
  const reader = response.body!.getReader();
  return { socket, abort, add, remove, warn, response, reader };
}

afterEach(() => vi.restoreAllMocks());

describe('request-scoped SSE bridge', () => {
  it('returns headers immediately and creates one isolated configured SUB per request', async () => {
    const first = start(); const second = start();
    try {
      expect(first.response.headers.get('content-type')).toBe('text/event-stream; charset=utf-8');
      expect(first.response.headers.get('cache-control')).toBe('no-cache, no-store, no-transform');
      expect(first.response.headers.get('x-accel-buffering')).toBe('no');
      expect(decoder.decode((await first.reader.read()).value)).toBe(': qbook metrics; latest samples only\n\n');
      expect(first.socket.conflate).toBe(true);
      expect(first.socket.linger).toBe(0);
      expect(first.socket.subscribed).toBe('');
      expect(first.socket).not.toBe(second.socket);
    } finally { await first.reader.cancel(); await second.reader.cancel(); }
    expect(first.socket.closeCalls).toBe(1);
    expect(first.socket.returned).toBe(true);
    expect(first.remove).toHaveBeenCalledWith('abort', first.add.mock.calls[0][1]);
  });

  it('rejects malformed/multipart messages, rate-limits diagnostics and recovers', async () => {
    const t = start();
    try {
      await t.reader.read();
      for (const parts of [[Buffer.alloc(135)], [fixture(), fixture()], []]) {
        t.socket.frame(parts); await flush();
      }
      const valid = fixture(); t.socket.frame([valid]); await flush();
      const chunk = decoder.decode((await t.reader.read()).value);
      expect(chunk.startsWith('data: {')).toBe(true);
      expect(chunk.endsWith('\n\n')).toBe(true);
      expect(t.warn).toHaveBeenCalledTimes(1);
    } finally { await t.reader.cancel(); }
  });

  it('holds at most one queued event and the latest pending sample for a slow reader', async () => {
    const t = start();
    try {
      await t.reader.read();
      for (let n = 1; n <= 1000; n++) {
        const bytes = fixture(); bytes.writeBigUInt64LE(BigInt(n), 8);
        t.socket.frame([bytes]); await flush();
      }
      const readCounter = async () => JSON.parse(decoder.decode((await t.reader.read()).value).slice(6)).feedMessages;
      expect(await readCounter()).toBe('1');
      expect(await readCounter()).toBe('1000');
      expect(t.socket.received).toBe(1000);
      // 没有第三条历史事件；取消使挂起读取结束。
      let thirdSettled = false;
      const third = t.reader.read().then(value => { thirdSettled = true; return value; });
      await flush(); expect(thirdSettled).toBe(false);
      await t.reader.cancel(); expect((await third).done).toBe(true);
    } finally { await t.reader.cancel(); }
  });

  it('closes a waiting receive exactly once across abort and repeated cancel', async () => {
    const t = start();
    await t.reader.read();
    const waiting = t.reader.read();
    t.abort.abort(); t.abort.abort();
    await t.reader.cancel(); await t.reader.cancel(); await flush();
    expect((await waiting).done).toBe(true);
    expect(t.socket.closeCalls).toBe(1);
    expect(t.socket.returned).toBe(true);
    expect(t.remove).toHaveBeenCalledTimes(1);
    expect(t.warn).not.toHaveBeenCalled();
  });

  it('ends active SSE and removes shutdown listeners when the Node server stops', async () => {
    const on = vi.spyOn(process, 'on');
    const off = vi.spyOn(process, 'off');
    const t = start(); await t.reader.read();
    const signalHandler = on.mock.calls.find(call => call[0] === 'SIGTERM')![1] as () => void;
    const rejection = expect(t.reader.read()).rejects.toThrow('metrics server stopping');
    signalHandler(); signalHandler();
    await rejection; await flush();
    expect(t.socket.closeCalls).toBe(1);
    expect(t.socket.returned).toBe(true);
    expect(off).toHaveBeenCalledWith('SIGTERM', signalHandler);
    expect(off).toHaveBeenCalledWith('SIGINT', signalHandler);
    expect(t.warn).not.toHaveBeenCalled();
  });

  it('delivers one event exactly once to a reader already waiting for it', async () => {
    const t = start();
    try {
      await t.reader.read();
      const first = t.reader.read();
      t.socket.frame([fixture()]);
      expect((await first).done).toBe(false);
      await flush();
      let duplicated = false;
      const next = t.reader.read().then(result => { duplicated = !result.done; return result; });
      await flush();
      expect(duplicated).toBe(false);
      await t.reader.cancel(); await next;
    } finally { await t.reader.cancel(); }
  });

  it('uses only one process shutdown listener pair for many independent requests', async () => {
    const beforeInt = process.listenerCount('SIGINT');
    const beforeTerm = process.listenerCount('SIGTERM');
    const requests = Array.from({ length: 20 }, () => start());
    try {
      expect(process.listenerCount('SIGINT')).toBe(beforeInt + 1);
      expect(process.listenerCount('SIGTERM')).toBe(beforeTerm + 1);
      expect(new Set(requests.map(request => request.socket)).size).toBe(20);
    } finally { await Promise.all(requests.map(request => request.reader.cancel())); }
    expect(process.listenerCount('SIGINT')).toBe(beforeInt);
    expect(process.listenerCount('SIGTERM')).toBe(beforeTerm);
    for (const request of requests) expect(request.socket.closeCalls).toBe(1);
  });

  it('does not create a SUB for an already aborted request', () => {
    const controller = new AbortController(); controller.abort();
    const createSubscriber = vi.fn();
    expect(createMetricsResponse(controller.signal, { createSubscriber }).status).toBe(499);
    expect(createSubscriber).not.toHaveBeenCalled();
  });

  it('handles abort during listener registration and constructor delivery', async () => {
    const early = new AbortController();
    const original = early.signal.addEventListener.bind(early.signal);
    vi.spyOn(early.signal, 'addEventListener').mockImplementation((...args) => { original(...args); early.abort(); });
    const factory = vi.fn();
    expect(createMetricsResponse(early.signal, { createSubscriber: factory }).status).toBe(499);
    expect(factory).not.toHaveBeenCalled();
    const late = new AbortController(); const socket = new FakeSubscriber();
    expect(createMetricsResponse(late.signal, { createSubscriber: () => { late.abort(); return socket; } }).status).toBe(499);
    expect(socket.closeCalls).toBe(1);
  });

  it.each(['constructor', 'conflate', 'linger', 'subscribe', 'connect', 'response'])('cleans partial setup failure at %s', step => {
    const abort = new AbortController(); const socket = new FakeSubscriber(); const warn = vi.fn();
    const remove = vi.spyOn(abort.signal, 'removeEventListener');
    socket.failAt = step;
    if (step === 'conflate' || step === 'linger') {
      Object.defineProperty(socket, step, { set() { throw new Error(`${step} failure`); } });
    }
    if (step === 'response') vi.stubGlobal('Response', class { constructor() { throw new Error('response failure'); } });
    try {
      expect(() => createMetricsResponse(abort.signal, {
        createSubscriber: () => { if (step === 'constructor') throw new Error('constructor failure'); return socket; }, warn,
      })).toThrow(`${step} failure`);
      expect(socket.closeCalls).toBe(step === 'constructor' ? 0 : 1);
      expect(remove).toHaveBeenCalledTimes(1);
    } finally { vi.unstubAllGlobals(); }
  });

  it.each(['error', 'end'])('unexpected receive %s errors the stream and cleans up', async mode => {
    const t = start(); await t.reader.read();
    const pending = t.reader.read();
    const rejection = expect(pending).rejects.toThrow(mode === 'error' ? 'receive failure' : 'unexpectedly');
    if (mode === 'error') t.socket.fail(new Error('receive failure')); else t.socket.end();
    await rejection; await flush();
    expect(t.socket.closeCalls).toBe(1);
    expect(t.remove).toHaveBeenCalledTimes(1);
    expect(t.warn.mock.calls[0][0]).toContain(mode === 'error' ? 'receive-error' : 'unexpected-end');
  });

  it('preserves the first receive error when secondary cleanup fails', async () => {
    const t = start(); t.socket.failAt = 'close'; await t.reader.read();
    const rejection = expect(t.reader.read()).rejects.toThrow('first failure');
    t.socket.fail(new Error('first failure'));
    await rejection; await flush();
    expect(t.warn.mock.calls[0][0]).toContain('receive-error');
    expect(t.warn.mock.calls[1][0]).toContain('cleanup failed');
    expect(t.socket.closeCalls).toBe(1);
    expect(t.remove).toHaveBeenCalledTimes(1);
  });

  it('ignores a delivered frame after cancellation and creates no unhandled rejection', async () => {
    const unhandled = vi.fn(); process.on('unhandledRejection', unhandled);
    try {
      const t = start(); await t.reader.read();
      t.socket.frame([fixture()]);
      t.abort.abort(); await t.reader.cancel(); await flush();
      expect(t.socket.closeCalls).toBe(1);
      expect(unhandled).not.toHaveBeenCalled();
    } finally { process.off('unhandledRejection', unhandled); }
  });

  it('native zeromq close truly ends the pending async receive', async () => {
    const abort = new AbortController(); let native: Subscriber | undefined;
    const response = createMetricsResponse(abort.signal, {
      endpoint: `ipc:///tmp/qbook-v4-ui-no-publisher-${process.pid}.sock`,
      createSubscriber: () => (native = new Subscriber()),
    });
    const reader = response.body!.getReader();
    await reader.read();
    await reader.cancel(); // 必须等待真实 iterator 结束，不只是设置 closed。
    expect(native!.closed).toBe(true);
  });
});

describe('server endpoint configuration', () => {
  it('defaults only when absent and accepts configured TCP / IPC endpoints', () => {
    expect(metricsEndpoint(undefined)).toBe('tcp://127.0.0.1:5556');
    expect(metricsEndpoint('tcp://[::1]:5556')).toBe('tcp://[::1]:5556');
    expect(metricsEndpoint('ipc:///tmp/qbook-metrics.sock')).toBe('ipc:///tmp/qbook-metrics.sock');
  });
  it.each(['', ' ', 'http://localhost:5556', 'tcp://localhost', 'tcp://localhost:0', 'tcp://localhost:65536', 'tcp://u:p@localhost:5556', 'tcp://localhost:5556?q=1', 'ipc://relative'])('rejects %j', endpoint => {
    expect(() => metricsEndpoint(endpoint)).toThrow();
  });
});
