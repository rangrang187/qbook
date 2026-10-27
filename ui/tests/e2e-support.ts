import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { randomInt } from "node:crypto";
import { mkdir, readFile, readdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { setTimeout as delay } from "node:timers/promises";

export const period = Number(process.env.QBOOK_E2E_SAMPLE_PERIOD_MS);
if (period !== 100 && period !== 2000) throw new Error("explicit paired period required");
export const uiRoot = process.cwd();
export const outputRoot = process.env.QBOOK_E2E_OUTPUT!;
export const runtime: Record<string, string> = JSON.parse(
  await readFile(process.env.QBOOK_RUNTIME_PATHS!, "utf8"));
const active = new Set<ManagedProcess>();

export async function until<T>(read: () => T | Promise<T>, accept: (value: T) => boolean,
  label: string, timeout = 20_000): Promise<T> {
  const deadline = performance.now() + timeout;
  for (;;) {
    const value = await read();
    if (accept(value)) return value;
    if (performance.now() >= deadline) throw new Error(`deadline: ${label}`);
    await delay(25);
  }
}

export class ManagedProcess {
  readonly child: ChildProcessWithoutNullStreams;
  readonly exited: Promise<{ code: number | null; signal: string | null }>;
  output = "";
  result: { code: number | null; signal: string | null } | null = null;
  failure: Error | null = null;
  constructor(readonly name: string, executable: string, args: string[], cwd = uiRoot,
    env: Partial<NodeJS.ProcessEnv> = {}) {
    this.child = spawn(executable, args, { cwd, env: { ...process.env, ...env }, stdio: "pipe" });
    active.add(this);
    const capture = (chunk: Buffer) => {
      this.output += chunk.toString("utf8");
      if (this.output.length > 1_000_000) {
        this.failure = new Error(`${name}: output limit exceeded`);
        this.output = this.output.slice(-500_000);
        this.child.kill("SIGTERM");
      }
    };
    this.child.stdout.on("data", capture);
    this.child.stderr.on("data", capture);
    this.child.stdin.on("error", error => { this.failure = error; });
    this.exited = new Promise(resolve => {
      this.child.once("error", error => {
        this.failure = error;
        this.result = { code: -1, signal: null };
        active.delete(this);
        resolve(this.result);
      });
      // close 才保证 stdout/stderr 已排空；exit 只表示进程退出。
      this.child.once("close", (code, signal) => {
        this.result = { code, signal };
        active.delete(this);
        resolve(this.result);
      });
    });
  }
  async wait(pattern: RegExp, timeout = 20_000): Promise<RegExpMatchArray> {
    return until(() => {
      const match = this.output.match(pattern);
      if (match) return match;
      if (this.failure || this.result) throw new Error(`${this.name} ended: ${this.failure ?? JSON.stringify(this.result)}\n${this.output}`);
      return null;
    }, value => value !== null, `${this.name}: ${pattern}`, timeout) as Promise<RegExpMatchArray>;
  }
  async command(command: string) {
    const from = this.output.length;
    if (this.result || this.failure) throw new Error(`${this.name} not running`);
    this.child.stdin.write(command + "\n");
    await until(() => {
      if (this.failure) throw this.failure;
      return this.output.slice(from);
    }, value => value.includes(`"command":"${command}"`), `${this.name} command ${command}`);
  }
  subscriptions(): number {
    const all = [...this.output.matchAll(/"event":"subscription","active":(\d+)/g)];
    return all.length ? Number(all.at(-1)![1]) : 0;
  }
  async normalServiceStop(signal: "SIGINT" | "SIGTERM") {
    if (!this.result) this.child.kill(signal);
    await until(() => this.result, result => result !== null, `${this.name} normal stop`, 20_000);
    if (this.result?.code !== 0 || this.result.signal !== null)
      throw new Error(`${this.name}: failed normal Close\n${this.output}`);
  }
  async stop() {
    if (this.result) return;
    this.child.kill("SIGTERM");
    try { await until(() => this.result, value => value !== null, `${this.name} stop`, 10_000); }
    catch (error) {
      this.child.kill("SIGKILL");
      await this.exited;
      throw error;
    }
  }
}

export async function cleanupProcesses() {
  const results = await Promise.allSettled([...active].map(child => child.stop()));
  const failures = results.filter(result => result.status === "rejected");
  if (failures.length) throw new Error(`cleanup failed: ${JSON.stringify(failures)}`);
}

export async function resources(process: ManagedProcess) {
  const pid = process.child.pid;
  if (!pid || process.result) throw new Error("resource observation requires a live owned process");
  const status = await readFile(`/proc/${pid}/status`, "utf8");
  return { pid, rssKiB: Number(status.match(/^VmRSS:\s+(\d+)/m)?.[1]),
    fileDescriptors: (await readdir(`/proc/${pid}/fd`)).length };
}

export async function publisher(mode: "feed" | "metrics", name: string) {
  const child = new ManagedProcess(name, runtime.fixturePublisher, [mode, String(period)]);
  const ready = await child.wait(/"event":"ready","endpoint":"([^"]+)"/);
  return { child, endpoint: ready[1] };
}

