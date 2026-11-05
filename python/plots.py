"""Plots from the real experiment outputs (experiments/results/*.csv, runs/)."""
import csv
import json
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "experiments", "results")
OUT = os.path.join(RES, "plots")
TRACES = os.path.join(ROOT, "experiments", "traces")

# validated categorical palette (fixed order) + text/grid tokens
C = {"fixed": "#2a78d6", "adaptive_tx": "#eb6834", "adaptive": "#1baf7a"}
STRAT_LABEL = {"fixed": "Fixed period", "adaptive_tx": "Adaptive telemetry only", "adaptive": "Adaptive sampling + telemetry"}
BREAK = [("e_sensor_J", "Sensors", "#2a78d6"), ("e_radio_J", "Radio TX/RX", "#eb6834"),
         ("e_mcu_J", "MCU + idle", "#1baf7a"), ("e_sleep_J", "Sleep", "#eda100")]
INK, INK2, GRID, SURF = "#0b0b0b", "#52514e", "#e4e3df", "#fcfcfb"

plt.rcParams.update({"font.size": 9, "axes.edgecolor": GRID, "axes.labelcolor": INK2, "xtick.color": INK2,
                     "ytick.color": INK2, "axes.titlecolor": INK, "figure.facecolor": SURF, "axes.facecolor": SURF,
                     "axes.spines.top": False, "axes.spines.right": False, "axes.grid": True, "grid.color": GRID,
                     "grid.linewidth": 0.6, "axes.axisbelow": True, "legend.frameon": False})


def rows(name):
    with open(os.path.join(RES, name)) as f:
        return list(csv.DictReader(f))


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return np.nan


def save(fig, name):
    os.makedirs(OUT, exist_ok=True)
    fig.savefig(os.path.join(OUT, name), dpi=150, bbox_inches="tight")
    plt.close(fig)
    print("wrote", os.path.relpath(os.path.join(OUT, name), ROOT))


def relative_plot(agg):
    profiles = list(dict.fromkeys(r["profile"] for r in agg))
    metrics = [("tx_attempts", "Radio transmissions"), ("airtime_s", "Radio airtime"), ("energy_J", "Estimated energy"),
               ("samples", "Samples acquired")]
    fig, axs = plt.subplots(1, 4, figsize=(12, 3.2), sharey=True)
    w = 0.26
    for ax, (m, title) in zip(axs, metrics):
        for k, st in enumerate(["fixed", "adaptive_tx", "adaptive"]):
            vals = []
            for p in profiles:
                f = num(next(r[m] for r in agg if r["profile"] == p and r["strategy"] == "fixed"))
                v = num(next(r[m] for r in agg if r["profile"] == p and r["strategy"] == st))
                vals.append(100 * v / f)
            x = np.arange(len(profiles)) + (k - 1) * (w + 0.02)
            ax.bar(x, vals, w, color=C[st], label=STRAT_LABEL[st])
            for xi, v in zip(x, vals):
                if st != "fixed":
                    ax.text(xi, v + 2, f"{v:.0f}", ha="center", va="bottom", fontsize=7, color=INK2)
        ax.set_xticks(range(len(profiles)), profiles)
        ax.set_title(title, fontsize=9, loc="left")
        ax.set_ylim(0, 118)
    axs[0].set_ylabel("% of fixed-period strategy")
    axs[0].legend(loc="upper left", bbox_to_anchor=(0, -0.12), ncol=3, fontsize=8)
    fig.suptitle("Totals over 8 scenarios per profile, relative to fixed-period reporting (=100 %)", x=0.01, ha="left", color=INK)
    save(fig, "relative_vs_fixed.png")


def energy_breakdown(agg):
    profiles = list(dict.fromkeys(r["profile"] for r in agg))
    fig, axs = plt.subplots(1, 3, figsize=(11, 3.4))
    for ax, p in zip(axs, profiles):
        sel = [r for r in agg if r["profile"] == p]
        bottom = np.zeros(len(sel))
        for key, lab, col in BREAK:
            v = np.array([num(r.get(key, 0)) for r in sel])
            ax.bar(range(len(sel)), v, 0.6, bottom=bottom, color=col, label=lab, edgecolor=SURF, linewidth=1)
            bottom += v
        for i, b in enumerate(bottom):
            ax.text(i, b * 1.01, f"{b:.0f} J", ha="center", va="bottom", fontsize=7, color=INK2)
        ax.set_xticks(range(len(sel)), ["fixed", "adaptive\ntelemetry", "adaptive\nsamp.+tel."])
        ax.set_title(f"{p} profile", fontsize=9, loc="left")
        ax.set_ylim(0, bottom.max() * 1.15)
    axs[0].set_ylabel("Estimated energy, 8 scenarios [J]")
    axs[0].legend(loc="upper left", bbox_to_anchor=(0, -0.2), ncol=4, fontsize=8)
    fig.suptitle("Energy by component (software model estimate, not measured)", x=0.01, ha="left", color=INK)
    save(fig, "energy_breakdown.png")


def read_csv_np(path):
    with open(path) as f:
        r = list(csv.DictReader(f))
    return {k: np.array([num(x[k]) if k != "type" else x[k] for x in r]) for k in (r[0].keys() if r else [])}


