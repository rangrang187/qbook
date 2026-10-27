/** 单调到达时间上的精确增量窗口，区间为 (now - windowMs, now]。 */
export class SlidingRate {
  private entries: { t: number; v: bigint }[] = [];
  private head = 0;
  private sum = 0n;
  private lastNow: number | null = null;

  constructor(private readonly windowMs = 1000) {
    if (!Number.isFinite(windowMs) || windowMs <= 0) {
      throw new RangeError("窗口必须是正有限毫秒数");
    }
  }

  push(t: number, increment: bigint): void {
    this.validateTime(t);
    if (typeof increment !== "bigint" || increment < 0n) {
      throw new RangeError("窗口增量必须是非负 bigint");
    }
    this.evict(t);
    this.entries.push({ t, v: increment });
    this.sum += increment;
    this.lastNow = t;
  }

  total(now: number): bigint {
    this.validateTime(now);
    this.evict(now);
    this.lastNow = now;
    return this.sum;
  }

  reset(): void {
    this.entries = [];
    this.head = 0;
    this.sum = 0n;
    this.lastNow = null;
  }

  private validateTime(now: number): void {
    if (!Number.isFinite(now) || (this.lastNow !== null && now < this.lastNow)) {
      throw new RangeError("窗口时间必须有限且不倒退");
    }
  }

  private evict(now: number): void {
    const boundary = now - this.windowMs;
    while (this.head < this.entries.length && this.entries[this.head].t <= boundary) {
      this.sum -= this.entries[this.head++].v;
    }
    if (this.head >= 1024 && this.head * 2 >= this.entries.length) {
      this.entries = this.entries.slice(this.head);
      this.head = 0;
    }
  }
}
