"""Generate deterministic scenario traces (input to the virtual sensor backend).

Each trace CSV has, per row (dt_s resolution):
  t, v0..v3 (measured physical value = truth + noise [+ spikes]),
  f0..f3 (0 ok, 1 missing/no response, 2 stuck/failed), link (1 = radio link up),
  tv0..tv3 (noise-free truth, used ONLY by the analysis for ground truth).

Thresholds/hysteresis are read from the firmware itself (sensor_node --dump-profile),
so the scenario generator and firmware cannot silently disagree.

Usage:
  python python/generate_data.py                       # all profiles x scenarios
  python python/generate_data.py --emit-c-header firmware/targets/stm32f4/trace_data.h \
         --profile water --scenario sudden --hours 3 --dt 5
"""
import argparse
import json
import os
import subprocess
import sys
import zlib

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "sensor_node.exe" if os.name == "nt" else "sensor_node")
CFG = os.path.join(ROOT, "experiments", "configs", "experiment_config.json")
TRACE_DIR = os.path.join(ROOT, "experiments", "traces")


def load_config():
    with open(CFG) as f:
        return json.load(f)


def dump_profile(name):
    out = subprocess.run([EXE, "--dump-profile", name], capture_output=True, text=True, check=True).stdout
    return json.loads(out)


def event_target(ch, nominal, direction):
    """Value 15 % beyond the critical threshold on the requested side."""
    if direction > 0:
        lim = ch["crit_hi"]
        return lim + 0.15 * (lim - nominal)
    lim = ch["crit_lo"]
    return lim - 0.15 * (nominal - lim)


def ramp(t, a, b):
    return np.clip((t - a) / max(b - a, 1e-9), 0.0, 1.0)


def make_trace(profile, scenario, pcfg, seed, hours=None, dt=None):
    hours = hours or pcfg["duration_h"]
    dt = dt or pcfg["dt_s"]
    D = int(hours * 3600)
    t = np.arange(0, D, dt, dtype=np.int64)
    n = len(t)
    rng = np.random.default_rng(seed + zlib.crc32(f"{profile['name']}/{scenario}".encode()))
    chans = profile["channels"]
    nom = np.array(pcfg["nominal"], dtype=float)
    noise = np.array(pcfg["noise"], dtype=float)
    diu = np.array(pcfg["diurnal"], dtype=float)

    truth = np.zeros((4, n))
    for i, ch in enumerate(chans):
        if ch["name"] == "light":     # half-wave solar day, nominal = peak lux
            truth[i] = nom[i] * np.clip(np.sin(2 * np.pi * (t / 86400.0 - 0.25)), 0, None)
        else:
            truth[i] = nom[i] + diu[i] * np.sin(2 * np.pi * t / 86400.0 - np.pi / 2)

    faults = np.zeros((4, n), dtype=np.uint8)
    link = np.ones(n, dtype=np.uint8)
    ec = pcfg["event_channel"]
    sig = noise.copy()
    spikes = np.zeros((4, n))

    def apply_event(shape, direction):
        tgt = event_target(chans[ec], nom[ec], direction)
        truth[ec] = truth[ec] + shape * (tgt - truth[ec])

    if scenario == "gradual":
        shape = ramp(t, 0.25 * D, 0.50 * D) * (1 - ramp(t, 0.60 * D, 0.85 * D))
        apply_event(shape, pcfg["event_dir"])
    elif scenario == "sudden":
        on, off = 0.40 * D, 0.48 * D
        shape = np.where((t >= on) & (t < off), 1.0, 0.0)
        tau = 0.01 * D
        shape = np.where(t >= off, np.exp(-(t - off) / tau), shape)
        apply_event(shape, pcfg["sudden_dir"])
    elif scenario == "noisy":
        sig = noise * 4.0
        hit = rng.random(n) < 0.01
        mag = 1.3 * abs(event_target(chans[ec], nom[ec], pcfg["event_dir"]) - nom[ec])
        spikes[ec][hit] = pcfg["event_dir"] * mag          # isolated single-row spikes
    elif scenario == "sensor_failure":
        fc = pcfg["fault_channel"]
        faults[fc][(t >= 0.30 * D) & (t < 0.45 * D)] = 2     # stuck / failed
        faults[fc][(t >= 0.60 * D) & (t < 0.65 * D)] = 1     # disconnected
    elif scenario == "missing":
        mc = pcfg["missing_channel"]
        win = (t >= 0.20 * D) & (t < 0.80 * D)
        faults[mc][win & (rng.random(n) < 0.20)] = 1         # 20 % random missing readings
        faults[mc][(t >= 0.50 * D) & (t < 0.55 * D)] = 1     # continuous dropout
    elif scenario == "outage":
        link[(t >= 0.35 * D) & (t < 0.55 * D)] = 0
        shape = np.where((t >= 0.42 * D) & (t < 0.48 * D), 1.0, 0.0)
        apply_event(shape, pcfg["sudden_dir"])
    elif scenario == "recovery":
        d = pcfg["event_dir"]
        ch = chans[ec]
        crit = ch["crit_hi"] if d > 0 else ch["crit_lo"]
        tgt = event_target(ch, nom[ec], d)
        base = truth[ec].copy()
        rise = ramp(t, 0.30 * D, 0.31 * D)
        v = base + rise * (tgt - base)
        osc = (t >= 0.40 * D) & (t < 0.55 * D)               # oscillate around the critical limit
        v[osc] = crit + d * 0.6 * ch["hyst"] * np.sin(2 * np.pi * (t[osc] - 0.40 * D) / 1200.0)
        back = t >= 0.55 * D
        v[back] = crit + (base[back] - crit) * ramp(t[back], 0.55 * D, 0.70 * D)
        truth[ec] = v
    elif scenario != "stable":
        raise ValueError(scenario)

    meas = truth + rng.normal(0, 1, (4, n)) * sig[:, None] + spikes
    for i, ch in enumerate(chans):
        lo = ch["range_min"] if ch["range_min"] is not None else -1e9
        hi = ch["range_max"] if ch["range_max"] is not None else 1e9
        meas[i] = np.clip(meas[i], lo, hi)
        if ch["name"] == "light":
            meas[i] = np.clip(meas[i], 0, None)
    return t, meas, faults, link, truth


