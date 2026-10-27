"use client";

import { useEffect, useState } from "react";
import { parseSample, type MetricsSample } from "../lib/metrics";
import { createMonitorModel, formatMiB, parseMonitorConfig, type CounterField,
  type MonitorConfig, type MonitorView } from "../lib/monitor";
import { MetricTrend } from "../components/MetricTrend";

type DisplayField = Exclude<keyof MetricsSample, "tsNs">;
type MetricDefinition = { field: DisplayField; label: string; unit: string; note: string; counter?: CounterField };
const GROUPS: readonly { title: string; subtitle: string; description: string; metrics: readonly MetricDefinition[] }[] = [
  { title: "Feed", subtitle: "接收与筛选", description: "收到消息、有效解码与成功接纳是不同边界。", metrics: [
    { field: "feedMessages", label: "已收消息", unit: "条", counter: "feedMessages", note: "包含随后被拒绝的消息" },
    { field: "feedBytes", label: "已收内容字节", unit: "bytes", counter: "feedBytes", note: "不含网络协议开销" },
    { field: "feedOrders", label: "已解码订单类", unit: "条", counter: "feedOrders", note: "含撤单；在成功入队前计数" },
    { field: "feedTrades", label: "已解码成交", unit: "条", counter: "feedTrades", note: "通过格式、字段与模式筛选" },
    { field: "feedLevels", label: "已解码价位", unit: "条", counter: "feedLevels", note: "通过格式、字段与模式筛选" },
    { field: "feedDropped", label: "输入拒绝", unit: "条", counter: "feedDropped", note: "不含业务忽略或停止时尚未入队的记录" },
  ] },
  { title: "Book", subtitle: "订单簿处理", description: "Apply 只更新订单簿；累计处理数包含业务忽略。", metrics: [
    { field: "bookEvents", label: "已完成 Apply", unit: "条", counter: "bookEvents", note: "含合法无变化和业务忽略" },
    { field: "bookApplyNs", label: "累计抽样耗时", unit: "ns", counter: "bookApplyNs", note: "只计被抽样的 Apply 调用" },
    { field: "bookSamples", label: "抽样次数", unit: "次", counter: "bookSamples", note: "第 1、1025、2049… 条事件" },
    { field: "bookInstruments", label: "已知品种", unit: "个", note: "状态量；在快照轮次完成时更新" },
    { field: "bookRestingOrders", label: "本地逐笔挂单数", unit: "笔", note: "L2 为 0，不代表真实委托数" },
    { field: "bookMemoryBytes", label: "订单簿内存估计", unit: "bytes", note: "状态量；不等于进程 RSS" },
  ] },
  { title: "Writer", subtitle: "文件追加", description: "仅 Add 成功后计数；文件完整性由正常 Close 后读回确认。", metrics: [
    { field: "writerOrders", label: "已追加订单类", unit: "条", counter: "writerOrders", note: "含撤单；订单与撤单分别保存文件" },
    { field: "writerTrades", label: "已追加成交", unit: "条", counter: "writerTrades", note: "Add 成功不等于安全落盘" },
    { field: "writerLevels", label: "已追加价位", unit: "条", counter: "writerLevels", note: "保存原始价位事件" },
    { field: "writerBooks", label: "已追加快照", unit: "条", counter: "writerBooks", note: "无新行情时仍可周期增长" },
  ] },
];
const STATE_TEXT = { connecting: "等待首个有效样本", live: "有效样本持续到达", stalled: "连接仍在，样本已过期", offline: "SSE 连接错误，等待自动重连" } as const;

function initialConfiguration(): { config: MonitorConfig | null; error: string | null } {
  try { return { config: parseMonitorConfig(process.env.NEXT_PUBLIC_METRICS_SAMPLE_PERIOD_MS), error: null }; }
  catch (error) { return { config: null, error: error instanceof Error ? error.message : "未知配置错误" }; }
}
function formatRate(value: number | null): string {
  if (value === null) return "—";
  if (value > 0 && value < 0.001) return value.toExponential(2);
  return value.toLocaleString("en-US", { useGrouping: false, maximumFractionDigits: 3 });
}

