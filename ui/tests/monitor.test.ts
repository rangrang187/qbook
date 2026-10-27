import { describe, expect, it, vi } from "vitest";
import { METRIC_FIELDS, type MetricsSample } from "../lib/metrics";
import { COUNTER_FIELDS, GAUGE_FIELDS, createMonitorModel, formatMiB, parseMonitorConfig } from "../lib/monitor";
import { SlidingRate } from "../lib/rate";

const sample = (overrides: Partial<MetricsSample> = {}): MetricsSample => ({
  ...Object.fromEntries(METRIC_FIELDS.map((field) => [field, 0n])) as MetricsSample,
  ...overrides,
});
const model = (period?: string) => createMonitorModel(parseMonitorConfig(period));

describe("monitor configuration", () => {
  it("derives both paired periods and defaults only an absent value", () => {
    expect(parseMonitorConfig()).toEqual({ expectedSamplePeriodMs: 100, rateWindowMs: 1000,
      stallAfterMs: 1500, historyMs: 60000, checkPeriodMs: 500, maxHistoryPoints: 2000 });
    expect(parseMonitorConfig("2000")).toEqual({ expectedSamplePeriodMs: 2000, rateWindowMs: 4000,
      stallAfterMs: 6000, historyMs: 60000, checkPeriodMs: 500, maxHistoryPoints: 2000 });
  });
  it.each(["", "0", "-1", "1.5", "1e3", " 100", "100 ", "100\n", "100\r\n", "+100", "NaN", "Infinity",
    "9007199254740992", "3002399751580331"])("rejects invalid period %s including derived overflow", (value) => {
    expect(() => parseMonitorConfig(value)).toThrow(RangeError);
  });
  it("classifies all 16 metric fields exactly once", () => {
    expect(COUNTER_FIELDS).toHaveLength(13);
    expect(GAUGE_FIELDS).toHaveLength(3);
    expect([...COUNTER_FIELDS, ...GAUGE_FIELDS].sort()).toEqual(METRIC_FIELDS.filter((f) => f !== "tsNs").sort());
  });
});

