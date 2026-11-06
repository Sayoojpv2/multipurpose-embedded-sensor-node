"""End-to-end tests of the simulator + Python pipeline (python -m unittest)."""
import csv
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "python"))
import analyse_results  # noqa: E402
import generate_data  # noqa: E402

EXE = generate_data.EXE
TRACE = os.path.join(ROOT, "experiments", "traces", "water_outage.csv")


def sim(out, *args):
    r = subprocess.run([EXE, "--profile", "water", "--trace", TRACE, "--out", out, *args], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr)
    with open(os.path.join(out, "summary.json")) as f:
        s = json.load(f)
    return s


def digest(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


class Pipeline(unittest.TestCase):
    def setUp(self):
        if not os.path.exists(TRACE):
            self.skipTest("run python/generate_data.py first")

    def test_reproducible_same_seed(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            sa = sim(a, "--strategy", "adaptive", "--seed", "11", "--loss", "0.1")
            sb = sim(b, "--strategy", "adaptive", "--seed", "11", "--loss", "0.1")
            for k in ("host_exec_ms", "host_us_per_sample"):
                sa.pop(k), sb.pop(k)
            self.assertEqual(sa, sb)
            for name in ("gateway.csv", "tx.csv", "samples.csv", "storage_dump.csv"):
                self.assertEqual(digest(os.path.join(a, name)), digest(os.path.join(b, name)), name)

    def test_trace_generation_deterministic(self):
        prof = generate_data.dump_profile("air")
        cfg = generate_data.load_config()
        t1 = generate_data.make_trace(prof, "noisy", cfg["profiles"]["air"], cfg["seed"], hours=1)
        t2 = generate_data.make_trace(prof, "noisy", cfg["profiles"]["air"], cfg["seed"], hours=1)
        for x, y in zip(t1, t2):
            self.assertTrue((x == y).all())

    def test_outage_store_and_forward(self):
        with tempfile.TemporaryDirectory() as d:
            s = sim(d, "--strategy", "adaptive", "--loss", "0")
            self.assertGreaterEqual(s["outages_detected"], 1)
            self.assertGreater(s["recovered_after_outage"], 0)
            self.assertEqual(s["packets_in_queue_at_end"], 0)
            self.assertEqual(s["packets_dropped_queue"], 0)
            with open(os.path.join(d, "gateway.csv")) as f:
                rows = list(csv.DictReader(f))
            stored = [r for r in rows if r["stored"] == "1"]
            self.assertTrue(stored, "gateway must see packets flagged as stored")
            self.assertTrue(any(r["node_state"] == "2" for r in stored), "critical alert delivered after outage")
            # every raw sample is retrievable from local storage
            self.assertEqual(s["storage_retrieved"], s["samples"])

    def test_fixed_vs_adaptive_same_trace(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            f = sim(a, "--strategy", "fixed")
            ad = sim(b, "--strategy", "adaptive")
            self.assertEqual(f["duration_s"], ad["duration_s"])
            self.assertLess(ad["tx_attempts"], f["tx_attempts"])
            self.assertLess(ad["airtime_ms"], f["airtime_ms"])
            self.assertLess(ad["energy_mas"]["total"], f["energy_mas"]["total"])

    def test_corruption_detected_by_crc(self):
        with tempfile.TemporaryDirectory() as d:
            s = sim(d, "--strategy", "fixed", "--corrupt", "0.2", "--loss", "0")
            self.assertGreater(s["radio_corrupted"], 0)
            self.assertEqual(s["gateway_crc_fail"], s["radio_corrupted"])

    def test_profile_dump_matches_config(self):
        for p in ("water", "air", "soil"):
            prof = generate_data.dump_profile(p)
            self.assertEqual(len(prof["channels"]), 4)
            self.assertLess(prof["sampling"]["alert_s"], prof["sampling"]["stable_s"])

    def test_config_file_option(self):
        with tempfile.TemporaryDirectory() as d:
            ini = os.path.join(d, "node.ini")
            with open(ini, "w") as f:
                f.write(f"profile = soil\nstrategy = fixed\ntrace = {os.path.join(ROOT, 'experiments', 'traces', 'soil_stable.csv')}\n"
                        f"out = {d}\nseed = 3\n")
            r = subprocess.run([EXE, "--config", ini], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            with open(os.path.join(d, "summary.json")) as f:
                s = json.load(f)
            self.assertEqual((s["profile"], s["strategy"], s["seed"]), ("soil", "fixed", 3))


def run_raw(*args):
    return subprocess.run([EXE, *args], capture_output=True, text=True)


class InputValidation(unittest.TestCase):
    """Malformed traces and out-of-range options must fail with a clear message."""

    HEADER = "t,v0,v1,v2,v3,f0,f1,f2,f3,link\n"
    ROW = "{t},7.4,15,450,26,0,0,0,0,1\n"

    def run_trace(self, text):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "trace.csv")
            with open(path, "w") as f:
                f.write(text)
            return run_raw("--profile", "water", "--trace", path, "--out", d)

    def good_rows(self, n=5):
        return "".join(self.ROW.format(t=10 * i) for i in range(n))

    def test_valid_trace_runs(self):
        self.assertEqual(self.run_trace(self.HEADER + self.good_rows()).returncode, 0)

    def test_empty_value_is_a_missing_reading(self):
        text = self.HEADER + self.good_rows(2) + "20,,15,450,26,0,0,0,0,1\n"
        self.assertEqual(self.run_trace(text).returncode, 0)

    def test_malformed_traces_rejected(self):
        cases = {
            "short row": self.HEADER + self.good_rows(2) + "20,7.4,15\n",
            "text in number": self.HEADER + self.good_rows(2) + "20,abc,15,450,26,0,0,0,0,1\n",
            "bad fault code": self.HEADER + self.good_rows(2) + "20,7.4,15,450,26,7,0,0,0,1\n",
            "bad link": self.HEADER + self.good_rows(2) + "20,7.4,15,450,26,0,0,0,0,2\n",
            "uneven time": self.HEADER + self.good_rows(2) + "25,7.4,15,450,26,0,0,0,0,1\n",
            "no time column": "v0,v1\n1,2\n3,4\n",
            "one row": self.HEADER + self.good_rows(1),
            "empty file": "",
        }
        for name, text in cases.items():
            with self.subTest(name):
                r = self.run_trace(text)
                self.assertEqual(r.returncode, 2, r.stdout)
                self.assertTrue(r.stderr.strip(), "an error message is expected")

    def row(self, t, v0="7.4", v3="26"):
        return f"{t},{v0},15,450,{v3},0,0,0,0,1\n"

    def test_non_finite_and_out_of_range_rejected(self):
        h, ok = self.HEADER, self.row(0) + self.row(10)
        cases = {
            "inf timestamp": h + ok + self.row("inf"),
            "-inf timestamp": h + ok + self.row("-inf"),
            "nan timestamp": h + ok + self.row("nan"),
            "inf sensor value": h + ok + self.row(20, v0="inf"),
            "-inf sensor value": h + ok + self.row(20, v0="-inf"),
            "nan sensor value": h + ok + self.row(20, v0="nan"),
            "value beyond float range": h + ok + self.row(20, v0="1e300"),
            "huge timestamp": h + ok + self.row(10000000000000),
            "step beyond uint32 range": h + self.row(0) + self.row(5000000000),
            "duration beyond range": h + self.row(0) + self.row(2147483647),
            "fractional step": h + self.row(0) + self.row(0.5),
            "first time not zero": h + self.row(100) + self.row(110),
            "negative time": h + self.row(-10) + self.row(0),
        }
        for name, text in cases.items():
            with self.subTest(name):
                r = self.run_trace(text)
                self.assertEqual(r.returncode, 2, r.stdout)
                self.assertRegex(r.stderr, r"trace\.csv:[0-9]+: ", "error should name the line")

    def test_negative_reading_and_empty_field_still_accepted(self):
        text = self.HEADER + self.row(0) + self.row(10, v3="-5.5") + self.row(20, v0="")
        r = self.run_trace(text)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_option_ranges(self):
        for opt, val in (("--loss", "1.5"), ("--corrupt", "-0.1"), ("--sf", "13"), ("--storage-cap", "0"),
                         ("--node-id", "70000")):
            with self.subTest(opt):
                r = run_raw("--profile", "water", "--trace", TRACE, opt, val)
                self.assertEqual(r.returncode, 2)


class GatewayView(unittest.TestCase):
    def test_live_view_uses_reception_time(self):
        import numpy as np
        ts = np.array([0.0, 100.0, 200.0])        # sample times
        t_rx = np.array([0.0, 500.0, 210.0])      # packet 2 was delayed (store-and-forward)
        v = np.array([1.0, 2.0, 3.0])
        grid = np.array([50.0, 150.0, 220.0, 600.0])
        live = analyse_results.gateway_view(grid, ts, t_rx, v, "live")
        offline = analyse_results.gateway_view(grid, ts, t_rx, v, "offline")
        # at 150 s the gateway has not received the t=100 sample yet
        self.assertEqual(list(live), [1.0, 1.0, 3.0, 3.0])   # late, older sample never overwrites newer
        self.assertEqual(list(offline), [1.0, 2.0, 3.0, 3.0])


if __name__ == "__main__":
    unittest.main()
