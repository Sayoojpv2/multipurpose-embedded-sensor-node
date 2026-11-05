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


if __name__ == "__main__":
    unittest.main()
