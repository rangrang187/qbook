import { test, expect } from "@playwright/test";
import { readFile, writeFile } from "node:fs/promises";
import http from "node:http";
import path from "node:path";
import { cleanupProcesses, coreScenario, finishCore, nextServer, outputRoot,
  period, publisher, resources, runtime, until } from "./e2e-support";

test.afterEach(async () => { await cleanupProcesses(); });

test("真实 C++ 指标经 Node SSE 到浏览器：精度、四状态及请求释放", async ({ page, browser }) => {
  const source = await publisher("metrics", "fixed-metrics");
  let next = await nextServer(source.endpoint);
  const errors: string[] = [];
  page.on("pageerror", error => errors.push(error.message));
  await page.goto(next.url);
  await expect(page.getByTestId("sample-period")).toHaveText(`${period} ms`);
  await expect(page.getByTestId("connection-state")).toHaveText("connecting");
  await until(() => source.child.subscriptions(), n => n === 1, "one request owns one SUB");
  await source.child.command("resume");
  await expect(page.getByTestId("connection-state")).toHaveText("live");
  await expect(page.getByTestId("metric-bookEvents")).toHaveText("9007199254740993");
  await source.child.command("use large-b");
  await expect(page.getByTestId("metric-bookEvents")).toHaveText("9007199254741003");
  await expect(page.getByTestId("apply-mean")).toHaveText("100");
  await expect(page.getByTestId("rate-bookEvents")).toHaveText("0", { timeout: Math.max(10_000, period * 5) });
  await expect(page.getByTestId("connection-state")).toHaveText("live");

  // 所有字段来自 C++ 样本，期望由独立手写 JSON 读取；页面不注入业务状态。
  for (const fixture of ["layout", "bounds-min", "bounds-max", "zero-samples"]) {
    await source.child.command(`use ${fixture}`);
    const expected: Record<string, string> = JSON.parse(await readFile(
      path.join(runtime.metricsFixtures, `${fixture}.expected.json`), "utf8"));
    for (const [field, value] of Object.entries(expected))
      await expect(page.getByTestId(field === "tsNs" ? "sample-timestamp" : `metric-${field}`)).toHaveText(value);
  }
  await expect(page.getByTestId("apply-mean")).toHaveText("无抽样数据");
  await source.child.command("pause");
  await expect(page.getByTestId("connection-state")).toHaveText("stalled", {
    timeout: Math.max(6000, period * 4 + 2000),
  });
  await source.child.command("malformed");
  await expect(page.getByTestId("connection-state")).toHaveText("stalled");
  const points = await page.getByTestId("history-chart").getAttribute("data-points");
  await next.child.stop();
  await expect(page.getByTestId("connection-state")).toHaveText("offline");
  await until(() => source.child.subscriptions(), n => n === 0, "Node shutdown closes subscription");
  next = await nextServer(source.endpoint, next.port);
  await expect(page.getByTestId("connection-state")).toHaveText("connecting");
  await source.child.command("resume");
  await expect(page.getByTestId("connection-state")).toHaveText("live");
  await page.close();
  await until(() => source.child.subscriptions(), n => n === 0, "page unload closes native SUB");

  const resourceSamples = [await resources(next.child)];
  for (let i = 0; i < 3; ++i) {
    const observer = await browser.newPage();
    await observer.goto(next.url);
    await expect(observer.getByTestId("connection-state")).toHaveText("live");
    await observer.close();
    await until(() => source.child.subscriptions(), n => n === 0, "repeated page close releases request");
    resourceSamples.push(await resources(next.child));
  }

  // 真实 HTTP 慢读者：读取暂停时同一发布端仍发帧；恢复能取得近期值，取消释放原生订阅。
  const observer = await browser.newPage();
  const request = http.get(`${next.url}/api/metrics`, { signal: AbortSignal.timeout(30_000) });
  let response: http.IncomingMessage | undefined;
  let responseError: Error | null = null;
  request.on("error", error => { responseError = error; });
  try {
    request.once("response", value => { response = value; value.pause(); });
    await until(() => { if (responseError) throw responseError; return response; },
      value => value !== undefined, "SSE response headers");
    await until(() => source.child.subscriptions(), n => n === 1, "slow HTTP client owns subscription");
    await observer.goto(next.url);
    await expect(observer.getByTestId("connection-state")).toHaveText("live");
    await source.child.command("use bounds-max");
    await expect(observer.getByTestId("metric-bookEvents")).toHaveText("18446744073709551615");
    let received = "";
    response!.on("data", chunk => { received = (received + chunk.toString()).slice(-64_000); });
    response!.resume();
    await until(() => { if (responseError) throw responseError; return received; },
      value => value.includes('"bookEvents":"18446744073709551615"'),
      "slow reader resumes with current exact metric");
  } finally {
    request.destroy(); response?.destroy();
    await observer.close();
    await until(() => response?.closed ?? request.destroyed, value => value, "HTTP close completed");
  }
  await until(() => source.child.subscriptions(), n => n === 0, "slow reader cancellation releases native SUB");
  expect(errors).toEqual([]);
  expect(next.child.output).not.toMatch(/unhandledRejection|UnhandledPromiseRejection/);
  await writeFile(path.join(outputRoot, "browser.json"), JSON.stringify({
    browser: browser.version(), samplePeriod: period, states: ["connecting", "live", "stalled", "offline"],
    exactBookEvents: "9007199254741003", fixturesChecked: 6, repeatedPageCleanup: 3,
    slowHttpCleanup: true, pointsBeforeTransportFailure: points,
    resourceSamples,
  }, null, 2) + "\n");
  await next.child.stop();
  await source.child.command("quit");
  await until(() => source.child.result, value => value !== null, "fixed metrics publisher quit");
  expect(source.child.result?.code).toBe(0);
});