describe("exact monitored calculations", () => {
  it("first establishes a baseline then divides bigint delta 10 by actual 200 ms", () => {
    const m = model();
    const first = sample({ bookEvents: 9007199254740993n });
    m.onSample(first, 0);
    expect(m.getView().rates.bookEvents).toBeNull();
    m.onSample(sample({ bookEvents: 9007199254741003n }), 200);
    expect(m.getView().coveredMs).toBe(200);
    expect(m.getView().rates.bookEvents).toBe(50);
    expect(m.getView().sample?.bookEvents).toBe(9007199254741003n);
    expect(first.bookEvents).toBe(9007199254740993n);
  });
  it("handles equal arrival times without division by zero, retaining increments", () => {
    const m = model();
    m.onSample(sample(), 0);
    m.onSample(sample({ feedMessages: 5n }), 0);
    expect(m.getView().rates.feedMessages).toBeNull();
    m.onSample(sample({ feedMessages: 7n }), 100);
    expect(m.getView().rates.feedMessages).toBe(70);
  });
  it.each(COUNTER_FIELDS)("counter %s falling resets the entire baseline", (field) => {
    const m = model();
    m.onSample(sample({ [field]: 100n, bookInstruments: 2n }), 0);
    m.onSample(sample({ [field]: 120n, bookInstruments: 2n }), 100);
    const segment = m.getView().history.at(-1)!.segmentId;
    m.onSample(sample({ [field]: 119n, bookInstruments: 2n }), 200);
    expect(Object.values(m.getView().rates)).toEqual(Array(13).fill(null));
    expect(m.getView().baselineReason).toContain("累计量下降");
    expect(m.getView().connection).toBe("live");
    expect(m.getView().history.at(-1)!.segmentId).toBeGreaterThan(segment);
  });
  it("gauge decreases and backwards system timestamps do not reset the baseline", () => {
    const m = model();
    m.onSample(sample({ tsNs: 100n, bookInstruments: 9n, bookRestingOrders: 8n, bookMemoryBytes: 7n }), 0);
    m.onSample(sample({ tsNs: -10n, bookInstruments: 3n, bookRestingOrders: 2n, bookMemoryBytes: 1n, bookEvents: 10n }), 200);
    expect(m.getView().rates.bookEvents).toBe(50);
    expect(m.getView().history[0].segmentId).toBe(m.getView().history[1].segmentId);
  });
  it("a gap equal to a window is valid, but a larger gap starts a new segment", () => {
    const m = model();
    m.onSample(sample(), 0);
    m.onSample(sample({ feedMessages: 10n }), 1000);
    expect(m.getView().rates.feedMessages).toBe(10);
    m.onSample(sample({ feedMessages: 100n }), 2001);
    expect(m.getView().rates.feedMessages).toBeNull();
    expect(m.getView().baselineReason).toContain("间隔超出");
    m.onSample(sample({ feedMessages: 110n }), 2201);
    expect(m.getView().rates.feedMessages).toBe(50);
  });
  it("zero increments expire prior traffic while keeping the connection live", () => {
    const m = model();
    m.onSample(sample(), 0);
    m.onSample(sample({ feedMessages: 10n }), 100);
    m.onSample(sample({ feedMessages: 10n }), 1000);
    expect(m.getView().rates.feedMessages).toBe(10);
    m.onSample(sample({ feedMessages: 10n }), 1100);
    expect(m.getView().rates.feedMessages).toBe(0);
    expect(m.getView().connection).toBe("live");
  });
  it("keeps exact counters when delta or the derived rate cannot be safely plotted", () => {
    const m = model();
    m.onSample(sample(), 0);
    m.onSample(sample({ bookEvents: 9007199254740993n, feedMessages: 9007199254740991n }), 1);
    expect(m.getView().sample?.bookEvents.toString()).toBe("9007199254740993");
    expect(m.getView().rates.bookEvents).toBeNull();
    expect(m.getView().rates.feedMessages).toBeNull();
    expect(m.getView().unsafeFields).toContain("bookEvents");
    expect(m.getView().unsafeFields).toContain("feedMessages");
  });
  it("does not invent a mean with zero denominator; zero numerator is a valid estimate", () => {
    const m = model();
    m.onSample(sample(), 0);
    m.onSample(sample({ bookSamples: 1n }), 100);
    expect(m.getView().applyMean).toEqual({ text: "0", plotValue: 0 });
    m.onSample(sample({ bookSamples: 1n, bookApplyNs: 10n }), 200);
    expect(m.getView().applyMean.text).toBe("10");
    m.onSample(sample({ bookSamples: 1n, bookApplyNs: 10n }), 1100);
    expect(m.getView().applyMean.text).toBeNull();
    expect(m.getView().diagnostic).toBeNull();
  });
  it("formats mean using integer division and safe display precision", () => {
    const m = model();
    m.onSample(sample(), 0);
    m.onSample(sample({ bookSamples: 3n, bookApplyNs: 10n }), 200);
    expect(m.getView().applyMean).toEqual({ text: "3.333", plotValue: 3.333 });
    const large = model();
    large.onSample(sample(), 0);
    large.onSample(sample({ bookSamples: 1n, bookApplyNs: 18446744073709551615n }), 200);
    expect(large.getView().applyMean.text).toBe("18446744073709551615");
    expect(large.getView().applyMean.plotValue).toBeNull();
    expect(large.getView().unsafeFields).toContain("applyMean");
    const ratio = model();
    ratio.onSample(sample(), 0);
    ratio.onSample(sample({ bookSamples: 9007199254740993n, bookApplyNs: 18014398509481986n }), 200);
    expect(ratio.getView().applyMean).toEqual({ text: "2", plotValue: 2 });
  });
  it("formats MiB without first rounding the original integer bytes", () => {
    expect(formatMiB(0n)).toBe("0");
    expect(formatMiB(1572864n)).toBe("1.5");
    expect(formatMiB(18446744073709551615n)).toBe("17592186044415.999");
    expect(() => formatMiB(-1n)).toThrow();
  });
  it("exposes immutable samples and history rather than its mutable working state", () => {
    const m = model();
    const input = sample({ bookEvents: 7n });
    m.onSample(input, 0);
    const before = m.getView();
    input.bookEvents = 100n;
    expect(before.sample?.bookEvents).toBe(7n);
    expect(() => (before.history as unknown[]).push({})).toThrow();
    expect(() => { (before.sample as MetricsSample).bookEvents = 2n; }).toThrow();
    m.onSample(sample({ bookEvents: 8n }), 100);
    expect(before.history).toHaveLength(1);
    expect(m.getView().rates.bookEvents).toBe(10);
  });
  it("an unexpected window failure reports a diagnostic and resets every field together", () => {
    const m = model();
    m.onSample(sample(), 0);
    const original = SlidingRate.prototype.push;
    let calls = 0;
    const intercepted = vi.spyOn(SlidingRate.prototype, "push").mockImplementation(function (this: SlidingRate, time, value) {
      if (++calls === 4) throw new Error("controlled window failure");
      original.call(this, time, value);
    });
    try {
      m.onSample(sample({ feedMessages: 10n, bookEvents: 30n }), 100);
      expect(calls).toBe(4);
      expect(m.getView().diagnostic).toContain("controlled window failure");
      expect(Object.values(m.getView().rates)).toEqual(Array(13).fill(null));
    } finally { intercepted.mockRestore(); }
    m.onSample(sample({ feedMessages: 12n, bookEvents: 31n }), 200);
    expect(m.getView().diagnostic).toBeNull();
    expect(m.getView().rates.feedMessages).toBe(20);
    expect(m.getView().rates.bookEvents).toBe(10);
  });
});

