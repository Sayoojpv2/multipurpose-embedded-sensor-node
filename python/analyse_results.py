"""Compute experiment metrics from the simulator outputs.

Ground truth comes from the noise-free columns (tv*) of each trace and the
firmware thresholds; node-side detections from samples.csv; gateway-side
notification from gateway.csv. Writes:
  experiments/results/metrics.csv          one row per run (main matrix)
  experiments/results/sweep_stable.csv     stable-interval sweep
  experiments/results/sweep_loss.csv       packet-loss sweep
  experiments/results/results_summary.md   tables used in the docs
"""
import csv
import json
import os
import subprocess

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "sensor_node.exe" if os.name == "nt" else "sensor_node")
CFG = os.path.join(ROOT, "experiments", "configs", "experiment_config.json")
TRACES = os.path.join(ROOT, "experiments", "traces")
RES = os.path.join(ROOT, "experiments", "results")
RUNS = os.path.join(RES, "runs")


def read_csv(path):
    with open(path) as f:
        rows = list(csv.reader(f))
    hdr = rows[0]
    data = {h: [] for h in hdr}
    for r in rows[1:]:
        for h, v in zip(hdr, r):
            if h == "type":
                data[h].append(v)
            else:
                data[h].append(float(v) if v != "" else np.nan)
    return {h: np.array(v) for h, v in data.items()}


def episodes(times, flags, ends, merge_gap):
    """Contiguous runs of flags==True -> list of [start, end]; merge small gaps."""
    eps = []
    for t, f, e in zip(times, flags, ends):
        if not f:
            continue
        if eps and t - eps[-1][1] <= merge_gap:
            eps[-1][1] = max(eps[-1][1], e)
        else:
            eps.append([t, e])
    return eps


def truth_episodes(trace, prof, dt, merge_gap):
    crit = np.zeros(len(trace["t"]), dtype=bool)
    for i, ch in enumerate(prof["channels"]):
        tv = trace[f"tv{i}"]
        hi = ch["crit_hi"] if ch["crit_hi"] is not None else np.inf
        lo = ch["crit_lo"] if ch["crit_lo"] is not None else -np.inf
        crit |= (tv >= hi) | (tv <= lo)
    eps = episodes(trace["t"], crit, trace["t"] + dt, merge_gap)
    return [e for e in eps if e[1] - e[0] >= 60]


def critical_mask(trace, prof):
    """True where any noise-free channel is at or beyond its critical limit."""
    crit = np.zeros(len(trace["t"]), dtype=bool)
    for i, ch in enumerate(prof["channels"]):
        tv = trace[f"tv{i}"]
        hi = ch["crit_hi"] if ch["crit_hi"] is not None else np.inf
        lo = ch["crit_lo"] if ch["crit_lo"] is not None else -np.inf
        crit |= (tv >= hi) | (tv <= lo)
    return crit


