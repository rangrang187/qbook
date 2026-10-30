#!/usr/bin/env python3
"""只读分析已关闭的 V5 原始证据；不推断接纳数或单事件延迟分布。"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from decimal import Decimal, localcontext
from fractions import Fraction
import json
import os
from pathlib import Path
import re
import statistics
import sys
from typing import Any


SCHEMA_VERSION = 1
UINT64_MAX = (1 << 64) - 1
COLUMNS = (
    "schema_version", "run_id", "case_id", "trial_index", "measurement_kind",
    "phase", "count_kind", "work_count", "duration_ns", "sample_mode", "checksum",
    "validation_status", "code_id", "build_id", "workload_hash", "validation_message",
)
KINDS = {"smoke", "baseline", "candidate", "profile"}
COUNT_KINDS = {
    "none", "operations", "snapshots", "snapshot_rounds", "frames", "bytes",
    "written_records", "accepted_events", "completed_raw_intervals",
    "validated_raw_records", "sent_events",
}
STATUSES = {"pending", "valid", "invalid"}
MAX_JSON_BYTES = 64 * 1024 * 1024
MAX_CSV_BYTES = 128 * 1024 * 1024
MAX_ROWS = 1_000_000


class EvidenceError(ValueError):
    """缺失、歧义或矛盾的证据不能通过猜测修补。"""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def uint(value: Any, where: str, *, text: bool = False) -> int:
    if text:
        require(isinstance(value, str) and len(value) <= 20 and re.fullmatch(r"0|[1-9][0-9]*", value) is not None,
                f"{where}: expected canonical unsigned decimal integer")
        value = int(value)
    require(type(value) is int and 0 <= value <= UINT64_MAX, f"{where}: outside uint64")
    return value


def nonempty(value: Any, where: str) -> str:
    require(isinstance(value, str) and bool(value) and len(value) <= 16_384,
            f"{where}: expected non-empty bounded string")
    return value


def mapping(value: Any, where: str, fields: tuple[str, ...] = ()) -> dict:
    require(isinstance(value, dict), f"{where}: expected object")
    missing = set(fields) - value.keys()
    require(not missing, f"{where}: missing fields {sorted(missing)}")
    return value


def canonical(value: Any) -> str:
    return json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(",", ":"), allow_nan=False)


def json_pairs(pairs: list[tuple[str, Any]]) -> dict:
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def read_json(file: Path) -> dict:
    require(file.is_file(), f"missing manifest: {file}")
    require(file.stat().st_size <= MAX_JSON_BYTES, f"manifest exceeds size limit: {file}")
    try:
        def bounded_integer(value: str) -> int:
            require(len(value.lstrip("-")) <= 20, f"{file}: integer exceeds evidence format bounds")
            return int(value)
        value = json.loads(file.read_text(encoding="utf-8"), object_pairs_hook=json_pairs, parse_int=bounded_integer,
                           parse_constant=lambda value: (_ for _ in ()).throw(EvidenceError(f"invalid JSON constant {value}")))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise EvidenceError(f"{file}: {error}") from error
    return mapping(value, str(file))


def contained_file(directory: Path, name: str) -> Path:
    relative = Path(nonempty(name, "csv_files entry"))
    require(not relative.is_absolute() and ".." not in relative.parts, f"unsafe result path: {name}")
    result = directory.joinpath(relative).resolve()
    require(result.is_relative_to(directory), f"result symlink escapes run: {name}")
    require(result.is_file(), f"missing result file: {result}")
    return result


@dataclass(frozen=True)
class Row:
    values: dict[str, Any]
    source: str

    def __getitem__(self, key: str) -> Any:
        return self.values[key]


@dataclass(frozen=True)
class Run:
    directory: Path
    manifest: dict
    cases: dict[str, dict]
    trials: dict[tuple[str, int], dict]
    rows: tuple[Row, ...]


def validate_identity(manifest: dict, source: Path) -> None:
    mapping(manifest, str(source), (
        "schema_version", "run_id", "measurement_kind", "complete", "command", "code", "build",
        "environment", "generator_version", "seed", "warmup_rounds", "repetitions", "cases", "trials",
        "warmup_trials", "csv_files", "error",
    ))
    require(type(manifest["schema_version"]) is int and manifest["schema_version"] == SCHEMA_VERSION,
            f"{source}: unsupported schema_version")
    require(manifest["complete"] is True, f"{source}: incomplete run: {manifest.get('error', '')}")
    nonempty(manifest["run_id"], "run_id")
    require(manifest["measurement_kind"] in KINDS, "unknown measurement_kind")
    nonempty(manifest["generator_version"], "generator_version")
    uint(manifest["seed"], "seed")
    require(isinstance(manifest["command"], list) and bool(manifest["command"]), "command must be a non-empty argv array")
    for argument in manifest["command"]:
        require(isinstance(argument, str), "command arguments must be strings")
    require(isinstance(manifest["error"], str), "error must be a string")
    code = mapping(manifest["code"], "code", ("id", "git_head", "dirty", "source_root", "content_hash"))
    for key in ("id", "git_head", "source_root", "content_hash"):
        nonempty(code[key], f"code.{key}")
    require(type(code["dirty"]) is bool, "code.dirty must be boolean")
    build = mapping(manifest["build"], "build", ("id", "executable", "binary_hash", "build_directory", "policy"))
    for key in ("id", "executable", "binary_hash", "build_directory"):
        nonempty(build[key], f"build.{key}")
    policy = mapping(build["policy"], "build.policy", ("build_type", "ipo", "compiler_id", "compiler_version", "cxx_flags", "cxx_flags_release",
                                                        "dependency_manifests", "arrow_package_config", "parquet_package_config"))
    dependencies = mapping(policy["dependency_manifests"], "build.policy.dependency_manifests")
    require(len(dependencies) == 6, "expected six fixed dependency manifest identities")
    for name, identity in dependencies.items():
        nonempty(name, "dependency manifest name")
        nonempty(identity, "dependency manifest identity")
    for key in ("arrow_package_config", "parquet_package_config"):
        nonempty(policy[key], f"build.policy.{key}")
    for key in ("build_type", "compiler_id", "compiler_version"):
        nonempty(policy[key], f"build.policy.{key}")
    for key in ("cxx_flags", "cxx_flags_release"):
        require(isinstance(policy[key], str), f"build.policy.{key} must be string")
    if manifest["measurement_kind"] in {"baseline", "candidate"}:
        require(policy["build_type"] == "Release", "formal results require Release build policy")
        require(policy["ipo"] is False or policy["ipo"] == "OFF", "formal baseline policy requires IPO/LTO OFF")
        require(not any(flag.startswith("-flto") for flag in (policy["cxx_flags"] + " " + policy["cxx_flags_release"]).split()),
                "formal baseline flags cannot enable LTO")
    environment = mapping(manifest["environment"], "environment", (
        "os", "kernel", "machine", "cpu_model", "logical_cpus", "memory_total_kb", "affinity",
        "filesystem", "frequency_control", "background_load",
    ))
    require(all(value is not None for value in environment.values()), "environment must record unknown explicitly, not null")
    canonical(environment)  # 拒绝非有限数，不将其写成可比较条件。
    repetitions = uint(manifest["repetitions"], "repetitions")
    require(0 < repetitions <= MAX_ROWS, "repetitions out of analysis limits")
    uint(manifest["warmup_rounds"], "warmup_rounds")


def read_run(directory: Path) -> Run:
    directory = directory.resolve()
    manifest_file = contained_file(directory, "manifest.json")
    manifest = read_json(manifest_file)
    validate_identity(manifest, manifest_file)
    cases: dict[str, dict] = {}
    require(isinstance(manifest["cases"], list) and 0 < len(manifest["cases"]) <= 10_000, "cases must be bounded and non-empty")
    phases: dict[str, dict[str, dict]] = {}
    expected_rows = 0
    for case in manifest["cases"]:
        mapping(case, "case", ("case_id", "workload_hash", "parameters", "expected_phases", "instrumentation"))
        case_id = nonempty(case["case_id"], "case_id")
        require(case_id not in cases, f"duplicate case_id: {case_id}")
        require(re.fullmatch(r"fnv1a64:[0-9a-f]{16}", case["workload_hash"]) is not None, "invalid workload_hash")
        mapping(case["parameters"], f"{case_id}.parameters")
        nonempty(case["instrumentation"], f"{case_id}.instrumentation")
        phase_list = case["expected_phases"]
        require(isinstance(phase_list, list) and 0 < len(phase_list) <= 64, "expected_phases must be non-empty and bounded")
        phase_map = {}
        for phase in phase_list:
            mapping(phase, "phase", ("phase", "count_kind", "timing_boundary"))
            name = nonempty(phase["phase"], "phase")
            require(name not in phase_map, f"duplicate phase: {case_id}/{name}")
            require(phase["count_kind"] in COUNT_KINDS, f"unknown count_kind: {phase['count_kind']}")
            nonempty(phase["timing_boundary"], "timing_boundary")
            phase_map[name] = phase
        cases[case_id] = case
        phases[case_id] = phase_map
        expected_rows += len(phase_map) * manifest["repetitions"]
    require(expected_rows <= MAX_ROWS, "expected result count exceeds analysis limit")

    def parse_trials(items: Any, count: int, label: str) -> dict[tuple[str, int], dict]:
        require(isinstance(items, list), f"{label} must be array")
        require(len(items) == len(cases) * count, f"{label}: missing or extra trials")
        parsed = {}
        for trial in items:
            mapping(trial, label, ("case_id", "trial_index", "validation_status", "validation_message", "details"))
            index = uint(trial["trial_index"], f"{label}.trial_index")
            key = (trial["case_id"], index)
            require(key[0] in cases and index < count, f"{label}: unknown case or trial index")
            require(key not in parsed, f"{label}: duplicate trial {key}")
            require(trial["validation_status"] in STATUSES, f"{label}: unknown validation_status")
            require(isinstance(trial["validation_message"], str), f"{label}: validation_message must be string")
            if trial["validation_status"] == "invalid":
                nonempty(trial["validation_message"], f"{label}: invalid trial reason")
            details = mapping(trial["details"], f"{label}.details")
            for name, value in details.items():
                if name.endswith("_count") and value is not None:
                    uint(value, f"details.{name}")
            if key[0].startswith("pipeline/network-"):
                require("accepted_count" in details and details["accepted_count"] is None,
                        "network accepted_count must explicitly remain null")
            elif key[0].startswith("pipeline/controlled-queue/"):
                require("accepted_count" in details, "controlled queue trial requires admission ledger count")
                uint(details["accepted_count"], "controlled accepted_count")
            if "file_rows" in details:
                for name, value in mapping(details["file_rows"], "file_rows").items():
                    uint(value, f"file_rows.{name}")
            if "pipeline_samples" in details:
                samples = details["pipeline_samples"]
                require(isinstance(samples, list) and len(samples) <= MAX_ROWS, "pipeline_samples must be bounded array")
                for sample in samples:
                    mapping(sample, "pipeline sample", ("clock_domain", "begin_ns", "end_ns", "lateness_ns", "feed_messages", "feed_dropped",
                        "book_events", "writer_raw_records", "first_send_ns", "last_send_ns", "max_send_lateness_ns", "phase", "status", "reason"))
                    require(sample["clock_domain"] in {"parent", "worker"}, "unknown pipeline clock domain")
                    for name in ("begin_ns", "end_ns", "lateness_ns", "feed_messages", "feed_dropped", "book_events", "writer_raw_records",
                                 "first_send_ns", "last_send_ns", "max_send_lateness_ns"):
                        uint(sample[name], f"pipeline sample.{name}")
                    require(sample["phase"] in {"boundary", "warmup", "measure"}, "unknown pipeline sample phase")
                    require(sample["status"] in {"boundary", "valid", "invalid"} and isinstance(sample["reason"], str), "invalid pipeline sample status/reason")
            if key[0].startswith("pipeline/network-steady/") and trial["validation_status"] == "valid" and manifest["measurement_kind"] in {"baseline", "candidate"}:
                require(details.get("measured_before_guard_end") is True, "steady measurement did not finish in guard window")
                require(details.get("close_success") is True and details.get("fields_verified") is True, "steady files not normally closed and independently verified")
                samples = details.get("pipeline_samples", [])
                stability = mapping(details.get("stability"), "steady stability", ("parent", "worker"))
                needed = uint(cases[key[0]]["parameters"].get("stable_windows"), "stable_windows")
                require(needed > 0, "steady stable_windows must be positive")
                for domain in ("parent", "worker"):
                    state = mapping(stability[domain], f"stability.{domain}", ("valid", "warmup_windows", "measure_windows", "message"))
                    require(state["valid"] is True, f"{domain} steady stability failed")
                    warm = [sample for sample in samples if sample["clock_domain"] == domain and sample["phase"] == "warmup"]
                    measure = [sample for sample in samples if sample["clock_domain"] == domain and sample["phase"] == "measure"]
                    require(uint(state["warmup_windows"], "warmup_windows") == len(warm) and uint(state["measure_windows"], "measure_windows") == len(measure),
                            f"{domain} stability summary/window count mismatch")
                    require(len(warm) >= needed and len(measure) >= needed, f"{domain} missing planned steady windows")
                    require(all(sample["status"] == "valid" for sample in warm[-needed:] + measure), f"{domain} invalid required steady window")
            parsed[key] = trial
        return parsed

    trials = parse_trials(manifest["trials"], manifest["repetitions"], "trials")
    parse_trials(manifest["warmup_trials"], manifest["warmup_rounds"], "warmup_trials")
    files = manifest["csv_files"]
    require(isinstance(files, list) and 0 < len(files) <= 64, "csv_files must be a bounded non-empty array")
    require(len(set(files)) == len(files), "duplicate csv_files entry")
    keys = set()
    rows: list[Row] = []
    physical_files = set()
    for filename in files:
        file = contained_file(directory, filename)
        require(file not in physical_files, "csv_files resolve to the same file")
        physical_files.add(file)
        require(0 < file.stat().st_size <= MAX_CSV_BYTES, f"CSV empty or too large: {file}")
        try:
            with file.open(newline="", encoding="utf-8") as source:
                reader = csv.DictReader(source, strict=True)
                require(tuple(reader.fieldnames or ()) == COLUMNS, f"{file}: expected fixed 16-column header")
                for raw in reader:
                    location = f"{file}:{reader.line_num}"
                    require(None not in raw and all(value is not None for value in raw.values()), f"{location}: truncated or extra columns")
                    for field in ("schema_version", "trial_index", "work_count", "duration_ns", "checksum"):
                        raw[field] = uint(raw[field], f"{location}:{field}", text=True)
                    require(raw["schema_version"] == SCHEMA_VERSION, f"{location}: unknown schema_version")
                    require(raw["run_id"] == manifest["run_id"], f"{location}: run_id mismatch")
                    require(raw["measurement_kind"] == manifest["measurement_kind"], f"{location}: measurement_kind mismatch")
                    require(raw["code_id"] == manifest["code"]["id"] and raw["build_id"] == manifest["build"]["id"], f"{location}: code/build identity mismatch")
                    case_id = raw["case_id"]
                    trial_key = (case_id, raw["trial_index"])
                    require(trial_key in trials, f"{location}: unexpected case/trial")
                    require(raw["phase"] in phases[case_id], f"{location}: unexpected phase")
                    definition = phases[case_id][raw["phase"]]
                    require(raw["count_kind"] == definition["count_kind"], f"{location}: count_kind mismatch")
                    require(raw["workload_hash"] == cases[case_id]["workload_hash"], f"{location}: workload identity mismatch")
                    require(raw["sample_mode"] == "batch-average", f"{location}: unsupported sample_mode (operation distributions require separate evidence)")
                    require(raw["validation_status"] in STATUSES and raw["validation_status"] == trials[trial_key]["validation_status"], f"{location}: trial/row validation mismatch")
                    require(isinstance(raw["validation_message"], str), f"{location}: invalid validation_message")
                    if raw["validation_status"] == "invalid":
                        nonempty(raw["validation_message"], f"{location}: invalid row reason")
                    if raw["count_kind"] == "none":
                        require(raw["work_count"] == 0, f"{location}: none requires work_count=0")
                        if raw["duration_ns"] == 0 and raw["validation_status"] == "valid":
                            nonempty(raw["validation_message"], f"{location}: zero duration needs resolution note")
                    elif raw["validation_status"] == "valid":
                        require(raw["work_count"] > 0 and raw["duration_ns"] > 0, f"{location}: valid ratio requires positive count and duration")
                    key = (raw["run_id"], case_id, raw["trial_index"], raw["phase"])
                    require(key not in keys, f"{location}: duplicate complete result key {key}")
                    keys.add(key)
                    rows.append(Row(raw, location))
                    require(len(rows) <= expected_rows, f"{location}: too many rows")
        except (OSError, UnicodeError, csv.Error) as error:
            raise EvidenceError(f"{file}: {error}") from error
    require(len(rows) == expected_rows, f"{directory}: missing expected trial/phase rows")
    return Run(directory, manifest, cases, trials, tuple(rows))


def ReadValidatedRuns(root: Path | str) -> list[Run]:
    root = Path(root).resolve()
    require(root.is_dir(), f"not a result directory: {root}")
    if (root / "manifest.json").exists():
        directories = [root]
    else:
        require(not list(root.glob("*.csv")), f"CSV without manifest at root: {root}")
        directories = []
        for entry in sorted(root.iterdir()):
            if entry.is_symlink():
                require(entry.resolve().is_relative_to(root), f"result directory symlink escapes root: {entry}")
            if not entry.is_dir():
                continue
            if (entry / "manifest.json").exists():
                directories.append(entry.resolve())
            else:
                require(not list(entry.glob("*.csv")), f"CSV without manifest: {entry}")
        require(bool(directories), f"no runs found in {root}")
    require(len(directories) <= 10_000 and len(set(directories)) == len(directories), "duplicate or too many run directories")
    runs = [read_run(directory) for directory in directories]
    ids = [run.manifest["run_id"] for run in runs]
    require(len(ids) == len(set(ids)), "duplicate run_id across result directories")
    return runs


def comparable_definition(run: Run, row: Row) -> dict:
    case = run.cases[row["case_id"]]
    phase = next(value for value in case["expected_phases"] if value["phase"] == row["phase"])
    return {
        "case_id": row["case_id"], "phase": row["phase"], "count_kind": row["count_kind"],
        "sample_mode": row["sample_mode"], "workload_hash": row["workload_hash"],
        "parameters": case["parameters"], "timing_boundary": phase["timing_boundary"],
        "instrumentation": case["instrumentation"], "generator_version": run.manifest["generator_version"],
        "seed": run.manifest["seed"],
        "warmup_rounds": run.manifest["warmup_rounds"], "repetitions": run.manifest["repetitions"],
        "build_policy": run.manifest["build"]["policy"], "environment": run.manifest["environment"],
    }


def median(values: list[Fraction]) -> Fraction:
    ordered = sorted(values)
    middle = len(ordered) // 2
    return ordered[middle] if len(ordered) % 2 else (ordered[middle - 1] + ordered[middle]) / 2


def describe(values: list[Fraction]) -> dict:
    require(bool(values), "cannot summarize an empty group")
    q1, _, q3 = statistics.quantiles(values, n=4, method="inclusive") if len(values) > 1 else (values[0], values[0], values[0])
    return {"n": len(values), "median": median(values), "min": min(values), "max": max(values), "iqr": q3 - q1}


def AnalyseRuns(runs: list[Run], kind: str = "baseline") -> dict:
    require(kind in {"baseline", "candidate"}, "only baseline/candidate can form formal summaries")
    groups = {}
    excluded = []
    for run in runs:
        failed_warmups = {}
        for trial in run.manifest["warmup_trials"]:
            if trial["validation_status"] != "valid":
                failed_warmups.setdefault(trial["case_id"], []).append(
                    f"warmup {trial['trial_index']} {trial['validation_status']}: {trial['validation_message']}")
        for row in run.rows:
            definition = comparable_definition(run, row)
            key = canonical({"definition": definition, "code_id": row["code_id"], "build_id": row["build_id"]})
            group = groups.setdefault(key, {"definition": definition, "code_id": row["code_id"], "build_id": row["build_id"], "rows": [], "excluded": []})
            reason = None
            if row["measurement_kind"] != kind:
                reason = f"measurement_kind={row['measurement_kind']} excluded from {kind}"
            elif row["validation_status"] != "valid":
                reason = f"{row['validation_status']}: {row['validation_message'] or run.trials[(row['case_id'], row['trial_index'])]['validation_message']}"
            elif row["case_id"] in failed_warmups:
                reason = "; ".join(failed_warmups[row["case_id"]])
            if reason is not None:
                entry = {"source": row.source, "reason": reason, "row": row}
                excluded.append(entry)
                group["excluded"].append(entry)
            else:
                group["rows"].append(row)
    summaries = []
    for group in groups.values():
        rows = group["rows"]
        if not rows:
            continue
        pure = group["definition"]["count_kind"] == "none"
        values = [Fraction(row["duration_ns"]) if pure else Fraction(row["duration_ns"], row["work_count"]) for row in rows]
        group["statistics"] = describe(values)
        group["unit"] = "ns" if pure else "ns/work"
        group["throughput"] = None if pure else describe([Fraction(row["work_count"] * 1_000_000_000, row["duration_ns"]) for row in rows])
        group["insufficient"] = any(entry["row"]["measurement_kind"] == kind for entry in group["excluded"])
        summaries.append(group)
    invalid = [entry for entry in excluded if entry["row"]["measurement_kind"] == kind]
    return {"kind": kind, "runs": runs, "groups": summaries, "excluded": excluded,
            "complete": bool(summaries) and not invalid,
            "valid_rows": sum(len(group["rows"]) for group in summaries)}


def CompareCandidate(baseline: list[Run], candidate: list[Run]) -> dict:
    left = AnalyseRuns(baseline, "baseline")
    right = AnalyseRuns(candidate, "candidate")
    pairs = []
    incompatible = []
    def index(analysis: dict) -> dict:
        result = {}
        for group in analysis["groups"]:
            definition = group["definition"]
            key = (definition["case_id"], definition["phase"])
            result.setdefault(key, []).append(group)
        return result
    a, b = index(left), index(right)
    for key in sorted(a.keys() | b.keys()):
        if len(a.get(key, [])) != 1 or len(b.get(key, [])) != 1:
            incompatible.append({"case_phase": key, "reason": "missing or ambiguous group on one side"})
            continue
        before, after = a[key][0], b[key][0]
        differences = [field for field in before["definition"] if canonical(before["definition"][field]) != canonical(after["definition"][field])]
        if differences:
            incompatible.append({"case_phase": key, "reason": "incompatible " + ", ".join(differences)})
            continue
        if before["insufficient"] or after["insufficient"]:
            incompatible.append({"case_phase": key, "reason": "invalid trials leave insufficient planned repetitions"})
            continue
        old, new = before["statistics"]["median"], after["statistics"]["median"]
        throughput_change = None
        if before["throughput"] is not None:
            old_rate = before["throughput"]["median"]
            throughput_change = (after["throughput"]["median"] - old_rate) * 100 / old_rate
        pairs.append({"case_phase": key, "before": before, "after": after,
                      "time_reduction_percent": (old - new) * 100 / old if old else None,
                      "throughput_increase_percent": throughput_change})
    return {"baseline": left, "candidate": right, "pairs": pairs, "incompatible": incompatible,
            "complete": left["complete"] and right["complete"] and bool(pairs) and not incompatible}


def display(value: Fraction | None) -> str:
    if value is None:
        return "N/A"
    with localcontext() as context:
        context.prec = 80
        text = format(Decimal(value.numerator) / Decimal(value.denominator), ".6f")
    return text.rstrip("0").rstrip(".") if "." in text else text


def md(value: Any) -> str:
    return str(value).replace("\\", "\\\\").replace("|", "\\|").replace("\r", "\\r").replace("\n", "\\n")


def source_reference(value: str | Path, relative_to: Path | None) -> str:
    text = str(value)
    if relative_to is None:
        return text
    filename, separator, line = text.rpartition(":")
    if not separator or not line.isdecimal():
        filename, line = text, ""
    relative = os.path.relpath(filename, relative_to)
    return relative + (":" + line if line else "")


def summary_text(analysis: dict, relative_to: Path | None = None) -> str:
    lines = [f"## {analysis['kind']} 汇总", "",
             f"有效结果行：{analysis['valid_rows']}；排除行：{len(analysis['excluded'])}；完整有效汇总：{'是' if analysis['complete'] else '否'}。", "",
             "统计为每轮批均值的中位数与范围；不是单事件 P99。IQR 使用 inclusive 四分位法；单轮 IQR=0 不表示无噪声。", "",
             "| case / phase | 分母 | 有效轮 | 中位数 | 最小～最大 | IQR | 吞吐中位数（work/s） |", "|---|---|---:|---:|---|---:|---:|"]
    for group in analysis["groups"]:
        d, s = group["definition"], group["statistics"]
        lines.append(f"| {md(d['case_id'])} / {md(d['phase'])} | {md(d['count_kind'])} | {s['n']} | {display(s['median'])} {group['unit']} | {display(s['min'])}～{display(s['max'])} | {display(s['iqr'])} | {display(group['throughput']['median']) if group['throughput'] else 'N/A'} |")
    lines += ["", "### 全部原始轮次与定义", ""]
    for group in analysis["groups"]:
        d = group["definition"]
        lines += [f"- {md(d['case_id'])} / {md(d['phase'])}；code={md(group['code_id'])}；build={md(group['build_id'])}；workload={d['workload_hash']}。",
                  f"  时间边界：{md(d['timing_boundary'])}；观测方式：{md(d['instrumentation'])}。",
                  f"  完整比较条件：`{md(canonical(d))}`。"]
        for row in group["rows"]:
            note = "；低于当前计时分辨率，不表示零成本" if row["duration_ns"] == 0 else ""
            lines.append(f"  - {md(source_reference(row.source, relative_to))}；trial={row['trial_index']}；work_count={row['work_count']}；duration_ns={row['duration_ns']}；checksum={row['checksum']}{note}。")
    lines += ["", "### 排除记录（保留原始证据）", ""]
    lines += [f"- {md(source_reference(entry['source'], relative_to))}：{md(entry['reason'])}；work_count={entry['row']['work_count']}；duration_ns={entry['row']['duration_ns']}。" for entry in analysis["excluded"]] or ["无。"]
    lines += ["", "### 运行及未知计数", ""]
    for run in analysis["runs"]:
        lines.append(f"- {md(source_reference(run.directory / 'manifest.json', relative_to))}；run_id={md(run.manifest['run_id'])}；kind={run.manifest['measurement_kind']}。")
        for key, trial in run.trials.items():
            details = trial["details"]
            if "accepted_count" in details:
                count = "null（未知，未用发送或解码数代填）" if details["accepted_count"] is None else str(details["accepted_count"])
                lines.append(f"  - {md(key[0])} / trial {key[1]}：accepted_count={count}。")
            if "stability" in details:
                lines.append(f"  - {md(key[0])} / trial {key[1]}：稳态摘要 `{md(canonical(details['stability']))}`；完整窗口保留于 manifest 的 pipeline_samples，各时钟域独立。")
        for trial in run.manifest["warmup_trials"]:
            lines.append(f"  - {md(trial['case_id'])} / warmup {trial['trial_index']}：{trial['validation_status']} {md(trial['validation_message'])}。")
    return "\n".join(lines) + "\n"


def WriteEvidenceIndex(result: dict, output: Path | str) -> None:
    output = Path(output)
    relative_to = output.parent.resolve()
    lines = ["# qbook V5 实验分析", "", "OI-04 的绝对性能门槛待定；本报告不宣称性能达标或 V5 完整验收通过。", ""]
    if "pairs" not in result:
        lines.append(summary_text(result, relative_to))
    else:
        lines += ["## A/B 可比性", "", f"全部计划组可比较：{'是' if result['complete'] else '否'}。正的耗时降低比例和正的吞吐增加比例分别表示对应方向改善，不自动构成保留优化的决定。", "",
                  "| case / phase | A 中位数 | B 中位数 | 耗时降低 % | 吞吐增加 % |", "|---|---:|---:|---:|---:|"]
        for pair in result["pairs"]:
            before, after = pair["before"], pair["after"]
            lines.append(f"| {md('/'.join(pair['case_phase']))} | {display(before['statistics']['median'])} {before['unit']} | {display(after['statistics']['median'])} {after['unit']} | {display(pair['time_reduction_percent'])} | {display(pair['throughput_increase_percent'])} |")
        lines += ["", "不可比或缺失组：", ""]
        lines += [f"- {md('/'.join(item['case_phase']))}：{md(item['reason'])}。" for item in result["incompatible"]] or ["无。"]
        lines += ["", summary_text(result["baseline"], relative_to), "", summary_text(result["candidate"], relative_to)]
    # x 模式在打开时排他，已有报告或符号链接不会被截断覆盖。
    with output.open("x", encoding="utf-8", newline="\n") as destination:
        destination.write("\n".join(lines) + "\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--input", type=Path)
    mode.add_argument("--baseline", type=Path)
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    if (args.baseline is not None) != (args.candidate is not None):
        parser.error("--baseline and --candidate must be supplied together; cannot mix with --input")
    try:
        require(not args.output.exists() and not args.output.is_symlink(), f"output already exists: {args.output}")
        if args.input is not None:
            result = AnalyseRuns(ReadValidatedRuns(args.input))
        else:
            result = CompareCandidate(ReadValidatedRuns(args.baseline), ReadValidatedRuns(args.candidate))
        WriteEvidenceIndex(result, args.output)
        print(f"wrote {args.output}; complete={str(result['complete']).lower()}")
        return 0 if result["complete"] else 1
    except (EvidenceError, OSError, TypeError, csv.Error) as error:
        print(f"benchmark analysis failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