describe("connection and history", () => {
  it("distinguishes connecting, idle-live, stalled, offline, and a fresh reconnect", () => {
    const m = model();
    expect(m.evaluateConnection(0)).toBe("connecting");
    expect(m.evaluateConnection(100000)).toBe("connecting");
    m.onOpen(100000);
    m.onSample(sample(), 100000);
    m.onSample(sample(), 100100);
    expect(m.getView().connection).toBe("live");
    expect(m.getView().rates.feedMessages).toBe(0);
    expect(m.evaluateConnection(101600)).toBe("live");
    expect(m.evaluateConnection(101601)).toBe("stalled");
    m.onError(101602);
    expect(m.evaluateConnection(101603)).toBe("offline");
    expect(m.getView().sample).not.toBeNull();
    m.onOpen(101604);
    expect(m.getView().connection).toBe("connecting");
    m.onSample(sample({ bookEvents: 1000n }), 101605);
    expect(m.getView().connection).toBe("live");
    expect(m.getView().rates.bookEvents).toBeNull();
    expect(m.getView().connectionGeneration).toBe(2);
  });
  it("checks slow paired periods against 6000 ms, not a fixed 1500 ms", () => {
    const m = model("2000");
    m.onSample(sample(), 0);
    m.onSample(sample({ bookEvents: 4n }), 2000);
    expect(m.getView().rates.bookEvents).toBe(2);
    expect(m.evaluateConnection(8000)).toBe("live");
    expect(m.evaluateConnection(8001)).toBe("stalled");
  });
  it("invalid frames and timer checks do not refresh samples or create zero points", () => {
    const m = model();
    m.onSample(sample(), 0);
    m.onSample(sample({ bookEvents: 10n }), 100);
    m.onInvalidSample("缺字段");
    expect(m.evaluateConnection(1601)).toBe("stalled");
    m.pruneHistory(1601);
    expect(m.getView().lastArrivalMs).toBe(100);
    expect(m.getView().history).toHaveLength(2);
    expect(m.getView().rates.bookEvents).toBe(100);
    expect(m.getView().diagnostic).toContain("缺字段");
  });
  it("prunes using actual 60 seconds including the exact left boundary", () => {
    const m = model();
    m.onSample(sample(), 0);
    m.onSample(sample(), 100);
    m.pruneHistory(59999);
    expect(m.getView().history).toHaveLength(2);
    m.pruneHistory(60000);
    expect(m.getView().history).toHaveLength(2);
    m.pruneHistory(60001);
    expect(m.getView().history.map((point) => point.arrivalMs)).toEqual([100]);
    m.pruneHistory(60101);
    expect(m.getView().history).toHaveLength(0);
    expect(m.getView().sample).not.toBeNull();
  });
  it("bounds drawing history without discarding calculation inputs or segment IDs", () => {
    const m = model();
    for (let i = 0; i <= 4000; ++i) {
      m.onSample(sample({ bookEvents: BigInt(i) }), i / 10);
    }
    const view = m.getView();
    expect(view.history).toHaveLength(2000);
    expect(view.downsampled).toBe(true);
    expect(view.history[0].arrivalMs).toBe(0);
    expect(view.history.at(-1)?.arrivalMs).toBe(400);
    expect(view.sample?.bookEvents).toBe(4000n);
    expect(view.rates.bookEvents).toBe(10000);
    expect(view.history.every((p, i, all) => i === 0 || p.arrivalMs > all[i - 1].arrivalMs)).toBe(true);
    m.onError(401);
    m.onOpen(402);
    m.onSample(sample({ bookEvents: 5000n }), 403);
    expect(m.getView().history.at(-1)!.segmentId).not.toBe(view.history.at(-1)!.segmentId);
  });
  it("rejects backwards and non-finite operations before changing valid values", () => {
    const m = model();
    m.onSample(sample({ bookEvents: 5n }), 100);
    for (const time of [99, Infinity, NaN]) {
      expect(() => m.onSample(sample(), time)).toThrow(RangeError);
      expect(() => m.onOpen(time)).toThrow(RangeError);
      expect(() => m.onError(time)).toThrow(RangeError);
      expect(() => m.pruneHistory(time)).toThrow(RangeError);
      expect(() => m.evaluateConnection(time)).toThrow(RangeError);
      expect(() => m.resetBaseline(sample(), time)).toThrow(RangeError);
    }
    expect(m.getView().sample?.bookEvents).toBe(5n);
    expect(m.getView().history).toHaveLength(1);
  });
});
