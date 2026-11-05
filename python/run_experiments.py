"""Run the experiment matrix with the native firmware simulator.

profiles x scenarios x strategies (identical trace + seed for every strategy),
plus two small sweeps (adaptive stable interval, packet-loss rate).
Raw per-run outputs go to experiments/results/runs/ (git-ignored, regenerable).
"""
import json
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "sensor_node.exe" if os.name == "nt" else "sensor_node")
CFG = os.path.join(ROOT, "experiments", "configs", "experiment_config.json")
TRACES = os.path.join(ROOT, "experiments", "traces")
RUNS = os.path.join(ROOT, "experiments", "results", "runs")


def run(profile, scenario, strategy, out, cfg, extra=None):
    os.makedirs(out, exist_ok=True)
    r = cfg["radio"]
    cmd = [EXE, "--profile", profile, "--strategy", strategy,
           "--trace", os.path.join(TRACES, f"{profile}_{scenario}.csv"), "--out", out,
           "--seed", str(cfg["seed"]), "--loss", str(r["loss"]), "--ack-loss", str(r["ack_loss"]),
           "--corrupt", str(r["corrupt"]), "--sf", str(r["sf"])]
    if extra:
        for k, v in extra.items():
            cmd += [f"--{k}", str(v)]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit(f"run failed: {' '.join(cmd)}\n{res.stderr}")
    with open(os.path.join(out, "run_cmd.txt"), "w") as f:
        f.write(" ".join(os.path.relpath(c, ROOT) if os.path.isabs(c) else c for c in cmd) + "\n")
    return res.stdout.strip()


def main():
    if not os.path.exists(EXE):
        sys.exit("build first (build.bat)")
    with open(CFG) as f:
        cfg = json.load(f)
    t0 = time.time()
    n = 0
    for p in cfg["profiles"]:
        for s in cfg["scenarios"]:
            if not os.path.exists(os.path.join(TRACES, f"{p}_{s}.csv")):
                sys.exit("traces missing - run python/generate_data.py")
            for st in cfg["strategies"]:
                run(p, s, st, os.path.join(RUNS, "main", p, s, st), cfg)
                n += 1
    sw = cfg["sweeps"]
    prof_stable = {p: json.loads(subprocess.run([EXE, "--dump-profile", p], capture_output=True, text=True).stdout)
                   ["sampling"]["stable_s"] for p in cfg["profiles"]}
    for p in cfg["profiles"]:
        for fac in sw["stable_interval_factor"]:
            st_s = int(prof_stable[p] * fac)
            run(p, sw["stable_sweep_scenario"], "adaptive",
                os.path.join(RUNS, "sweep_stable", p, f"x{fac}"), cfg, {"stable-s": st_s})
            n += 1
    for loss in sw["loss"]:
        c2 = json.loads(json.dumps(cfg))
        c2["radio"]["loss"] = loss
        c2["radio"]["ack_loss"] = loss
        for st in ["fixed", "adaptive"]:
            run(sw["loss_sweep_profile"], sw["loss_sweep_scenario"], st,
                os.path.join(RUNS, "sweep_loss", f"loss{loss}", st), c2)
            n += 1
    print(f"{n} simulation runs completed in {time.time() - t0:.1f} s (host wall-clock)")


if __name__ == "__main__":
    main()
