"""手写数值与契约验证；不由被测汇总函数生成期望。"""

import copy
import csv
from fractions import Fraction
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/analyse-benchmarks.py"
SPEC = importlib.util.spec_from_file_location("qbook_benchmark_analysis", SCRIPT)
analysis = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = analysis
SPEC.loader.exec_module(analysis)

# 独立固定表头，避免测试与实现一起误减成计划早期的15列。
HEADER = "schema_version,run_id,case_id,trial_index,measurement_kind,phase,count_kind,work_count,duration_ns,sample_mode,checksum,validation_status,code_id,build_id,workload_hash,validation_message".split(",")


class AnalysisTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="qbook-analyser-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def fixture(self, name="run-a", *, kind="baseline", times=(100, 200, 300),
                count=10, count_kind="operations", phase="apply", case="core.example",
                code="code-a", build="build-a"):
        directory = self.root / name
        directory.mkdir()
        manifest = {
            "schema_version": 1, "run_id": name, "measurement_kind": kind,
            "complete": True, "command": ["./qbook_core_bench", "--case", case],
            "code": {"id": code, "git_head": "123abc", "dirty": True,
                     "source_root": "/source", "content_hash": "sha256:abc"},
            "build": {"id": build, "executable": "/build/qbook_core_bench", "binary_hash": "sha256:def",
                      "build_directory": "/build", "policy": {"build_type": "Release", "ipo": False,
                      "compiler_id": "GNU", "compiler_version": "13.3.0", "cxx_flags": "", "cxx_flags_release": "-O3 -DNDEBUG",
                      "dependency_manifests": {name: "test-hash" for name in ("qlib", "Catch2", "fkYAML", "spdlog", "libzmq", "cppzmq")},
                      "arrow_package_config": "not-linked", "parquet_package_config": "not-linked"}},
            "environment": {"os": "Linux", "kernel": "test", "machine": "x86_64", "cpu_model": "fixture CPU",
                            "logical_cpus": 4, "memory_total_kb": 1024, "affinity": "0-3", "filesystem": "ext4",
                            "frequency_control": "uncontrolled", "background_load": "uncontrolled"},
            "generator_version": "test-generator-1", "seed": 42, "warmup_rounds": 0, "repetitions": len(times),
            "csv_files": ["measurements.csv"],
            "cases": [{"case_id": case, "workload_hash": "fnv1a64:0123456789abcdef",
                       "parameters": {"seed": 42, "initial_orders": 4, "events_per_trial": count},
                       "expected_phases": [{"phase": phase, "count_kind": count_kind, "timing_boundary": "two steady reads around real calls"}],
                       "instrumentation": "batch-boundaries"}],
            "trials": [{"case_id": case, "trial_index": index, "validation_status": "valid", "validation_message": "",
                        "details": {"accepted_count": None} if case.startswith("pipeline/network-") else {}}
                       for index in range(len(times))],
            "warmup_trials": [], "error": "",
        }
        rows = [{"schema_version": "1", "run_id": name, "case_id": case, "trial_index": str(index),
                 "measurement_kind": kind, "phase": phase, "count_kind": count_kind,
                 "work_count": str(count), "duration_ns": str(duration), "sample_mode": "batch-average",
                 "checksum": "18446744073709551615", "validation_status": "valid", "code_id": code,
                 "build_id": build, "workload_hash": "fnv1a64:0123456789abcdef",
                 "validation_message": "below clock resolution" if duration == 0 else ""}
                for index, duration in enumerate(times)]
        self.save(directory, manifest, rows)
        return directory, manifest, rows

    def save(self, directory, manifest, rows, header=HEADER):
        (directory / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
        with (directory / "measurements.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=header, extrasaction="ignore")
            writer.writeheader()
            writer.writerows(rows)

    def read(self, directory):
        return analysis.ReadValidatedRuns(directory)

    def test_fixed_header_and_hand_calculated_statistics(self):
        directory, _, _ = self.fixture()
        self.assertEqual(len(analysis.COLUMNS), 16)
        self.assertEqual(tuple(HEADER), analysis.COLUMNS)
        result = analysis.AnalyseRuns(self.read(directory))
        group = result["groups"][0]
        self.assertEqual(group["statistics"], {"n": 3, "median": Fraction(20), "min": Fraction(10), "max": Fraction(30), "iqr": Fraction(10)})
        self.assertEqual(group["throughput"]["median"], Fraction(50_000_000))
        self.assertTrue(result["complete"])

    def test_uint64_and_over_two_to_53_are_never_float(self):
        directory, _, _ = self.fixture(times=(9_007_199_254_740_993, 18_446_744_073_709_551_615), count=3)
        runs = self.read(directory)
        result = analysis.AnalyseRuns(runs)
        self.assertEqual(runs[0].rows[0]["duration_ns"], 9_007_199_254_740_993)
        self.assertEqual(result["groups"][0]["statistics"]["median"], Fraction(9_007_199_254_740_993 + 18_446_744_073_709_551_615, 6))
        self.assertEqual(runs[0].rows[0]["checksum"], (1 << 64) - 1)

    def test_none_zero_kept_without_rate(self):
        directory, _, _ = self.fixture(times=(0, 10), count=0, count_kind="none", phase="close")
        result = analysis.AnalyseRuns(self.read(directory))
        group = result["groups"][0]
        self.assertEqual(group["statistics"]["median"], Fraction(5))
        self.assertIsNone(group["throughput"])
        self.assertEqual(group["unit"], "ns")
        report = analysis.summary_text(result)
        self.assertIn("低于当前计时分辨率", report)
        self.assertIn("不是单事件 P99", report)
        self.assertNotIn("Infinity", report)

    def test_strict_unsigned_decimal_and_bounds(self):
        directory, manifest, rows = self.fixture()
        for value in ("-1", "+1", "01", "1.0", "1e3", " 1", "1\n", str(1 << 64), "9" * 5000, ""):
            with self.subTest(value=value):
                changed = copy.deepcopy(rows)
                changed[0]["duration_ns"] = value
                self.save(directory, manifest, changed)
                with self.assertRaises(analysis.EvidenceError):
                    self.read(directory)

    def test_invalid_ratio_and_none_denominator_rejected(self):
        directory, manifest, rows = self.fixture()
        for field in ("duration_ns", "work_count"):
            changed = copy.deepcopy(rows)
            changed[0][field] = "0"
            self.save(directory, manifest, changed)
            with self.assertRaisesRegex(analysis.EvidenceError, "positive"):
                self.read(directory)
        directory2, manifest2, rows2 = self.fixture("none", times=(1,), count=1, count_kind="none")
        with self.assertRaisesRegex(analysis.EvidenceError, "work_count=0"):
            self.read(directory2)

    def test_zero_pure_duration_requires_resolution_note(self):
        directory, manifest, rows = self.fixture(times=(0,), count=0, count_kind="none")
        rows[0]["validation_message"] = ""
        self.save(directory, manifest, rows)
        with self.assertRaisesRegex(analysis.EvidenceError, "resolution"):
            self.read(directory)

    def test_wrong_header_extra_and_truncated_rows(self):
        directory, manifest, rows = self.fixture()
        self.save(directory, manifest, rows, HEADER[:-1])
        with self.assertRaisesRegex(analysis.EvidenceError, "16-column"):
            self.read(directory)
        for suffix in (",unexpected",):
            self.save(directory, manifest, rows)
            with (directory / "measurements.csv").open("a") as stream:
                stream.write(",".join(str(rows[0][key]) for key in HEADER) + suffix + "\n")
            with self.assertRaisesRegex(analysis.EvidenceError, "columns"):
                self.read(directory)
        self.save(directory, manifest, rows)
        with (directory / "measurements.csv").open("a") as stream:
            stream.write("1,partial\n")
        with self.assertRaisesRegex(analysis.EvidenceError, "truncated"):
            self.read(directory)

    def test_complete_duplicate_key_rejected(self):
        directory, manifest, rows = self.fixture()
        rows[1] = dict(rows[0])
        self.save(directory, manifest, rows)
        with self.assertRaisesRegex(analysis.EvidenceError, "duplicate complete"):
            self.read(directory)

    def test_same_trial_phase_across_cases_is_legal(self):
        directory, manifest, rows = self.fixture(times=(100,))
        extra_case = copy.deepcopy(manifest["cases"][0])
        extra_case["case_id"] = "core.second"
        manifest["cases"].append(extra_case)
        manifest["trials"].append({**manifest["trials"][0], "case_id": "core.second"})
        rows.append({**rows[0], "case_id": "core.second"})
        self.save(directory, manifest, rows)
        self.assertEqual(len(analysis.AnalyseRuns(self.read(directory))["groups"]), 2)

    def test_missing_round_or_phase_is_not_complete(self):
        directory, manifest, rows = self.fixture()
        self.save(directory, manifest, rows[:-1])
        with self.assertRaisesRegex(analysis.EvidenceError, "missing expected"):
            self.read(directory)
        manifest["trials"].pop()
        self.save(directory, manifest, rows)
        with self.assertRaisesRegex(analysis.EvidenceError, "missing or extra trials"):
            self.read(directory)

    def test_manifest_mandatory_and_incomplete_rejected(self):
        directory, manifest, rows = self.fixture()
        manifest["complete"] = False
        manifest["error"] = "Close failed"
        self.save(directory, manifest, rows)
        with self.assertRaisesRegex(analysis.EvidenceError, "incomplete.*Close failed"):
            self.read(directory)
        (directory / "manifest.json").unlink()
        with self.assertRaisesRegex(analysis.EvidenceError, "without manifest"):
            self.read(directory)

    def test_json_duplicate_keys_truncation_and_missing_fields(self):
        directory, manifest, rows = self.fixture()
        for value in ('{"schema_version":1,"schema_version":1}', '{"x":', '{"x":NaN}'):
            (directory / "manifest.json").write_text(value)
            with self.assertRaises(analysis.EvidenceError):
                self.read(directory)
        del manifest["build"]["policy"]["compiler_version"]
        self.save(directory, manifest, rows)
        with self.assertRaisesRegex(analysis.EvidenceError, "missing fields"):
            self.read(directory)

    def test_unicode_quotes_backslashes_and_control_roundtrip(self):
        directory, manifest, rows = self.fixture()
        manifest["command"].append('中文 "引号" \\ 路径\n控制\t')
        self.save(directory, manifest, rows)
        self.assertEqual(self.read(directory)[0].manifest["command"][-1], '中文 "引号" \\ 路径\n控制\t')

    def test_unknown_schema_count_kind_sample_mode_and_status(self):
        directory, original, original_rows = self.fixture()
        for mutate in (
            lambda m, r: m.update(schema_version=2),
            lambda m, r: m["cases"][0]["expected_phases"][0].update(count_kind="guess"),
            lambda m, r: r[0].update(sample_mode="p99"),
            lambda m, r: r[0].update(validation_status="passed"),
            lambda m, r: m.update(measurement_kind="latest"),
        ):
            manifest, rows = copy.deepcopy(original), copy.deepcopy(original_rows)
            mutate(manifest, rows)
            self.save(directory, manifest, rows)
            with self.assertRaises(analysis.EvidenceError):
                self.read(directory)

    def test_row_identity_conflict_rejected(self):
        directory, manifest, rows = self.fixture()
        for field in ("run_id", "code_id", "build_id", "workload_hash", "measurement_kind"):
            changed = copy.deepcopy(rows)
            changed[0][field] = "other"
            self.save(directory, manifest, changed)
            with self.assertRaises(analysis.EvidenceError):
                self.read(directory)

    def test_formal_results_require_release_and_no_lto(self):
        directory, original, rows = self.fixture()
        for change in ({"build_type": "Debug"}, {"ipo": True}, {"cxx_flags": "-flto=auto"}):
            manifest = copy.deepcopy(original)
            manifest["build"]["policy"].update(change)
            self.save(directory, manifest, rows)
            with self.assertRaises(analysis.EvidenceError):
                self.read(directory)

    def test_invalid_round_preserved_and_excluded(self):
        directory, manifest, rows = self.fixture(times=(100, 9999, 300))
        manifest["trials"][1].update(validation_status="invalid", validation_message="independent readback mismatch")
        rows[1].update(validation_status="invalid", validation_message="independent readback mismatch")
        self.save(directory, manifest, rows)
        result = analysis.AnalyseRuns(self.read(directory))
        self.assertEqual(result["valid_rows"], 2)
        self.assertEqual(len(result["excluded"]), 1)
        self.assertFalse(result["complete"])
        self.assertEqual(result["groups"][0]["statistics"]["median"], Fraction(20))
        report = analysis.summary_text(result)
        self.assertIn("9999", report)
        self.assertIn("readback mismatch", report)

    def test_trial_cannot_mix_valid_and_failed_phases(self):
        directory, manifest, rows = self.fixture()
        manifest["trials"][0]["validation_status"] = "invalid"
        manifest["trials"][0]["validation_message"] = "Close failed"
        self.save(directory, manifest, rows)
        with self.assertRaisesRegex(analysis.EvidenceError, "validation mismatch"):
            self.read(directory)

    def test_failed_warmup_cannot_produce_valid_formal_summary(self):
        directory, manifest, rows = self.fixture()
        manifest["warmup_rounds"] = 1
        manifest["warmup_trials"] = [copy.deepcopy(manifest["trials"][0])]
        manifest["warmup_trials"][0].update(validation_status="invalid", validation_message="warmup readback failed")
        manifest["error"] = "one or more trials failed validation"
        self.save(directory, manifest, rows)
        result = analysis.AnalyseRuns(self.read(directory))
        self.assertFalse(result["complete"])
        self.assertEqual(result["valid_rows"], 0)
        self.assertEqual(len(result["excluded"]), 3)
        self.assertIn("warmup readback failed", analysis.summary_text(result))

    def test_smoke_profile_excluded_and_slow_valid_round_retained(self):
        self.fixture("baseline", times=(1, 2, 1_000_000))
        self.fixture("smoke", kind="smoke", times=(1,))
        self.fixture("profile", kind="profile", times=(2,))
        (self.root / "summary.md").write_text("not a run")
        result = analysis.AnalyseRuns(self.read(self.root))
        self.assertEqual(result["valid_rows"], 3)
        self.assertEqual(len(result["excluded"]), 2)
        self.assertEqual(result["groups"][0]["statistics"]["max"], Fraction(100_000))
        self.assertTrue(result["complete"])

    def test_network_unknown_accepted_kept_null(self):
        directory, manifest, rows = self.fixture(case="pipeline/network-finite/mbo", count_kind="validated_raw_records", phase="finite-completion")
        result = analysis.AnalyseRuns(self.read(directory))
        self.assertIn("null（未知", analysis.summary_text(result))
        manifest["trials"][0]["details"]["accepted_count"] = 0
        self.save(directory, manifest, rows)
        with self.assertRaisesRegex(analysis.EvidenceError, "remain null"):
            self.read(directory)

    def steady_fixture(self):
        directory, manifest, rows = self.fixture(case="pipeline/network-steady/mbo", times=(300,),
                                                 count_kind="completed_raw_intervals", phase="writer-span")
        manifest["cases"][0]["parameters"]["stable_windows"] = 2
        details = manifest["trials"][0]["details"]
        details.update(measured_before_guard_end=True, close_success=True, fields_verified=True,
                       pipeline_samples=[], stability={})
        # 独立时钟域故意采用不同原点，禁止分析器直接跨域作时间差。
        for domain, origin in (("parent", 1_000), ("worker", 1_000_000)):
            details["stability"][domain] = {"valid": True, "warmup_windows": 3,
                                            "measure_windows": 2, "message": "verified"}
            for index, phase in enumerate(("warmup", "warmup", "warmup", "measure", "measure")):
                details["pipeline_samples"].append({
                    "clock_domain": domain, "begin_ns": origin + index * 100,
                    "end_ns": origin + (index + 1) * 100, "lateness_ns": 0,
                    "feed_messages": 100 + index * 10, "feed_dropped": 0,
                    "book_events": 100 + index * 10, "writer_raw_records": 100 + index * 10,
                    "first_send_ns": origin, "last_send_ns": origin + index * 100,
                    "max_send_lateness_ns": 0, "phase": phase,
                    "status": "invalid" if index == 0 else "valid", "reason": "initial settling" if index == 0 else "",
                })
        self.save(directory, manifest, rows)
        return directory, manifest, rows

    def test_steady_accepts_independent_clocks_and_only_required_warmup_tail(self):
        directory, _, _ = self.steady_fixture()
        result = analysis.AnalyseRuns(self.read(directory))
        self.assertTrue(result["complete"])
        self.assertEqual(result["groups"][0]["statistics"]["median"], Fraction(30))

    def test_steady_requires_close_guard_complete_windows_and_valid_measurement(self):
        directory, original, rows = self.steady_fixture()
        for mutate in (
            lambda d: d.update(measured_before_guard_end=False),
            lambda d: d.update(close_success=False),
            lambda d: d.update(fields_verified=False),
            lambda d: d["stability"]["parent"].update(valid=False),
            lambda d: d["stability"]["worker"].update(measure_windows=3),
            lambda d: d["pipeline_samples"][3].update(status="invalid", reason="rate below frozen tolerance"),
            lambda d: d["pipeline_samples"][2].update(status="invalid", reason="warmup tail failed"),
            lambda d: d["pipeline_samples"][2].update(clock_domain="shared"),
            lambda d: d["pipeline_samples"][2].update(book_events=-1),
        ):
            manifest = copy.deepcopy(original)
            mutate(manifest["trials"][0]["details"])
            self.save(directory, manifest, rows)
            with self.assertRaises(analysis.EvidenceError):
                self.read(directory)

    def test_failed_steady_keeps_failed_samples_and_raw_durations(self):
        directory, manifest, rows = self.steady_fixture()
        trial = manifest["trials"][0]
        trial.update(validation_status="invalid", validation_message="worker rate below frozen tolerance")
        trial["details"]["stability"]["worker"]["valid"] = False
        trial["details"]["pipeline_samples"][8].update(status="invalid", reason="worker rate below frozen tolerance")
        rows[0].update(validation_status="invalid", validation_message=trial["validation_message"])
        self.save(directory, manifest, rows)
        result = analysis.AnalyseRuns(self.read(directory))
        self.assertFalse(result["complete"])
        self.assertEqual(result["valid_rows"], 0)
        self.assertEqual(result["excluded"][0]["row"]["duration_ns"], 300)
        self.assertIn("worker rate below frozen tolerance", analysis.summary_text(result))

    def test_controlled_queue_requires_known_admission_ledger(self):
        directory, manifest, rows = self.fixture(case="pipeline/controlled-queue/mbo", count_kind="accepted_events")
        for value in (None, -1, "10"):
            for trial in manifest["trials"]:
                trial["details"]["accepted_count"] = value
            self.save(directory, manifest, rows)
            with self.assertRaises(analysis.EvidenceError):
                self.read(directory)
        for trial in manifest["trials"]:
            trial["details"]["accepted_count"] = 10
        self.save(directory, manifest, rows)
        self.assertTrue(analysis.AnalyseRuns(self.read(directory))["complete"])

    def test_unsafe_csv_paths_and_symlink_escape_rejected(self):
        directory, original, rows = self.fixture()
        external = self.root / "outside.csv"
        external.write_text("outside")
        for name in ("../outside.csv", str(external)):
            manifest = copy.deepcopy(original)
            manifest["csv_files"] = [name]
            self.save(directory, manifest, rows)
            with self.assertRaisesRegex(analysis.EvidenceError, "unsafe"):
                self.read(directory)
        (directory / "external.csv").symlink_to(external)
        manifest["csv_files"] = ["external.csv"]
        self.save(directory, manifest, rows)
        with self.assertRaisesRegex(analysis.EvidenceError, "escapes"):
            self.read(directory)

    def test_one_level_discovery_and_missing_manifest(self):
        nested = self.root / "nested"
        nested.mkdir()
        (nested / "deeper").mkdir()
        (nested / "deeper" / "manifest.json").write_text("{}")
        with self.assertRaisesRegex(analysis.EvidenceError, "no runs"):
            self.read(self.root)
        (nested / "measurements.csv").write_text("missing manifest")
        with self.assertRaisesRegex(analysis.EvidenceError, "without manifest"):
            self.read(self.root)

    def test_different_work_count_uses_per_round_ratios(self):
        directory, manifest, rows = self.fixture(times=(100, 100), count=10)
        rows[1]["work_count"] = "20"
        self.save(directory, manifest, rows)
        result = analysis.AnalyseRuns(self.read(directory))
        self.assertEqual(result["groups"][0]["statistics"]["median"], Fraction(15, 2))
        self.assertEqual(result["groups"][0]["throughput"]["median"], Fraction(150_000_000))

    def test_ab_allows_code_binary_and_run_ids_to_differ(self):
        a, _, _ = self.fixture("a", times=(100, 200, 300))
        b, manifest, rows = self.fixture("b", kind="candidate", times=(50, 100, 150), code="code-b", build="build-b")
        manifest["code"].update(content_hash="sha256:different", source_root="/candidate")
        manifest["build"].update(executable="/candidate/program", binary_hash="sha256:different", build_directory="/candidate")
        self.save(b, manifest, rows)
        result = analysis.CompareCandidate(self.read(a), self.read(b))
        self.assertTrue(result["complete"])
        self.assertEqual(result["pairs"][0]["time_reduction_percent"], Fraction(50))
        self.assertEqual(result["pairs"][0]["throughput_increase_percent"], Fraction(100))

    def test_ab_policy_environment_workload_and_boundaries_must_match(self):
        a, _, _ = self.fixture("a")
        b, original, rows = self.fixture("b", kind="candidate")
        for mutate in (
            lambda m: m["build"]["policy"].update(cxx_flags="-march=native"),
            lambda m: m["build"]["policy"]["dependency_manifests"].update(qlib="changed-hash"),
            lambda m: m["environment"].update(affinity="0"),
            lambda m: m.update(seed=43),
            lambda m: m["cases"][0]["parameters"].update(seed=43),
            lambda m: m["cases"][0]["expected_phases"][0].update(timing_boundary="different"),
            lambda m: m["cases"][0].update(instrumentation="perf-attached"),
        ):
            manifest = copy.deepcopy(original)
            mutate(manifest)
            self.save(b, manifest, rows)
            result = analysis.CompareCandidate(self.read(a), self.read(b))
            self.assertFalse(result["complete"])
            self.assertEqual(len(result["pairs"]), 0)
            self.assertEqual(len(result["incompatible"]), 1)

    def test_ab_missing_group_not_hidden_by_intersection(self):
        a, _, _ = self.fixture("a")
        b, _, _ = self.fixture("b", kind="candidate", case="core.other")
        result = analysis.CompareCandidate(self.read(a), self.read(b))
        self.assertFalse(result["complete"])
        self.assertEqual(len(result["incompatible"]), 2)

    def test_ab_zero_baseline_duration_has_no_improvement_ratio(self):
        a, _, _ = self.fixture("a", times=(0,), count=0, count_kind="none", phase="close")
        b, _, _ = self.fixture("b", kind="candidate", times=(1,), count=0, count_kind="none", phase="close")
        result = analysis.CompareCandidate(self.read(a), self.read(b))
        self.assertTrue(result["complete"])
        self.assertIsNone(result["pairs"][0]["time_reduction_percent"])
        self.assertIsNone(result["pairs"][0]["throughput_increase_percent"])

    def test_exclusive_report_and_raw_files_unchanged(self):
        directory, _, _ = self.fixture()
        original = {file.name: file.read_bytes() for file in directory.iterdir()}
        report = self.root / "summary.md"
        result = analysis.AnalyseRuns(self.read(directory))
        analysis.WriteEvidenceIndex(result, report)
        self.assertIn("OI-04", report.read_text())
        self.assertIn("run-a/manifest.json", report.read_text())
        self.assertNotIn(str(self.root), report.read_text())
        saved = report.read_bytes()
        with self.assertRaises(FileExistsError):
            analysis.WriteEvidenceIndex(result, report)
        self.assertEqual(report.read_bytes(), saved)
        self.assertEqual({file.name: file.read_bytes() for file in directory.iterdir()}, original)

    def test_cli_modes_error_codes_and_no_overwrite(self):
        directory, manifest, rows = self.fixture()
        report = self.root / "report.md"
        command = [sys.executable, str(SCRIPT)]
        completed = subprocess.run(command + ["--input", str(directory), "--output", str(report)], capture_output=True, text=True)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        repeated = subprocess.run(command + ["--input", str(directory), "--output", str(report)], capture_output=True, text=True)
        self.assertEqual(repeated.returncode, 2)
        for args in (["--input", str(directory), "--baseline", str(directory)], ["--baseline", str(directory)], ["--input", str(directory), "--candidate", str(directory)], ["--unknown"]):
            result = subprocess.run(command + args + ["--output", str(self.root / "unused.md")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
        self.assertFalse((self.root / "unused.md").exists())


if __name__ == "__main__":
    unittest.main()
