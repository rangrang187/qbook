import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import { decode, METRIC_FIELDS, METRICS_SIZE, parseSample } from '../lib/metrics';

const fixtures = fileURLToPath(new URL('../../tests/fixtures/metrics/', import.meta.url));
const names = ['layout', 'bounds-min', 'bounds-max', 'large-a', 'large-b', 'zero-samples'];

function zeroDto(): Record<string, unknown> {
  return Object.fromEntries(METRIC_FIELDS.map(field => [field, '0']));
}

describe('C++ metrics ABI and exact DTO', () => {
  it.each(names)('%s matches every independent decimal expectation', name => {
    const bytes = readFileSync(`${fixtures}${name}.bin`);
    const original = Buffer.from(bytes);
    const expected = JSON.parse(readFileSync(`${fixtures}${name}.expected.json`, 'utf8'));
    expect(bytes.length).toBe(METRICS_SIZE);
    const dto = decode(bytes);
    expect(dto).toEqual(expected);
    expect(Object.keys(dto)).toEqual([...METRIC_FIELDS]);
    for (const field of METRIC_FIELDS) expect(parseSample(dto)[field]).toBe(BigInt(expected[field]));
    expect(bytes).toEqual(original);
    const unaligned = Buffer.alloc(METRICS_SIZE + 3, 0xff);
    bytes.copy(unaligned, 1);
    expect(decode(unaligned.subarray(1, 137))).toEqual(expected);
  });

  it.each([0, 1, 128, 135, 137, 272])('rejects length %s', length => {
    expect(() => decode(Buffer.alloc(length))).toThrow(/136 bytes/);
  });

  it('preserves signed and unsigned 64-bit boundaries', () => {
    const dto = zeroDto();
    dto.tsNs = '-9223372036854775808';
    dto.feedMessages = '18446744073709551615';
    dto.bookEvents = '9007199254740993';
    const sample = parseSample(dto);
    expect(sample.tsNs).toBe(-(1n << 63n));
    expect(sample.feedMessages).toBe((1n << 64n) - 1n);
    expect(sample.bookEvents).toBe(9007199254740993n);
    dto.tsNs = '9223372036854775807';
    expect(parseSample(dto).tsNs).toBe((1n << 63n) - 1n);
  });
});

describe('complete browser DTO validation', () => {
  it.each([null, [], '0', 0, true, undefined])('rejects non-object %s', value => {
    expect(() => parseSample(value)).toThrow();
  });

  it.each(METRIC_FIELDS)('requires own string field %s', field => {
    const dto = zeroDto();
    delete dto[field];
    expect(() => parseSample(dto)).toThrow(/missing/);
    for (const invalid of [undefined, null, 0, 1n, {}, []]) {
      dto[field] = invalid;
      expect(() => parseSample(dto)).toThrow();
    }
    expect(() => parseSample(Object.create(zeroDto()))).toThrow(/missing/);
  });

  it.each(['', ' ', ' 1', '1 ', '+1', '-0', '01', '-01', '1.0', '1e3', '0x10', '1\n', '9'.repeat(1000)])(
    'rejects noncanonical decimal %j', raw => {
      for (const field of METRIC_FIELDS) {
        expect(() => parseSample({ ...zeroDto(), [field]: raw })).toThrow();
      }
    });

  it('rejects range overflow and negative unsigned values', () => {
    for (const tsNs of ['-9223372036854775809', '9223372036854775808']) {
      expect(() => parseSample({ ...zeroDto(), tsNs })).toThrow(/range/);
    }
    for (const field of METRIC_FIELDS.slice(1)) {
      expect(() => parseSample({ ...zeroDto(), [field]: '-1' })).toThrow();
      expect(() => parseSample({ ...zeroDto(), [field]: '18446744073709551616' })).toThrow(/range/);
    }
  });

  it('ignores extra fields without modifying the caller object', () => {
    const dto = Object.freeze({ ...zeroDto(), optionalFutureField: 'bad' });
    expect(Object.keys(parseSample(dto))).toEqual([...METRIC_FIELDS]);
    expect(dto.optionalFutureField).toBe('bad');
  });
});