for (const closeNode of [false, true]) {
  test(`真实四线程服务：关闭${closeNode ? "Node" : "页面"}后继续处理并读回五类文件`, async ({ page, browser }) => {
    const core = await coreScenario(closeNode ? "node-offline" : "page-offline");
    const next = await nextServer(core.endpoint);
    await page.goto(next.url);
    await expect(page.getByTestId("sample-period")).toHaveText(`${period} ms`);
    await expect(page.getByTestId("connection-state")).toHaveText("live");
    await core.feed.child.command("phase1");
    await core.service.wait(/driver_goal_reached/);
    const sentBytes = core.feed.child.output.match(/"command":"phase1","sent":6,"bytes":(\d+)/)?.[1];
    expect(sentBytes).toBe("438"); // 当前冻结布局的独立长度核对。
    const exact = {
      feedMessages: "6", feedBytes: sentBytes!, feedOrders: "5", feedTrades: "1", feedLevels: "0",
      feedDropped: "0", bookEvents: "6", bookSamples: "1", bookInstruments: "1",
      bookRestingOrders: "3", writerOrders: "5", writerTrades: "1", writerLevels: "0",
    };
    for (const [field, value] of Object.entries(exact))
      await expect(page.getByTestId(`metric-${field}`)).toHaveText(value);
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
    if (!closeNode) await page.screenshot({ path: path.join(outputRoot, "monitor-live.png"), fullPage: true });
    await expect(page.getByTestId("rate-bookEvents")).toHaveText("0", {
      timeout: Math.max(10_000, period * 5),
    });
    await expect(page.getByTestId("connection-state")).toHaveText("live");
    const before = BigInt((await page.getByTestId("metric-writerBooks").textContent())!);
    await expect.poll(async () => BigInt((await page.getByTestId("metric-writerBooks").textContent())!) > before)
      .toBe(true);
    if (closeNode) {
      await next.child.stop();
      await expect(page.getByTestId("connection-state")).toHaveText("offline");
    } else await page.close();
    await core.feed.child.command("phase2");
    // Writer 的 release/acquire 完成信号证明第 7 条 Add 后已出现快照；不是混合 relaxed 指标猜测。
    await core.service.wait(/driver_progress records_added=7 records_snapshotted=7/);
    if (!closeNode) {
      const reopened = await browser.newPage();
      await reopened.goto(next.url);
      await expect(reopened.getByTestId("connection-state")).toHaveText("live");
      await expect(reopened.getByTestId("metric-bookEvents")).toHaveText("7");
      await expect(reopened.getByTestId("metric-feedOrders")).toHaveText("6");
      await expect(reopened.getByTestId("metric-writerOrders")).toHaveText("6");
      await expect(reopened.getByTestId("metric-bookRestingOrders")).toHaveText("4");
      await reopened.close();
    }
    const result = await finishCore(core, "phase2", closeNode ? "SIGTERM" : "SIGINT");
    expect(result).toMatchObject({ verified: true, orders: 5, cancels: 1, trades: 1, levels: 0,
      bidPrice: 100, bidQty: 7, askPrice: 101, askQty: 6 });
    expect(result.books).toBeGreaterThan(0);
  });
}
