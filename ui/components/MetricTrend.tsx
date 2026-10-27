import type { CounterField, HistoryPoint } from "../lib/monitor";

const SERIES: readonly { field: CounterField; label: string; color: string }[] = [
  { field: "feedMessages", label: "已收消息", color: "#3578cc" },
  { field: "bookEvents", label: "已完成 Apply", color: "#068578" },
  { field: "writerOrders", label: "已追加订单类（含撤单）", color: "#9a6ac5" },
];

export function MetricTrend({ points, nowMs, downsampled }: {
  points: readonly HistoryPoint[]; nowMs: number; downsampled: boolean;
}) {
  const left = 56, right = 944, top = 20, bottom = 174;
  const sampleSpanMs = points.length > 1 ? points[points.length - 1].arrivalMs - points[0].arrivalMs : 0;
  let maximum = 1;
  for (const point of points) {
    for (const { field } of SERIES) maximum = Math.max(maximum, point.values[field] ?? 0);
  }
  const x = (time: number) => left + (time - (nowMs - 60000)) / 60000 * (right - left);
  const y = (value: number) => bottom - value / maximum * (bottom - top);
  const paths: { key: string; path: string; color: string }[] = [];
  for (const series of SERIES) {
    let path = "";
    let segment: number | null = null;
    let sequence = 0;
    const flush = () => {
      if (path) paths.push({ key: `${series.field}-${sequence++}`, path, color: series.color });
      path = "";
    };
    for (const point of points) {
      const value = point.values[series.field];
      if (value === null || (segment !== null && point.segmentId !== segment)) flush();
      if (value !== null) path += `${path ? " L" : "M"}${x(point.arrivalMs).toFixed(2)},${y(value).toFixed(2)}`;
      segment = point.segmentId;
    }
    flush();
  }
  return <section className="trend-panel" aria-labelledby="trend-title">
    <div className="section-heading">
      <div><p className="eyebrow">RECENT ACTIVITY</p><h2 id="trend-title">最近 60 秒</h2></div>
      <span className="section-meta">浏览器单调到达时间 · 条 / s</span>
    </div>
    <div className="trend-legend">
      {SERIES.map((s) => <span key={s.field}><i style={{ background: s.color }} />{s.label}</span>)}
    </div>
    <div data-testid="history-chart" data-points={points.length} className="chart-wrap">
      <svg viewBox="0 0 968 218" role="img" aria-label="最近60秒事件速率；断档不连线">
        <title>按真实到达时间绘制的近期速率</title>
        {[0, 0.5, 1].map((fraction) => <g key={fraction}>
          <line x1={left} x2={right} y1={y(maximum * fraction)} y2={y(maximum * fraction)} className="chart-grid" />
          <text x={left - 10} y={y(maximum * fraction) + 4} textAnchor="end">{(maximum * fraction).toLocaleString("en-US", { notation: "compact", maximumFractionDigits: 1 })}</text>
        </g>)}
        {paths.map((path) => <path key={path.key} d={path.path} stroke={path.color} strokeWidth="2" fill="none" vectorEffect="non-scaling-stroke" data-trend-segment="true" />)}
        <text x={left} y="204">−60 s</text>
        <text x={(left + right) / 2} y="204" textAnchor="middle">−30 s</text>
        <text x={right} y="204" textAnchor="end">现在</text>
        {!paths.length && <text x="500" y="100" textAnchor="middle" className="chart-empty">等待足够的有效样本</text>}
      </svg>
    </div>
    <p className="chart-note">首末有效样本跨度 <span data-testid="history-span">{(sampleSpanMs / 1000).toFixed(1)} s</span>，空白区域没有观测数据。最多 2000 个绘图点；{downsampled ? "当前历史已降采样，短时变化可能省略。" : "高频时仅减少绘图点，不减少计算输入。"}首次或断档后重新积累，图线不跨越基线变化。</p>
  </section>;
}