def write_csv(path, t, meas, faults, link, truth):
    cols = [t] + [meas[i] for i in range(4)] + [faults[i] for i in range(4)] + [link] + [truth[i] for i in range(4)]
    header = "t,v0,v1,v2,v3,f0,f1,f2,f3,link,tv0,tv1,tv2,tv3"
    fmt = ["%d"] + ["%.5f"] * 4 + ["%d"] * 5 + ["%.5f"] * 4
    np.savetxt(path, np.column_stack(cols), delimiter=",", header=header, comments="", fmt=fmt)


def emit_c_header(path, profile, scenario, t, meas, faults, link):
    n = len(t)
    dt = int(t[1] - t[0])
    with open(path, "w") as f:
        f.write(f"/* AUTO-GENERATED by python/generate_data.py - profile {profile}, scenario {scenario}.\n"
                f" * {n} rows at {dt} s. Virtual sensor input for the ARM/Renode build. */\n")
        f.write("#ifndef TRACE_DATA_H\n#define TRACE_DATA_H\n#include <stdint.h>\n")
        f.write(f"#define TRACE_ROWS {n}u\n#define TRACE_DT_S {dt}u\n#define TRACE_PROFILE_NAME \"{profile}\"\n")
        for i in range(4):
            f.write(f"static const float TRACE_V{i}[TRACE_ROWS] = {{")
            f.write(",".join(f"{x:.4f}f" for x in meas[i]))
            f.write("};\n")
            f.write(f"static const uint8_t TRACE_F{i}[TRACE_ROWS] = {{" + ",".join(str(int(x)) for x in faults[i]) + "};\n")
        f.write("static const uint8_t TRACE_LINK[TRACE_ROWS] = {" + ",".join(str(int(x)) for x in link) + "};\n")
        f.write("#endif\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile")
    ap.add_argument("--scenario")
    ap.add_argument("--hours", type=float)
    ap.add_argument("--dt", type=int)
    ap.add_argument("--emit-c-header")
    a = ap.parse_args()
    if not os.path.exists(EXE):
        sys.exit("build/sensor_node.exe not found - run build.bat first")
    cfg = load_config()
    os.makedirs(TRACE_DIR, exist_ok=True)
    profiles = [a.profile] if a.profile else list(cfg["profiles"])
    scenarios = [a.scenario] if a.scenario else cfg["scenarios"]
    for p in profiles:
        prof = dump_profile(p)
        for s in scenarios:
            t, meas, faults, link, truth = make_trace(prof, s, cfg["profiles"][p], cfg["seed"], a.hours, a.dt)
            if a.emit_c_header:
                emit_c_header(a.emit_c_header, p, s, t, meas, faults, link)
                print("wrote", a.emit_c_header)
                return
            path = os.path.join(TRACE_DIR, f"{p}_{s}.csv")
            write_csv(path, t, meas, faults, link, truth)
            print(f"wrote {os.path.relpath(path, ROOT)} ({len(t)} rows)")


if __name__ == "__main__":
    main()