def analyse_run(run_dir, trace, prof, cfg_p, tol, event_merge):
    with open(os.path.join(run_dir, "summary.json")) as f:
        s = json.load(f)
    sm = read_csv(os.path.join(run_dir, "samples.csv"))
    gw = read_csv(os.path.join(run_dir, "gateway.csv"))
    dt = cfg_p["dt_s"]
    tt = trace["t"]

    # Ground truth. A "crossing" is every separate excursion of the noise-free
    # signal beyond a critical limit. An "event" merges crossings that are
    # closer than event_merge seconds: a value hovering at the limit is one
    # environmental event, and the firmware's hysteresis is designed to hold a
    # single alarm through it. Both counts are reported.
    crossings = truth_episodes(trace, prof, dt, max(2 * dt, 60))
    te = truth_episodes(trace, prof, dt, event_merge)

    node_crit = sm["node_state"] == 2
    # Node alarms are sampled sparsely, so merge over two of the longest intervals.
    sample_merge = 2 * max(dt, prof["sampling"]["stable_s"])
    ne = episodes(sm["t"], node_crit, sm["t"] + sm["interval"], sample_merge)

    # An event is detected if the node is CRITICAL at any sample inside
    # [onset - tol, end + tol]. Delay = first such sample - onset; an alarm
    # before the true onset (noise near the limit) is reported as lead time.
    delays, early_leads, detected = [], [], 0
    for ta, tb in te:
        m = node_crit & (sm["t"] >= ta - tol) & (sm["t"] <= tb + tol)
        if m.any():
            detected += 1
            d = sm["t"][m].min() - ta
            (delays if d >= 0 else early_leads).append(abs(d))
    false_eps = sum(1 for a, b in ne if not any(a <= tb + tol and b >= ta - tol for ta, tb in te))

    # Fraction of truly-critical time during which the node was in CRITICAL
    # (node state held between samples).
    crit_rows = critical_mask(trace, prof)
    coverage = np.nan
    if crit_rows.any() and len(sm["t"]):
        idx = np.searchsorted(sm["t"], tt, side="right") - 1
        held = np.where(idx >= 0, sm["node_state"][np.clip(idx, 0, None)], 0)
        coverage = 100.0 * float(np.mean(held[crit_rows] == 2))

    gts = gw.get("ts", np.array([]))
    gw_delays, gw_early_leads = [], []
    if len(gts):
        for ta, tb in te:
            m = (gw["node_state"] == 2) & (gts >= ta - tol) & (gts <= tb + tol)
            if m.any():
                d = gw["t_rx"][m].min() - ta
                (gw_delays if d >= 0 else gw_early_leads).append(abs(d))

    # gateway reconstruction error (sample-and-hold), in units of delta_report
    nrmse = []
    order = np.argsort(gts) if len(gts) else []
    for i, ch in enumerate(prof["channels"]):
        if len(gts) == 0:
            nrmse.append(np.nan)
            continue
        ts = gts[order]
        v = gw[f"v{i}"][order]
        ok = ~np.isnan(v)
        ts, v = ts[ok], v[ok]
        if len(ts) == 0:
            nrmse.append(np.nan)
            continue
        idx = np.searchsorted(ts, tt, side="right") - 1
        valid = (idx >= 0) & (trace[f"f{i}"] == 0)
        rec = v[np.clip(idx, 0, None)]
        err = rec[valid] - trace[f"tv{i}"][valid]
        nrmse.append(float(np.sqrt(np.mean(err ** 2)) / ch["delta_report"]))

    # sensor-fault detection: truth = fault flag on a channel for >= 3 consecutive trace rows
    fault_truth, fault_det, fault_delay = 0, 0, []
    for i in range(len(prof["channels"])):
        fe = episodes(tt, trace[f"f{i}"] > 0, tt + dt, 0)
        fe = [e for e in fe if e[1] - e[0] >= 3 * max(cfg_p["dt_s"], 60)]
        st = sm.get(f"st{i}")
        for ta, tb in fe:
            fault_truth += 1
            m = (st == 4) & (sm["t"] >= ta) & (sm["t"] <= tb + tol)
            if m.any():
                fault_det += 1
                fault_delay.append(sm["t"][m].min() - ta)

    gen = s["packets_generated"]
    lost = s["packets_dropped_queue"] + s["packets_in_queue_at_end"]
    e = s["energy_mas"]
    return {
        "samples": s["samples"],
        "packets_generated": gen,
        "tx_attempts": s["tx_attempts"],
        "packets_delivered": s["packets_delivered"],
        "bytes_phy": s["bytes_air_phy"],
        "airtime_s": round(s["airtime_ms"] / 1000, 3),
        "duty_cycle_pct": s["duty_cycle_pct"],
        "energy_J": round(s["energy_mj_total"] / 1000, 3),
        "e_sensor_J": round(e["sensor"] * 3.3 / 1000, 3),
        "e_radio_J": round((e["tx"] + e["rx"]) * 3.3 / 1000, 3),
        "e_mcu_J": round((e["processing"] + e["idle"] + e["switching"]) * 3.3 / 1000, 3),
        "e_sleep_J": round(e["sleep"] * 3.3 / 1000, 4),
        "avg_current_mA": round(s["avg_current_ma"], 4),
        "battery_days_est": s["projected_battery_days"],
        "truth_crossings": len(crossings),
        "truth_crit_events": len(te),
        "detected_events": detected,
        "missed_events": len(te) - detected,
        "alarm_coverage_pct": round(coverage, 1) if not np.isnan(coverage) else np.nan,
        "false_alarm_episodes": false_eps,
        "false_alarm_rate": round(false_eps / len(ne), 3) if ne else 0.0,
        "node_detect_delay_s": round(float(np.mean(delays)), 1) if delays else np.nan,
        "node_early_warning_s": round(float(np.mean(early_leads)), 1) if early_leads else np.nan,
        "gateway_notify_delay_s": round(float(np.mean(gw_delays)), 1) if gw_delays else np.nan,
        "gateway_early_warning_s": round(float(np.mean(gw_early_leads)), 1) if gw_early_leads else np.nan,
        "fault_episodes": fault_truth,
        "faults_detected": fault_det,
        "fault_detect_delay_s": round(float(np.mean(fault_delay)), 1) if fault_delay else np.nan,
        "data_lost_packets": lost,
        "data_loss_pct": round(100 * lost / gen, 2) if gen else 0.0,
        "raw_records_stored": s["storage_records"],
        "raw_records_overwritten": s["storage_overwritten"],
        "recovered_after_outage": s["recovered_after_outage"],
        "max_queue": s["max_queue"],
        "gateway_duplicates": s["gateway_duplicates"],
        "gateway_crc_fail": s["gateway_crc_fail"],
        "recon_err_event_ch": round(nrmse[cfg_p["event_channel"]], 3),
        "recon_err_mean": round(float(np.nanmean(nrmse)), 3),
        "dsp_mac_ops": s["dsp_mac_ops"],
        "host_us_per_sample": s["host_us_per_sample"],
    }