def timeline(profile, scenario, ch, thr_key, fname):
    tr = read_csv_np(os.path.join(TRACES, f"{profile}_{scenario}.csv"))
    prof = json.loads(open(os.path.join(RES, "profiles.json")).read())[profile]
    chan = prof["channels"][ch]
    fig, axs = plt.subplots(2, 1, figsize=(11, 5), sharex=True)
    for ax, st in zip(axs, ["fixed", "adaptive"]):
        run = os.path.join(RES, "runs", "main", profile, scenario, st)
        sm = read_csv_np(os.path.join(run, "samples.csv"))
        gw = read_csv_np(os.path.join(run, "gateway.csv"))
        ax.plot(tr["t"] / 3600, tr[f"tv{ch}"], color=INK2, lw=1, label="true value (noise-free)")
        ax.plot(sm["t"] / 3600, sm[f"val{ch}"], "o", ms=2.5, color=C[st], alpha=0.6, label="samples acquired")
        if len(gw):
            ax.vlines(gw["t_rx"] / 3600, 0, 0.06, transform=ax.get_xaxis_transform(), color=INK, lw=0.6,
                      label="packet received at gateway")
        for k in ("crit_lo", "crit_hi"):
            if chan[k] is not None:
                ax.axhline(chan[k], color="#e34948", lw=1, ls="--")
        crit = sm["node_state"] == 2
        if crit.any():
            ax.fill_between(sm["t"] / 3600, 0, 1, where=crit, step="post", color="#e34948", alpha=0.08,
                            transform=ax.get_xaxis_transform(), label="node state CRITICAL")
        ax.set_title(f"{STRAT_LABEL[st]}: {len(sm['t'])} samples, {len(gw.get('ts', []))} packets delivered",
                     fontsize=9, loc="left")
        ax.set_ylabel(chan["name"] if chan["name"] == chan["unit"] else f"{chan['name']} [{chan['unit']}]")
    axs[0].legend(loc="upper right", fontsize=7, ncol=2)
    axs[1].set_xlabel("time [h]")
    fig.suptitle(f"{profile} profile, '{scenario}' scenario (dashed = demonstration critical threshold)",
                 x=0.01, ha="left", color=INK)
    save(fig, fname)


def delay_plot(metrics):
    sel = [r for r in metrics if r["scenario"] in ("sudden", "gradual", "recovery")]
    profiles = list(dict.fromkeys(r["profile"] for r in metrics))
    fig, axs = plt.subplots(1, 3, figsize=(11, 3.2))
    for ax, sc in zip(axs, ["sudden", "gradual", "recovery"]):
        w = 0.26
        for k, st in enumerate(["fixed", "adaptive_tx", "adaptive"]):
            v = [num(next(r["gateway_notify_delay_s"] for r in sel if r["profile"] == p and r["scenario"] == sc
                          and r["strategy"] == st)) / 60 for p in profiles]
            ax.bar(np.arange(len(profiles)) + (k - 1) * (w + 0.02), v, w, color=C[st], label=STRAT_LABEL[st])
        ax.set_xticks(range(len(profiles)), profiles)
        ax.set_title(f"'{sc}' scenario", fontsize=9, loc="left")
    axs[0].set_ylabel("gateway notification delay [min]")
    axs[0].legend(loc="upper left", bbox_to_anchor=(0, -0.12), ncol=3, fontsize=8)
    fig.suptitle("Time from true critical onset to first CRITICAL packet at the gateway (lower is better)",
                 x=0.01, ha="left", color=INK)
    save(fig, "detection_delay.png")


def sweep_plot(sw):
    profiles = list(dict.fromkeys(r["profile"] for r in sw))
    fig, axs = plt.subplots(1, 3, figsize=(11, 3.2))
    for ax, p in zip(axs, profiles):
        s = [r for r in sw if r["profile"] == p]
        e = [num(r["energy_J"]) for r in s]
        d = [num(r["node_detect_delay_s"]) / 60 for r in s]
        ax.plot(e, d, "-o", color=C["adaptive"], lw=2, ms=6)
        for r, x, y in zip(s, e, d):
            ax.annotate(f"stable {int(num(r['stable_s']))} s", (x, y), textcoords="offset points", xytext=(5, 4),
                        fontsize=7, color=INK2)
        ax.set_title(f"{p} profile", fontsize=9, loc="left")
        ax.set_xlabel("estimated energy [J]")
    axs[0].set_ylabel("node detection delay [min]")
    fig.suptitle("Trade-off: longer stable sampling interval saves energy but delays detection ('sudden' scenario)",
                 x=0.01, ha="left", color=INK)
    save(fig, "stable_interval_tradeoff.png")


def main():
    agg = rows("aggregate.csv")
    relative_plot(agg)
    energy_breakdown(agg)
    delay_plot(rows("metrics.csv"))
    sweep_plot(rows("sweep_stable.csv"))
    timeline("water", "sudden", 0, "crit_lo", "timeline_water_sudden.png")
    timeline("air", "outage", 0, "crit_hi", "timeline_air_outage.png")
    timeline("soil", "gradual", 0, "crit_lo", "timeline_soil_gradual.png")


if __name__ == "__main__":
    main()
