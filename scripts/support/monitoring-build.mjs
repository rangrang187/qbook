import { access, readFile } from "node:fs/promises";
import { constants } from "node:fs";
import path from "node:path";

const executables = ["qbook", "serviceTestDriver", "fixturePublisher", "fixtureGenerator", "parquetVerify"];

// 此配置仅选择验收产物；相对路径始终相对于仓库根，不受 npm 的 cwd 影响。
export function resolveTestBuildDir(root, configured) {
  if (configured === undefined) return path.join(root, "build/debug");
  if (configured.trim() === "" || configured.includes("\0"))
    throw new Error("QBOOK_TEST_BUILD_DIR must be a non-empty build directory");
  return path.resolve(root, configured);
}

export async function loadMonitoringRuntime(root, configured) {
  const buildDir = resolveTestBuildDir(root, configured);
  const runtimeFile = path.join(buildDir, "tests/runtime-paths.json");
  let runtime;
  try {
    runtime = JSON.parse(await readFile(runtimeFile, "utf8"));
    if (runtime === null || typeof runtime !== "object" || Array.isArray(runtime))
      throw new Error("runtime paths must be an object");
    for (const key of executables) {
      const executable = runtime[key];
      if (typeof executable !== "string" || !path.isAbsolute(executable))
        throw new Error(`${key} must contain an absolute executable path`);
      const relative = path.relative(buildDir, executable);
      if (relative === "" || relative === ".." || relative.startsWith(`..${path.sep}`) || path.isAbsolute(relative))
        throw new Error(`${key} does not belong to selected build directory: ${executable}`);
      await access(executable, constants.X_OK);
    }
    if (typeof runtime.metricsFixtures !== "string" || !path.isAbsolute(runtime.metricsFixtures))
      throw new Error("metricsFixtures must contain an absolute fixture directory");
    await access(path.join(runtime.metricsFixtures, "layout.bin"), constants.R_OK);
  } catch (error) {
    throw new Error(`Selected monitoring build is unavailable (${runtimeFile}); no fallback: ${error.message}`, { cause: error });
  }
  return { buildDir, runtimeFile, runtime };
}