export default function MonitorPage() {
  const [{ config, error }] = useState(initialConfiguration);
  const [view, setView] = useState<MonitorView | null>(null);
  useEffect(() => {
    if (!config) return;
    const model = createMonitorModel(config);
    let disposed = false;
    const source = new EventSource("/api/metrics");
    const publish = () => { if (!disposed) setView(model.getView()); };
    const onOpen = () => {
      if (disposed) return;
      model.onOpen(performance.now());
      publish();
    };
    const onError = () => {
      if (disposed) return;
      model.onError(performance.now());
      publish();
    };
    const onMessage = (event: MessageEvent<string>) => {
      if (disposed) return;
      const arrivalMs = performance.now();
      try { model.onSample(parseSample(JSON.parse(event.data)), arrivalMs); }
      catch (error) { model.onInvalidSample(error instanceof Error ? error.message : "无法解析消息"); }
      publish();
    };
    source.addEventListener("open", onOpen);
    source.addEventListener("error", onError);
    source.addEventListener("message", onMessage);
    const timer = window.setInterval(() => {
      if (disposed) return;
      const now = performance.now();
      model.evaluateConnection(now);
      model.pruneHistory(now);
      publish();
    }, config.checkPeriodMs);
    publish();
    return () => {
      disposed = true;
      source.removeEventListener("open", onOpen);
      source.removeEventListener("error", onError);
      source.removeEventListener("message", onMessage);
      source.close();
      window.clearInterval(timer);
    };
  }, [config]);

  const state = view?.connection ?? "connecting";
  const sample = view?.sample ?? null;
  const oldValues = sample !== null && state !== "live";
  return <main>
    <header className="topbar">
      <a className="brand" href="/" aria-label="qbook 监控首页"><span className="brand-mark">q</span>qbook<span className="brand-divider" />monitor</a>
      <span className="readonly-label">只读运行监控</span>
    </header>
    <div className="workspace">
      <section className="overview" aria-labelledby="page-title">
        <div><p className="eyebrow">PIPELINE OBSERVABILITY</p><h1 id="page-title">采集运行概览</h1><p className="intro">从消息接收，到订单簿处理，再到文件追加。</p></div>
        <div className="connection-panel">
          <span className={`status-badge ${state}`} role="status"><span className="status-dot" /><span data-testid="connection-state">{state}</span></span>
          <span className="connection-description">{STATE_TEXT[state]}</span>
        </div>
      </section>
      {error ? <section className="configuration-error" role="alert"><h2>监控配置错误</h2><p>{error}</p><p>修正构建参数并重新构建 UI；当前不建立订阅或估算速率。</p></section> : <>
        <div className="configuration-strip">
          <span>预期周期 <strong data-testid="sample-period">{config!.expectedSamplePeriodMs} ms</strong></span>
          <span>速率窗口 <strong data-testid="rate-window">{config!.rateWindowMs} ms</strong></span>
          <span>停滞阈值 <strong data-testid="stall-threshold">{config!.stallAfterMs} ms</strong></span>
          <span>状态检查 <strong>500 ms</strong></span>
          <span className="config-note">构建期配置须与 C++ sample_period 一致</span>
        </div>
        <div className={`sample-note ${oldValues ? "is-old" : ""}`}>
          <span>{oldValues ? "以下保留最近有效样本的旧值，不能代表当前业务状态。" : sample ? "累计值保留完整 64 位整数精度。" : "尚未收到有效样本；空白不表示业务量为零。"}</span>
          <span>采样时间 ts_ns <code data-testid="sample-timestamp">{sample?.tsNs.toString() ?? "—"}</code></span>
        </div>
        {view?.diagnostic && <p className="diagnostic" role="alert">{view.diagnostic}</p>}
        {Boolean(view?.unsafeFields.length) && <p className="diagnostic">超出绘图安全范围：{view!.unsafeFields.join("、")}。保留准确累计值，相关图值留空。</p>}
        <MetricTrend points={view?.history ?? []} nowMs={view?.nowMs ?? 0} downsampled={view?.downsampled ?? false} />
        {GROUPS.map((group, groupIndex) => <section className={`metric-section stage-${group.title.toLowerCase()}`} key={group.title} aria-labelledby={`heading-${group.title}`}>
          <div className="section-heading">
            <div className="stage-title"><span className="stage-number">0{groupIndex + 1}</span><h2 id={`heading-${group.title}`}>{group.title} <span>{group.subtitle}</span></h2></div>
            <p className="section-meta">{group.description}</p>
          </div>
          <div className={`metric-grid ${group.title === "Writer" ? "writer-grid" : ""}`}>
            {group.metrics.map((metric) => <article className="metric-card" key={metric.field}>
              <div className="metric-label"><h3>{metric.label}</h3><span>{metric.counter ? "累计" : "状态"}</span></div>
              <div className="metric-value"><strong data-testid={`metric-${metric.field}`}>{sample?.[metric.field].toString() ?? "—"}</strong><span>{metric.unit}</span></div>
              {metric.counter ? <div className="rate-line"><span data-testid={`rate-${metric.field}`}>{formatRate(view?.rates[metric.counter] ?? null)}</span><span>{metric.unit} / s</span>{view?.rates[metric.counter] == null && <small>{view?.unsafeFields.includes(metric.counter) ? "超出绘图安全范围" : "积累数据中"}</small>}</div>
                : <div className="rate-line gauge-note">{metric.field === "bookMemoryBytes" && sample ? `${formatMiB(sample.bookMemoryBytes)} MiB` : "快照轮次观察值"}</div>}
              <p>{metric.note}</p>
            </article>)}
          </div>
          {group.title === "Book" && <div className="apply-panel"><div><h3>近期抽样 Apply 均值</h3><div className="apply-value"><strong data-testid="apply-mean">{view?.applyMean.text ?? "无抽样数据"}</strong>{view?.applyMean.text !== null && view?.applyMean.text !== undefined && <span>ns</span>}</div></div><p>同一窗口内 Δ耗时 / Δ样本数，最多显示三位小数。独立原子字段的近期估计；不含队列等待、网络或文件写入，不是 P99 或端到端延迟。</p></div>}
        </section>)}
        <footer className="footer-notes">
          <p><strong>观测边界</strong> <span data-testid="baseline-reason">{view?.baselineReason ?? "等待首个有效样本"}</span>。接收、解码、Apply、Add 与 Close 是不同完成边界，单帧不保证跨字段事务一致。</p>
          <p>Writer Add 不等于落盘、fsync 或断电安全；正常停止后的文件读回才是保存证据。监控允许遗漏中间样本，无历史补发，也不保证最终帧。</p>
          <p>样本缺失只能说明监控状态未知，不能证明核心服务已停止。</p>
          <p>累计量下降或长间隔会重建基线；协议没有运行 ID，极短重启可能无法识别。页面仅观察，不提供交易或服务控制。</p>
        </footer>
      </>}
    </div>
  </main>;
}
