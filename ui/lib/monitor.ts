import type { MetricsSample } from "./metrics";
import { SlidingRate } from "./rate";

export const COUNTER_FIELDS = [
  "feedMessages", "feedBytes", "feedOrders", "feedTrades", "feedLevels", "feedDropped",
  "bookEvents", "bookApplyNs", "bookSamples",
  "writerOrders", "writerTrades", "writerLevels", "writerBooks",
] as const;
export const GAUGE_FIELDS = ["bookInstruments", "bookRestingOrders", "bookMemoryBytes"] as const;
export type CounterField = (typeof COUNTER_FIELDS)[number];
export type GaugeField = (typeof GAUGE_FIELDS)[number];
export type ConnectionState = "connecting" | "live" | "stalled" | "offline";
export type Rates = Readonly<Record<CounterField, number | null>>;

export interface MonitorConfig {
  readonly expectedSamplePeriodMs: number;
  readonly rateWindowMs: number;
  readonly stallAfterMs: number;
  readonly historyMs: 60000;
  readonly checkPeriodMs: 500;
  readonly maxHistoryPoints: 2000;
}

export function parseMonitorConfig(raw?: string): MonitorConfig {
  const text = raw === undefined ? "100" : raw;
  if (text.trim() !== text || !/^[1-9][0-9]*$/.test(text)) {
    throw new RangeError("NEXT_PUBLIC_METRICS_SAMPLE_PERIOD_MS 必须是正安全整数毫秒数");
  }
  const period = Number(text);
  const window = Math.max(1000, period * 2);
  const stall = Math.max(1500, period * 3);
  if (![period, window, stall].every((n) => Number.isSafeInteger(n) && n > 0)) {
    throw new RangeError("预期周期或派生窗口超出安全整数毫秒范围");
  }
  return Object.freeze({ expectedSamplePeriodMs: period, rateWindowMs: window,
    stallAfterMs: stall, historyMs: 60000, checkPeriodMs: 500, maxHistoryPoints: 2000 });
}

export interface MeanView {
  readonly text: string | null;
  readonly plotValue: number | null;
}
export interface HistoryPoint {
  readonly arrivalMs: number;
  readonly segmentId: number;
  readonly values: Rates;
  readonly applyMean: number | null;
}
export interface MonitorView {
  readonly config: MonitorConfig;
  readonly connection: ConnectionState;
  readonly connectionGeneration: number;
  readonly sample: Readonly<MetricsSample> | null;
  readonly lastArrivalMs: number | null;
  readonly nowMs: number;
  readonly coveredMs: number;
  readonly rates: Rates;
  readonly applyMean: MeanView;
  readonly history: readonly HistoryPoint[];
  readonly downsampled: boolean;
  readonly baselineReason: string;
  readonly diagnostic: string | null;
  readonly unsafeFields: readonly string[];
}

const MAX_SAFE = BigInt(Number.MAX_SAFE_INTEGER);
const emptyRates = (): Rates => Object.freeze(Object.fromEntries(
  COUNTER_FIELDS.map((field) => [field, null]),
) as Record<CounterField, number | null>);

// 商与余数保持整数精度；至多三位小数是显示精度，不是额外测量精度。
function scaledText(scaled: bigint): string {
  const fraction = (scaled % 1000n).toString().padStart(3, "0").replace(/0+$/, "");
  return `${scaled / 1000n}${fraction ? `.${fraction}` : ""}`;
}
export function formatMiB(bytes: bigint): string {
  if (bytes < 0n) throw new RangeError("内存字节数不可为负");
  return scaledText(bytes * 1000n / 1048576n);
}