def write_rows(path, rows):
    keys = list(rows[0].keys())
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        w.writerows(rows)


def md_table(rows, cols):
    out = "| " + " | ".join(cols) + " |\n|" + "---|" * len(cols) + "\n"
    for r in rows:
        out += "| " + " | ".join(str(r[c]) for c in cols) + " |\n"
    return out


def main():
    with open(CFG) as f:
        cfg = json.load(f)
    tol = cfg["detection_tolerance_s"]
    merge = cfg["event_merge_s"]
    profs = {p: json.loads(subprocess.run([EXE, "--dump-profile", p], capture_output=True, text=True).stdout)
             for p in cfg["profiles"]}
    with open(os.path.join(RES, "profiles.json"), "w") as f:
        json.dump(profs, f, indent=1)
    rows = []
    trace_cache = {}
    for p in cfg["profiles"]:
        for s in cfg["scenarios"]:
            tr = read_csv(os.path.join(TRACES, f"{p}_{s}.csv"))
            trace_cache[(p, s)] = tr
            for st in cfg["strategies"]:
                m = analyse_run(os.path.join(RUNS, "main", p, s, st), tr, profs[p], cfg["profiles"][p], tol, merge)
                rows.append({"profile": p, "scenario": s, "strategy": st, **m})
    write_rows(os.path.join(RES, "metrics.csv"), rows)

    sw = cfg["sweeps"]
    srows = []
    for p in cfg["profiles"]:
        tr = trace_cache[(p, sw["stable_sweep_scenario"])]
        for fac in sw["stable_interval_factor"]:
            d = os.path.join(RUNS, "sweep_stable", p, f"x{fac}")
            with open(os.path.join(d, "summary.json")) as f:
                stable_s = json.load(f)["stable_s"]
            m = analyse_run(d, tr, profs[p], cfg["profiles"][p], tol, merge)
            srows.append({"profile": p, "factor": fac, "stable_s": stable_s, **m})
    write_rows(os.path.join(RES, "sweep_stable.csv"), srows)

    lrows = []
    lp, ls = sw["loss_sweep_profile"], sw["loss_sweep_scenario"]
    for loss in sw["loss"]:
        for st in ["fixed", "adaptive"]:
            m = analyse_run(os.path.join(RUNS, "sweep_loss", f"loss{loss}", st), trace_cache[(lp, ls)],
                            profs[lp], cfg["profiles"][lp], tol, merge)
            lrows.append({"loss": loss, "strategy": st, **m})
    write_rows(os.path.join(RES, "sweep_loss.csv"), lrows)

    # ---- aggregate per profile x strategy over the 8 scenarios ----
    agg = []
    for p in cfg["profiles"]:
        for st in cfg["strategies"]:
            sel = [r for r in rows if r["profile"] == p and r["strategy"] == st]
            def tot(k): return sum(r[k] for r in sel)
            def mean(k):
                v = [r[k] for r in sel if not (isinstance(r[k], float) and np.isnan(r[k]))]
                return round(float(np.mean(v)), 1) if v else "n/a"
            agg.append({"profile": p, "strategy": st, "samples": tot("samples"), "tx_attempts": tot("tx_attempts"),
                        "bytes_phy": tot("bytes_phy"), "airtime_s": round(tot("airtime_s"), 1),
                        "energy_J": round(tot("energy_J"), 1), "e_sensor_J": round(tot("e_sensor_J"), 1),
                        "e_radio_J": round(tot("e_radio_J"), 1), "e_mcu_J": round(tot("e_mcu_J"), 1),
                        "e_sleep_J": round(tot("e_sleep_J"), 2),
                        "events_detected": f"{tot('detected_events')}/{tot('truth_crit_events')}",
                        "crossings": tot("truth_crossings"),
                        "alarm_coverage_pct": mean("alarm_coverage_pct"),
                        "false_alarms": tot("false_alarm_episodes"),
                        "faults_detected": f"{tot('faults_detected')}/{tot('fault_episodes')}",
                        "node_delay_s": mean("node_detect_delay_s"), "gw_delay_s": mean("gateway_notify_delay_s"),
                        "lost_pkts": tot("data_lost_packets"), "recovered": tot("recovered_after_outage"),
                        "recon_err": round(float(np.nanmean([r["recon_err_mean"] for r in sel])), 2)})
    write_rows(os.path.join(RES, "aggregate.csv"), agg)

    # ---- relative change vs fixed ----
    rel = []
    for p in cfg["profiles"]:
        f = next(a for a in agg if a["profile"] == p and a["strategy"] == "fixed")
        for st in ["adaptive_tx", "adaptive"]:
            a = next(x for x in agg if x["profile"] == p and x["strategy"] == st)
            rel.append({"profile": p, "strategy": st,
                        "tx_attempts_%": round(100 * (a["tx_attempts"] / f["tx_attempts"] - 1), 1),
                        "bytes_%": round(100 * (a["bytes_phy"] / f["bytes_phy"] - 1), 1),
                        "airtime_%": round(100 * (a["airtime_s"] / f["airtime_s"] - 1), 1),
                        "energy_%": round(100 * (a["energy_J"] / f["energy_J"] - 1), 1),
                        "samples_%": round(100 * (a["samples"] / f["samples"] - 1), 1)})
    write_rows(os.path.join(RES, "relative_vs_fixed.csv"), rel)

    with open(os.path.join(RES, "results_summary.md"), "w") as f:
        f.write("# Experiment results\n\n")
        f.write("Generated by `python/analyse_results.py` from the native simulation. Traces are synthetic, "
                f"generated with one fixed seed ({cfg['seed']}); see `seed_study.md` for a multi-seed check of the "
                "water profile. Energy is an estimate from the model in `firmware/src/energy.c`, not a measurement.\n\n")
        f.write("Totals and means are aggregated over all 8 scenarios of each profile. A true *event* merges "
                f"critical-limit crossings closer than {merge} s; *crossings* counts every excursion separately. "
                f"Detection tolerance: {tol} s.\n\n")
        f.write("## Traffic and estimated energy (8 scenarios per profile)\n\n")
        f.write(md_table(agg, ["profile", "strategy", "samples", "tx_attempts", "bytes_phy", "airtime_s",
                               "energy_J", "e_sensor_J", "e_radio_J", "e_mcu_J"]))
        f.write("\n## Change relative to fixed-period reporting (negative = reduction)\n\n")
        f.write(md_table(rel, list(rel[0].keys())))
        f.write("\n## Detection and delivery (8 scenarios per profile)\n\n")
        f.write(md_table(agg, ["profile", "strategy", "events_detected", "crossings", "alarm_coverage_pct",
                               "false_alarms", "faults_detected", "node_delay_s", "gw_delay_s", "lost_pkts",
                               "recovered", "recon_err"]))
        f.write("\n## Per-scenario detail\n\n")
        cols = ["profile", "scenario", "strategy", "samples", "tx_attempts", "energy_J", "truth_crossings",
                "truth_crit_events", "detected_events", "alarm_coverage_pct", "false_alarm_episodes",
                "node_detect_delay_s", "node_early_warning_s", "gateway_notify_delay_s",
                "faults_detected", "fault_detect_delay_s", "data_lost_packets", "recovered_after_outage",
                "recon_err_mean"]
        f.write(md_table(rows, cols))
        f.write("\n## Stable-interval sweep (adaptive, scenario '%s')\n\n" % sw["stable_sweep_scenario"])
        f.write(md_table(srows, ["profile", "factor", "stable_s", "samples", "tx_attempts", "energy_J",
                                 "node_detect_delay_s", "gateway_notify_delay_s", "recon_err_mean"]))
        f.write("\n## Packet-loss sweep (%s / %s)\n\n" % (lp, ls))
        f.write(md_table(lrows, ["loss", "strategy", "tx_attempts", "packets_delivered", "data_lost_packets",
                                 "recovered_after_outage", "gateway_duplicates", "energy_J", "gateway_notify_delay_s"]))
    print("wrote metrics.csv, aggregate.csv, relative_vs_fixed.csv, sweep_*.csv, results_summary.md")


if __name__ == "__main__":
    main()
