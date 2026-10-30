import test from "node:test";
import assert from "node:assert/strict";
import { mkdtemp, mkdir, writeFile, chmod, rm } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { loadMonitoringRuntime, resolveTestBuildDir } from "../scripts/support/monitoring-build.mjs";

async function fixture(t) {
  const root = await mkdtemp(path.join(os.tmpdir(), "qbook-monitoring-paths-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  const fixtures = path.join(root, "fixtures");
  await mkdir(fixtures);
  await writeFile(path.join(fixtures, "layout.bin"), Buffer.alloc(136));
  async function build(relative) {
    const directory = path.join(root, relative);
    await mkdir(path.join(directory, "tests"), { recursive: true });
    const executable = path.join(directory, "test-program");
    await writeFile(executable, "#!/bin/sh\nexit 0\n", { mode: 0o700 });
    const runtime = { metricsFixtures: fixtures };
    for (const key of ["qbook", "serviceTestDriver", "fixturePublisher", "fixtureGenerator", "parquetVerify"])
      runtime[key] = executable;
    const manifest = path.join(directory, "tests/runtime-paths.json");
    await writeFile(manifest, JSON.stringify(runtime));
    return { directory, executable, runtime, manifest };
  }
  return { root, build };
}

test("默认选择 debug；显式相对路径以仓库为根，绝对路径保持", async t => {
  const f = await fixture(t);
  const debug = await f.build("build/debug");
  const clean = await f.build("build/clean");
  assert.equal((await loadMonitoringRuntime(f.root)).buildDir, debug.directory);
  assert.equal((await loadMonitoringRuntime(f.root, "build/clean")).buildDir, clean.directory);
  assert.equal((await loadMonitoringRuntime(f.root, clean.directory)).buildDir, clean.directory);
});

test("明确选择缺失构建时即使 debug 可用也不回退", async t => {
  const f = await fixture(t);
  await f.build("build/debug");
  await assert.rejects(loadMonitoringRuntime(f.root, "build/missing"), /missing.*no fallback/);
});

test("拒绝引用旧构建程序的遗留清单", async t => {
  const f = await fixture(t);
  const old = await f.build("build/debug");
  const clean = await f.build("build/clean");
  await writeFile(clean.manifest, JSON.stringify({ ...clean.runtime, qbook: old.executable }));
  await assert.rejects(loadMonitoringRuntime(f.root, clean.directory), /does not belong/);
});

test("拒绝缺失或不可执行产物，不启动任何服务", async t => {
  const f = await fixture(t);
  const clean = await f.build("build/clean");
  await chmod(clean.executable, 0o600);
  await assert.rejects(loadMonitoringRuntime(f.root, clean.directory), /no fallback/);
  await rm(clean.executable);
  await assert.rejects(loadMonitoringRuntime(f.root, clean.directory), /no fallback/);
});

test("拒绝空环境变量、坏 JSON 和缺失键", async t => {
  const f = await fixture(t);
  for (const value of ["", " ", "\0"])
    assert.throws(() => resolveTestBuildDir(f.root, value), /non-empty/);
  const clean = await f.build("build/clean");
  for (const contents of ["{", "null", "{}"] ) {
    await writeFile(clean.manifest, contents);
    await assert.rejects(loadMonitoringRuntime(f.root, clean.directory), /no fallback/);
  }
});
