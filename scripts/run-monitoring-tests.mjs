import { access, readFile, mkdir, mkdtemp, writeFile } from "node:fs/promises";
import { constants } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";
import { spawn } from "node:child_process";

// 只编排本次构建的测试程序；不构建 UI，不猜测旧二进制位置。
const root = fileURLToPath(new URL("../", import.meta.url));
const runtimeFile = path.join(root, "build/debug/tests/runtime-paths.json");
const runtime = JSON.parse(await readFile(runtimeFile, "utf8"));
for (const key of ["qbook", "serviceTestDriver", "fixturePublisher", "fixtureGenerator", "parquetVerify"])
  await access(runtime[key], constants.X_OK);
await access(path.join(runtime.metricsFixtures, "layout.bin"));
const period = process.env.QBOOK_E2E_SAMPLE_PERIOD_MS;
if (period !== "100" && period !== "2000")
  throw new Error("QBOOK_E2E_SAMPLE_PERIOD_MS must explicitly be 100 or 2000");
await access(path.join(root, "ui/.next/BUILD_ID"));
const baseOutput = path.join(root, `build/v4-browser-${period}`);
await mkdir(baseOutput, { recursive: true });
const output = await mkdtemp(path.join(baseOutput, "run-"));
const child = spawn(process.execPath,
  [path.join(root, "ui/node_modules/@playwright/test/cli.js"), "test"], {
    cwd: path.join(root, "ui"), stdio: "inherit",
    env: { ...process.env, QBOOK_RUNTIME_PATHS: runtimeFile, QBOOK_E2E_OUTPUT: output },
  });
const forward = (signal) => { if (child.exitCode === null) child.kill(signal); };
const interrupt = () => forward("SIGINT");
const terminate = () => forward("SIGTERM");
process.on("SIGINT", interrupt);
process.on("SIGTERM", terminate);
const result = await new Promise((resolve, reject) => {
  child.once("error", reject);
  child.once("exit", (code, signal) => resolve({ code, signal }));
});
process.removeListener("SIGINT", interrupt);
process.removeListener("SIGTERM", terminate);
await writeFile(path.join(output, "execution.json"), JSON.stringify({ period, ...result }, null, 2) + "\n");
process.exitCode = result.code === 0 && result.signal === null ? 0 : 1;
