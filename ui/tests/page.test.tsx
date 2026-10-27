// @vitest-environment jsdom
import { StrictMode } from "react";
import { act, cleanup, render, screen } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import MonitorPage from "../app/page";
import { MetricTrend } from "../components/MetricTrend";
import { METRIC_FIELDS, type MetricsDto, type MetricsSample } from "../lib/metrics";
import { createMonitorModel, parseMonitorConfig } from "../lib/monitor";

class TestEventSource extends EventTarget {
  static instances: TestEventSource[] = [];
  close = vi.fn();
  constructor(public readonly url: string) {
    super();
    TestEventSource.instances.push(this);
  }
  open() { this.dispatchEvent(new Event("open")); }
  fail() { this.dispatchEvent(new Event("error")); }
  send(overrides: Partial<MetricsDto> = {}) {
    const dto = { ...Object.fromEntries(METRIC_FIELDS.map((field) => [field, "0"])), ...overrides };
    this.dispatchEvent(new MessageEvent("message", { data: JSON.stringify(dto) }));
  }
}
let now = 0;
beforeEach(() => {
  vi.useFakeTimers();
  now = 0;
  vi.spyOn(performance, "now").mockImplementation(() => now);
  vi.stubEnv("NEXT_PUBLIC_METRICS_SAMPLE_PERIOD_MS", undefined);
  TestEventSource.instances = [];
  vi.stubGlobal("EventSource", TestEventSource);
});
afterEach(() => {
  cleanup();
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
  vi.unstubAllEnvs();
  vi.useRealTimers();
});

