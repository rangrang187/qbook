import type { Buffer } from 'node:buffer';

/** 冻结的 qlib::metrics 小端 ABI，顺序同时决定逐标量读取偏移。 */
export const METRIC_FIELDS = [
  'tsNs',
  'feedMessages', 'feedBytes', 'feedOrders', 'feedTrades', 'feedLevels', 'feedDropped',
  'bookEvents', 'bookApplyNs', 'bookSamples',
  'bookInstruments', 'bookRestingOrders', 'bookMemoryBytes',
  'writerOrders', 'writerTrades', 'writerLevels', 'writerBooks',
] as const;

export const METRICS_SIZE = 136;
export type MetricField = typeof METRIC_FIELDS[number];
export type MetricsDto = { [K in MetricField]: string };
export type MetricsSample = { [K in MetricField]: bigint };

const SIGNED_MIN = -(1n << 63n);
const SIGNED_MAX = (1n << 63n) - 1n;
const UNSIGNED_MAX = (1n << 64n) - 1n;
const UNSIGNED_DECIMAL = /^(0|[1-9][0-9]*)$/;
const SIGNED_DECIMAL = /^(0|-?[1-9][0-9]*)$/;

/** 不修改输入，不读取 padding，不经 number 转换任意 64 位整数。 */
export function decode(frame: Buffer): MetricsDto {
  if (frame.length !== METRICS_SIZE) {
    throw new RangeError(`metrics frame must contain ${METRICS_SIZE} bytes, received ${frame.length}`);
  }
  const result = {} as MetricsDto;
  for (const [index, field] of METRIC_FIELDS.entries()) {
    result[field] = (index === 0
      ? frame.readBigInt64LE(0)
      : frame.readBigUInt64LE(index * 8)).toString(10);
  }
  return result;
}

/** 完整校验后才交付新样本；非法字段不补零、不刷新页面有效到达时间。 */
export function parseSample(dto: unknown): MetricsSample {
  if (dto === null || typeof dto !== 'object' || Array.isArray(dto)) {
    throw new TypeError('metrics DTO must be an object');
  }
  const object = dto as Record<string, unknown>;
  const result = {} as MetricsSample;
  for (const field of METRIC_FIELDS) {
    if (!Object.hasOwn(object, field)) throw new TypeError(`missing metrics field: ${field}`);
    const raw = object[field];
    const signed = field === 'tsNs';
    if (typeof raw !== 'string' || raw.length > 20 || raw.trim() !== raw ||
        !(signed ? SIGNED_DECIMAL : UNSIGNED_DECIMAL).test(raw)) {
      throw new TypeError(`invalid decimal metrics field: ${field}`);
    }
    const value = BigInt(raw);
    if (signed ? value < SIGNED_MIN || value > SIGNED_MAX : value > UNSIGNED_MAX) {
      throw new RangeError(`out-of-range metrics field: ${field}`);
    }
    result[field] = value;
  }
  return result;
}