export async function nextServer(endpoint: string, fixedPort?: number) {
  // 直接由真实 Next 进程绑定候选端口；冲突时有限重试，不先探测后假定端口仍归自己。
  for (let attempt = 0; attempt < 5; ++attempt) {
    const port = fixedPort ?? randomInt(20_000, 55_000);
    const child = new ManagedProcess("next", process.execPath,
      [path.join(uiRoot, "node_modules/next/dist/bin/next"), "start", "--hostname", "127.0.0.1", "--port", String(port)],
      uiRoot, { QBOOK_METRICS_ENDPOINT: endpoint, NEXT_TELEMETRY_DISABLED: "1" });
    try {
      await child.wait(/Ready in/);
      const url = `http://127.0.0.1:${port}`;
      const response = await fetch(url, { signal: AbortSignal.timeout(10_000) });
      if (!response.ok) throw new Error(`Next page HTTP ${response.status}`);
      await response.arrayBuffer();
      return { child, url, port };
    } catch (error) {
      await child.stop();
      if (!fixedPort && child.output.includes("EADDRINUSE")) continue;
      throw error;
    }
  }
  throw new Error("Next could not bind any candidate port");
}

export async function coreScenario(name: string) {
  const feed = await publisher("feed", `${name}-feed`);
  const cwd = path.join(outputRoot, name);
  await mkdir(path.join(cwd, "config"), { recursive: true });
  await writeFile(path.join(cwd, "config/config.yaml"), [
    "l2: false", `feed_endpoint: ${feed.endpoint}`, "metrics_endpoint: tcp://127.0.0.1:*",
    "feed_queue_capacity: 4", "record_queue_capacity: 4", "snapshot_period: 100",
    `sample_period: ${period}`, "batch_rows: 3", "out_dir: data_out", "",
  ].join("\n"));
  const service = new ManagedProcess(`${name}-service`, runtime.serviceTestDriver,
    ["monitor-goal", "4", "1", "1", "0", "1", "100", "7", "101", "6", "6", "0", "15"], cwd);
  const metrics = await service.wait(/metrics_bound[^\n]*(tcp:\/\/127\.0\.0\.1:\d+)/);
  await service.wait(/service_control_ready/);
  await service.wait(/writer_opened/);
  await until(() => feed.child.subscriptions(), n => n === 1, "Feed XPUB subscription");
  return { feed, service, endpoint: metrics[1], cwd };
}

export async function finishCore(core: Awaited<ReturnType<typeof coreScenario>>, phase: "phase1" | "phase2",
  signal: "SIGINT" | "SIGTERM") {
  await core.service.normalServiceStop(signal);
  const directories = await readdir(path.join(core.cwd, "data_out"), { withFileTypes: true });
  if (directories.length !== 1 || !directories[0].isDirectory()) throw new Error("expected one exclusive output directory");
  const verifier = new ManagedProcess("parquet-verify", runtime.parquetVerify,
    [path.join(core.cwd, "data_out", directories[0].name), phase]);
  await until(() => verifier.result, value => value !== null, "independent Parquet readback");
  if (verifier.result?.code !== 0) throw new Error(verifier.output);
  const result = JSON.parse(verifier.output.trim());
  await writeFile(path.join(core.cwd, "readback.json"), JSON.stringify(result, null, 2) + "\n");
  await core.feed.child.command("quit");
  await until(() => core.feed.child.result, value => value !== null, "publisher quit");
  if (core.feed.child.result?.code !== 0) throw new Error("publisher did not quit normally");
  return result;
}