describe("monitor page", () => {
  it("displays all 16 metrics, placeholders, explicit units and ownership limits", () => {
    render(<MonitorPage />);
    expect(screen.getByTestId("connection-state").textContent).toBe("connecting");
    expect(screen.getByTestId("sample-period").textContent).toBe("100 ms");
    for (const field of METRIC_FIELDS.filter((field) => field !== "tsNs")) {
      expect(screen.getByTestId(`metric-${field}`).textContent).toBe("—");
    }
    expect(screen.getByTestId("apply-mean").textContent).toBe("无抽样数据");
    expect(screen.getByText("L2 为 0，不代表真实委托数")).toBeTruthy();
    expect(screen.getByText("订单簿内存估计")).toBeTruthy();
    expect(screen.getByText("含撤单；订单与撤单分别保存文件")).toBeTruthy();
    expect(screen.getByText(/不是 P99 或端到端延迟/)).toBeTruthy();
    expect(screen.getByText(/Writer Add 不等于落盘/)).toBeTruthy();
    expect(document.body.textContent).not.toMatch(/NaN|Infinity/);
    expect(TestEventSource.instances).toHaveLength(1);
    expect(TestEventSource.instances[0].url).toBe("/api/metrics");
    expect(vi.getTimerCount()).toBe(1);
  });
  it("preserves exact display values and calculates the true initial interval", () => {
    render(<MonitorPage />);
    const source = TestEventSource.instances[0];
    act(() => { source.open(); source.send({ bookEvents: "9007199254740993" }); });
    expect(screen.getByTestId("metric-bookEvents").textContent).toBe("9007199254740993");
    expect(screen.getByTestId("rate-bookEvents").textContent).toBe("—");
    now = 200;
    act(() => source.send({ bookEvents: "9007199254741003", bookSamples: "1" }));
    expect(screen.getByTestId("metric-bookEvents").textContent).toBe("9007199254741003");
    expect(screen.getByTestId("rate-bookEvents").textContent).toBe("50");
    expect(screen.getByTestId("apply-mean").textContent).toBe("0");
    expect(screen.getByTestId("connection-state").textContent).toBe("live");
  });
  it("invalid DTOs never refresh last valid time; timer changes status without adding points", () => {
    render(<MonitorPage />);
    const source = TestEventSource.instances[0];
    act(() => { source.open(); source.send(); });
    expect(screen.getByTestId("history-chart").getAttribute("data-points")).toBe("1");
    now = 1600;
    act(() => source.dispatchEvent(new MessageEvent("message", { data: "{\"bookEvents\":\"5\"}" })));
    expect(screen.getByRole("alert").textContent).toContain("已忽略非法样本");
    act(() => vi.advanceTimersByTime(500));
    expect(screen.getByTestId("connection-state").textContent).toBe("stalled");
    expect(screen.getByTestId("history-chart").getAttribute("data-points")).toBe("1");
    expect(screen.getByText(/以下保留最近有效样本的旧值/)).toBeTruthy();
    act(() => source.fail());
    expect(screen.getByTestId("connection-state").textContent).toBe("offline");
    now = 1700;
    act(() => source.open());
    expect(screen.getByTestId("connection-state").textContent).toBe("connecting");
    act(() => source.send({ bookEvents: "100" }));
    expect(screen.getByTestId("connection-state").textContent).toBe("live");
    expect(screen.getByTestId("rate-bookEvents").textContent).toBe("—");
  });
  it("pairs the 2000 ms build configuration and shows invalid configuration explicitly", () => {
    vi.stubEnv("NEXT_PUBLIC_METRICS_SAMPLE_PERIOD_MS", "2000");
    const mounted = render(<MonitorPage />);
    expect(screen.getByTestId("sample-period").textContent).toBe("2000 ms");
    expect(screen.getByTestId("rate-window").textContent).toBe("4000 ms");
    expect(screen.getByTestId("stall-threshold").textContent).toBe("6000 ms");
    mounted.unmount();
    vi.stubEnv("NEXT_PUBLIC_METRICS_SAMPLE_PERIOD_MS", "0");
    render(<MonitorPage />);
    expect(screen.getByRole("alert").textContent).toContain("监控配置错误");
    expect(TestEventSource.instances).toHaveLength(1);
    expect(vi.getTimerCount()).toBe(0);
  });
  it("cleans up source, listeners and timer, and ignores a queued callback after unmount", () => {
    const add = vi.spyOn(TestEventSource.prototype, "addEventListener");
    const mounted = render(<MonitorPage />);
    const first = TestEventSource.instances[0];
    const remove = vi.spyOn(first, "removeEventListener");
    const oldCallback = add.mock.calls.find(([event]) => event === "message")![1] as EventListener;
    mounted.unmount();
    expect(first.close).toHaveBeenCalledTimes(1);
    expect(remove.mock.calls.map(([event]) => event).sort()).toEqual(["error", "message", "open"]);
    expect(vi.getTimerCount()).toBe(0);
    render(<MonitorPage />);
    const current = TestEventSource.instances[1];
    act(() => { current.open(); current.send({ bookEvents: "42" }); });
    act(() => oldCallback(new MessageEvent("message", { data: "{}" })));
    expect(screen.queryByRole("alert")).toBeNull();
    expect(screen.getByTestId("metric-bookEvents").textContent).toBe("42");
    expect(current.close).not.toHaveBeenCalled();
    expect(vi.getTimerCount()).toBe(1);
  });
  it("StrictMode setup/cleanup/setup leaves exactly one live source and timer", () => {
    const mounted = render(<StrictMode><MonitorPage /></StrictMode>);
    expect(TestEventSource.instances).toHaveLength(2);
    expect(TestEventSource.instances[0].close).toHaveBeenCalledTimes(1);
    expect(TestEventSource.instances[1].close).not.toHaveBeenCalled();
    expect(vi.getTimerCount()).toBe(1);
    act(() => TestEventSource.instances[1].fail());
    mounted.unmount();
    expect(TestEventSource.instances[1].close).toHaveBeenCalledTimes(1);
    expect(vi.getTimerCount()).toBe(0);
  });
  it("plots actual time spacing and separates resets rather than joining gaps", () => {
    const model = createMonitorModel(parseMonitorConfig());
    const blank = Object.fromEntries(METRIC_FIELDS.map((field) => [field, 0n])) as MetricsSample;
    model.onSample(blank, 0);
    model.onSample({ ...blank, feedMessages: 10n }, 200);
    model.onSample({ ...blank, feedMessages: 20n }, 800);
    model.onSample({ ...blank, feedMessages: 1n }, 900);
    model.onSample({ ...blank, feedMessages: 2n }, 950);
    model.onSample({ ...blank, feedMessages: 3n }, 1000);
    const view = model.getView();
    const mounted = render(<MetricTrend points={view.history} nowMs={1000} downsampled={false} />);
    const paths = mounted.container.querySelectorAll("path[data-trend-segment]");
    expect(paths).toHaveLength(6); // 三条曲线，各分为两个有效段。
    expect(paths[0].getAttribute("d")).toBe("M932.16,20.00 L941.04,97.00");
    expect(paths[1].getAttribute("d")).toBe("M943.26,112.40 L944.00,112.40");
    expect(screen.getByTestId("history-chart").getAttribute("data-points")).toBe("6");
    expect(screen.getByText(/图线不跨越基线变化/)).toBeTruthy();
  });
});