/** 只由当前页面事件循环拥有；不读网络，不使用墙钟，也不在定时器中制造样本。 */
export function createMonitorModel(config: MonitorConfig) {
  const windows = Object.fromEntries(COUNTER_FIELDS.map((field) =>
    [field, new SlidingRate(config.rateWindowMs)])) as Record<CounterField, SlidingRate>;
  let previous: Readonly<MetricsSample> | null = null;
  let latest: Readonly<MetricsSample> | null = null;
  let lastArrival: number | null = null;
  let baselineStart: number | null = null;
  let lastOperation: number | null = null;
  let hasSample = false;
  let transportError = false;
  let generation = 0;
  let segmentId = 0;
  let connection: ConnectionState = "connecting";
  let rates = emptyRates();
  let mean: MeanView = Object.freeze({ text: null, plotValue: null });
  let coveredMs = 0;
  let history: HistoryPoint[] = [];
  let downsampled = false;
  let baselineReason = "等待首个有效样本";
  let diagnostic: string | null = null;
  let unsafeFields: string[] = [];

  function checkTime(now: number) {
    if (!Number.isFinite(now) || (lastOperation !== null && now < lastOperation)) {
      throw new RangeError("页面到达时间必须有限且不倒退");
    }
  }
  function clearBaseline() {
    for (const field of COUNTER_FIELDS) windows[field].reset();
    previous = null;
    baselineStart = null;
    coveredMs = 0;
    rates = emptyRates();
    mean = Object.freeze({ text: null, plotValue: null });
    unsafeFields = [];
  }
  function connectionAt(now: number): ConnectionState {
    if (transportError) return "offline";
    if (!hasSample || lastArrival === null) return "connecting";
    return now - lastArrival > config.stallAfterMs ? "stalled" : "live";
  }
  function trimHistory(now: number) {
    const boundary = now - config.historyMs;
    let head = 0;
    while (head < history.length && history[head].arrivalMs < boundary) ++head;
    if (head) history = history.slice(head);
    if (!history.length) downsampled = false;
    if (history.length > config.maxHistoryPoints) {
      const before = history;
      history = Array.from({ length: config.maxHistoryPoints }, (_, i) =>
        before[Math.floor(i * (before.length - 1) / (config.maxHistoryPoints - 1))]);
      downsampled = true;
    }
  }
  function establish(sample: MetricsSample, arrivalMs: number, reason: string) {
    clearBaseline();
    previous = Object.freeze({ ...sample });
    baselineStart = arrivalMs;
    ++segmentId;
    baselineReason = reason;
  }
  function finishSample(sample: MetricsSample, arrivalMs: number) {
    latest = Object.freeze({ ...sample });
    previous = latest;
    lastArrival = arrivalMs;
    lastOperation = arrivalMs;
    hasSample = true;
    connection = connectionAt(arrivalMs);
    history.push(Object.freeze({ arrivalMs, segmentId, values: rates, applyMean: mean.plotValue }));
    trimHistory(arrivalMs);
  }
  function resetBaseline(sample: MetricsSample, arrivalMs: number) {
    checkTime(arrivalMs);
    diagnostic = null;
    establish(sample, arrivalMs, "正在重建基线，积累数据中");
    finishSample(sample, arrivalMs);
  }
  function onSample(sample: MetricsSample, arrivalMs: number) {
    checkTime(arrivalMs);
    diagnostic = null;
    let reason: string | null = null;
    if (previous === null) reason = generation > 1 ? "连接已恢复，积累数据中" : "首个有效样本，积累数据中";
    else if (COUNTER_FIELDS.some((field) => sample[field] < previous![field])) {
      reason = "累计量下降，正在重建基线";
    } else if (lastArrival !== null && arrivalMs - lastArrival > config.rateWindowMs) {
      reason = "样本间隔超出当前窗口，正在重建基线";
    }
    if (reason !== null) {
      establish(sample, arrivalMs, reason);
    } else {
      // 先算完全部差值再写任何窗口；gauge 和服务时间戳不参加下降判断。
      const increments = COUNTER_FIELDS.map((field) => sample[field] - previous![field]);
      try {
        const totals = {} as Record<CounterField, bigint>;
        for (let i = 0; i < COUNTER_FIELDS.length; ++i) {
          const field = COUNTER_FIELDS[i];
          windows[field].push(arrivalMs, increments[i]);
          totals[field] = windows[field].total(arrivalMs);
        }
        coveredMs = Math.min(config.rateWindowMs, arrivalMs - baselineStart!);
        unsafeFields = [];
        const nextRates = {} as Record<CounterField, number | null>;
        for (const field of COUNTER_FIELDS) {
          let value: number | null = null;
          if (coveredMs > 0 && totals[field] <= MAX_SAFE) {
            const candidate = Number(totals[field]) * 1000 / coveredMs;
            if (Number.isFinite(candidate) && candidate <= Number.MAX_SAFE_INTEGER) value = candidate;
          }
          if (coveredMs > 0 && value === null) unsafeFields.push(field);
          nextRates[field] = value;
        }
        rates = Object.freeze(nextRates);
        const denominator = totals.bookSamples;
        if (denominator === 0n) mean = Object.freeze({ text: null, plotValue: null });
        else {
          const scaled = totals.bookApplyNs * 1000n / denominator;
          mean = Object.freeze({ text: scaledText(scaled), plotValue: scaled <= MAX_SAFE ? Number(scaled) / 1000 : null });
          if (scaled > MAX_SAFE) unsafeFields.push("applyMean");
        }
        baselineReason = coveredMs > 0 ? "按实际观测覆盖时间估计" : "到达时间尚无间隔，积累数据中";
      } catch (error) {
        // 不向展示层交付半组计算结果；错误仍可见，下一帧从整组新基线开始。
        establish(sample, arrivalMs, "计算失败，正在重建基线");
        diagnostic = `计算失败：${error instanceof Error ? error.message : "未知错误"}`;
      }
    }
    finishSample(sample, arrivalMs);
  }
  function onOpen(now: number) {
    checkTime(now);
    clearBaseline();
    hasSample = false;
    transportError = false;
    ++generation;
    lastOperation = now;
    connection = "connecting";
    baselineReason = "等待首个有效样本";
    diagnostic = null;
    trimHistory(now);
  }
  function onError(now: number) {
    checkTime(now);
    clearBaseline();
    hasSample = false;
    transportError = true;
    lastOperation = now;
    connection = "offline";
    baselineReason = "SSE 连接错误，等待自动重连";
    trimHistory(now);
  }
  function evaluateConnection(now: number): ConnectionState {
    checkTime(now);
    lastOperation = now;
    connection = connectionAt(now);
    return connection;
  }
  function pruneHistory(now: number) {
    checkTime(now);
    trimHistory(now);
    lastOperation = now;
  }
  function onInvalidSample(reason: string) {
    diagnostic = `已忽略非法样本：${reason.slice(0, 180)}`;
  }
  function getView(): MonitorView {
    return Object.freeze({ config, connection, connectionGeneration: generation,
      sample: latest, lastArrivalMs: lastArrival, nowMs: lastOperation ?? 0, coveredMs,
      rates, applyMean: mean, history: Object.freeze(history.slice()), downsampled,
      baselineReason, diagnostic, unsafeFields: Object.freeze(unsafeFields.slice()) });
  }
  return { onSample, resetBaseline, onOpen, onError, onInvalidSample,
    evaluateConnection, pruneHistory, getView };
}
