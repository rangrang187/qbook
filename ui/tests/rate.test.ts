import { describe, expect, it } from "vitest";
import { SlidingRate } from "../lib/rate";

describe("SlidingRate", () => {
  it("uses the left-open boundary and accepts zero and same-time increments", () => {
    const rate = new SlidingRate();
    expect(rate.total(0)).toBe(0n);
    rate.push(0, 3n);
    rate.push(0, 4n);
    rate.push(500, 0n);
    rate.push(500, 8n);
    expect(rate.total(999)).toBe(15n);
    expect(rate.total(1000)).toBe(8n);
    expect(rate.total(1500)).toBe(0n);
    expect(rate.total(1500)).toBe(0n);
  });
  it("preserves bigint sums above the floating point integer range", () => {
    const rate = new SlidingRate();
    rate.push(0, 9007199254740993n);
    rate.push(1, 18446744073709551615n);
    expect(rate.total(1)).toBe(18455751272964292608n);
    expect(rate.total(1000)).toBe(18446744073709551615n);
  });
  it("compacts expired prefixes without changing active sums or later writes", () => {
    const rate = new SlidingRate(50);
    // 足够跨越多次 1024 项压缩阈值，同时按独立公式核对窗口结果。
    for (let t = 0; t < 7000; ++t) {
      rate.push(t, BigInt(t + 1));
      const first = Math.max(0, t - 49);
      const count = t - first + 1;
      expect(rate.total(t)).toBe(BigInt((first + 1 + t + 1) * count / 2));
    }
    expect(rate.total(7100)).toBe(0n);
    rate.push(7100, 11n);
    expect(rate.total(7100)).toBe(11n);
  });
  it("reset releases the old time origin and preserves the configured window", () => {
    const rate = new SlidingRate(20);
    rate.push(5000, 9n);
    rate.reset();
    rate.reset();
    rate.push(0, 2n);
    expect(rate.total(19)).toBe(2n);
    expect(rate.total(20)).toBe(0n);
  });
  it.each([0, -1, NaN, Infinity, -Infinity])("rejects invalid window %s", (window) => {
    expect(() => new SlidingRate(window)).toThrow(RangeError);
  });
  it.each([NaN, Infinity, -Infinity, 99])("rejects invalid push time %s before eviction", (time) => {
    const rate = new SlidingRate();
    rate.push(100, 7n);
    expect(() => rate.push(time, 2n)).toThrow(RangeError);
    expect(rate.total(100)).toBe(7n);
  });
  it.each([NaN, Infinity, -Infinity, 99])("rejects invalid query time %s without state changes", (time) => {
    const rate = new SlidingRate();
    rate.push(100, 7n);
    expect(() => rate.total(time)).toThrow(RangeError);
    expect(rate.total(100)).toBe(7n);
  });
  it("validates the increment before a far-future operation can evict old entries", () => {
    const rate = new SlidingRate();
    rate.push(10, 5n);
    expect(() => rate.push(9999, -1n)).toThrow(RangeError);
    expect(rate.total(10)).toBe(5n);
    expect(() => rate.push(9999, 3 as unknown as bigint)).toThrow(RangeError);
    expect(rate.total(10)).toBe(5n);
  });
  it("query advances the shared time watermark", () => {
    const rate = new SlidingRate();
    rate.total(100);
    expect(() => rate.push(99, 1n)).toThrow(RangeError);
    rate.push(100, 1n);
    expect(rate.total(100)).toBe(1n);
  });
});
