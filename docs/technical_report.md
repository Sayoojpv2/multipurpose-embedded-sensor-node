# Technical report

**Design and Development of a Multipurpose Embedded Environmental Sensor Node with Adaptive Sensing and Energy-Efficient Telemetry**

> Scope statement. Everything in this report comes from a software model: C firmware running natively against virtual sensors (software-in-the-loop), and the same firmware running on an emulated Cortex-M4 in Renode. No physical hardware was built or measured. Energy numbers are estimates from the configurable model in `firmware/src/energy.c`.

## 1. Objective

The goal is one reusable sensor-node firmware that serves three environmental applications (water, air, soil) through configuration only. The question studied is how much radio traffic and estimated energy an adaptive sampling and telemetry policy saves compared with conventional fixed-period reporting, and what that saving costs in detection delay, information at the gateway and fault detection.

## 2. System design (summary)

See `architecture.md`. Key design decisions:

- **Profile = data.** `app_profile_t` holds the sensor list, the drivers, calibration, thresholds and all timing. The common modules contain no profile-specific code. The EC driver is shared between the water and soil profiles with different calibration constants.
- **Real driver path without hardware.** Virtual devices encode physical values into ADC counts, ADS-style registers, MAX31865-style SPI bytes, PMS-style UART frames and Sensirion-style CRC-8 words. The drivers must decode these, which exercises calibration (two-point, Beta, Callendar-Van Dusen), unit conversion, temperature compensation, rail detection, checksum and CRC checks.
- **Processing chain per channel:** a range check, then a Hampel identifier (window 5, k = 3, with a per-channel minimum deviation), then a moving average or a 7-tap binomial FIR, then an event state machine (hysteresis band plus 2-sample confirmation; a fault is declared after 3 consecutive invalid readings).
- **Every sample is stored locally** (circular log with sequence numbers) whatever the telemetry policy sends. In the native build the log is backed by a file (`flash.bin`) and can be dumped (`storage_dump.csv`).

### Strategies compared

| Name | Sampling | Telemetry |
|---|---|---|
| `fixed` (baseline) | every `fixed_s` | DATA packet with all channels on every sample |
| `adaptive_tx` (ablation) | every `fixed_s` | adaptive policy below |
| `adaptive` (proposed) | STABLE / ACTIVE / ALERT / RECOVERY intervals | adaptive policy below |

**Adaptive telemetry policy:** an immediate ALERT when a channel becomes critical; a repeated ALERT every `crit_repeat_s` while critical; an UPDATE or FAULT packet on any channel state change; a delta UPDATE carrying only the changed channels (rate-limited by `min_update_s`); and a SUMMARY (mean/min/max) every `summary_s` when nothing else was sent.

**Adaptive sampling cues:** the node state; a pending (unconfirmed) warning or critical level; a raw value beyond a threshold; a sample the Hampel detector flagged as suspect; and a rate of change above `roc_per_min`. A suspect or threshold-crossing sample triggers burst sampling to confirm it quickly. The node returns to STABLE only after `stable_confirm` calm samples.

**Reliability layer (identical for all strategies):** each packet needs an ACK, with up to 3 retries and 2–6 s random backoff. After a failed send the link is marked down; the node probes again with exponential backoff (120 s → 1800 s), capped at 300 s while a CRITICAL packet is queued, and an alert generated during an outage triggers an immediate probe. Packets wait in a 128-entry priority queue; on overflow the lowest-priority, oldest packet is evicted. Packets sent more than 30 s after creation carry the `stored` flag.

| Profile | fixed | stable | active | alert | recovery | summary | min update | alert repeat |
|---|---|---|---|---|---|---|---|---|
| water | 60 s | 300 s | 60 s | 15 s | 60 s | 900 s | 60 s | 120 s |
| air | 60 s | 180 s | 60 s | 30 s | 60 s | 600 s | 60 s | 60 s |
| soil | 300 s | 1800 s | 300 s | 60 s | 300 s | 3600 s | 300 s | 600 s |

## 3. Energy model

Charge is accumulated in mA·s and converted at 3.3 V.

| Parameter | Value | Source |
|---|---|---|
| Radio TX current | 87 mA (20 dBm, 868 MHz) | RAK3172 datasheet |
| Radio RX current | 5.22 mA | RAK3172 datasheet |
| Sleep current (module) | 1.69 µA | RAK3172 datasheet |
| MCU active | 4.0 mA | **assumption** |
| MCU idle while sensors convert | 1.0 mA | **assumption** |
| Processing time | 3 ms per sample + 2 ms per packet | **assumption** |
| Sensor power-up transient | 20 µC per switch-on | **assumption** |
| LoRa PHY | SF9, BW 125 kHz, CR 4/5, preamble 8, CRC on, +13 B MAC overhead | Semtech time-on-air formula |
| RX when no ACK | 2 × 30 ms window | **assumption** |
| Battery for lifetime projection | 2600 mAh | **assumption** |
| Sensor currents and warm-up times | see the `pwr` fields in `profiles/*.c` (for example PM2.5: 80 mA, 30 s warm-up) | **assumptions**, representative of each sensor class, not from a specific datasheet |

The model counts: sensor on-time (warm-up + measure, or the whole interval if the sensor stays powered because the next sample comes sooner than its warm-up), switch-on transients, MCU idle during conversion, processing, TX airtime per attempt, ACK reception or RX windows, and sleep for the remaining time. The ARM build uses the same parameters.

## 4. Experiments

- **Scenarios** (`python/generate_data.py`, seed 42): stable, gradual (ramp beyond critical and back), sudden (step beyond critical for 8 % of the run), noisy (4× noise plus isolated spikes beyond critical), sensor_failure (stuck, then disconnected), missing (20 % random missing readings plus a dropout), outage (radio down for 20 % of the run with a critical event inside it), and recovery (critical event, then oscillation around the critical limit at 0.6 × the hysteresis band, then return).
- **Durations:** water and air 12 h at 1 s trace resolution; soil 48 h at 10 s resolution.
- **Channel:** 2 % uplink loss, 2 % ACK loss, 0.5 % bit corruption. The run seed drives the loss model.
- **Identical traces** were used for all strategies: 3 profiles × 8 scenarios × 3 strategies = 72 runs, plus a stable-interval sweep (12 runs) and a loss sweep (6 runs).
- **Ground truth:** critical episodes come from the noise-free trace and the firmware thresholds (merged if closer than 5 % of the run, minimum 60 s). A node detection counts if it starts within ±600 s of a true episode; any other node critical episode is a false alarm. Reconstruction error is the RMSE of the gateway's sample-and-hold picture against the noise-free truth, divided by the channel's `delta_report`.

## 5. Results

Generated files: `experiments/results/results_summary.md`, `metrics.csv`, `aggregate.csv`, `sweep_*.csv`, and the plots in `experiments/results/plots/`.

### 5.1 Totals over the 8 scenarios

| Profile | Strategy | Samples | TX attempts | Airtime [s] | Energy [J] (est.) | Detected / true critical | False alarms | Mean node detection delay [s] | Packets lost |
|---|---|---|---|---|---|---|---|---|---|
| water | fixed | 5760 | 6136 | 1765.6 | 1042.6 | 4/4 | 0 | 314.8 | 19 |
| water | adaptive_tx | 5760 | 747 | 234.2 | 589.8 | 4/4 | 0 | 314.8 | 0 |
| water | adaptive | 3481 | 742 | 232.5 | 384.2 | 4/4 | 0 | 104.8 | 0 |
| air | fixed | 5760 | 6142 | 1767.3 | 49695.2 | 4/4 | 0 | 309.0 | 21 |
| air | adaptive_tx | 5760 | 1210 | 367.8 | 49281.5 | 4/4 | 0 | 309.0 | 0 |
| air | adaptive | 3397 | 1127 | 344.1 | 28813.1 | 4/4 | 0 | 234.0 | 0 |
| soil | fixed | 4608 | 5068 | 1458.3 | 559.7 | 4/4 | 0 | 1457.5 | 0 |
| soil | adaptive_tx | 4608 | 1136 | 327.6 | 225.4 | 4/4 | 0 | 1457.5 | 0 |
| soil | adaptive | 2716 | 906 | 264.5 | 157.5 | 4/4 | 0 | 542.5 | 0 |

Relative to `fixed`, the `adaptive` strategy changed radio transmissions by −87.9 % (water), −81.7 % (air) and −82.1 % (soil); airtime by −86.8 %, −80.5 % and −81.9 %; and estimated energy by −63.1 %, −42.0 % and −71.9 %. The ablation `adaptive_tx` (fixed sampling) gives almost the same radio reduction, but much less energy reduction: −43.4 %, −0.8 % and −59.7 %.

All sensor-fault episodes (3 per profile) were detected by every strategy. No critical false alarms occurred in any run, including the noisy scenario with spikes beyond the critical limit; these were rejected by the Hampel stage.

### 5.2 Interpretation

1. **Radio savings come from the telemetry policy; energy savings depend on the sensors.** In the air profile the sensors account for about 98 % of the estimated energy, and almost all of that is the laser PM2.5 sensor (modelled at 80 mA with a 30 s warm-up). Reducing transmissions alone (`adaptive_tx`) therefore saves under 1 % of energy there; only sampling less often helps. In the soil profile, where the sensors are cheap, the radio dominates and `adaptive_tx` already saves about 60 %.
2. **Burst sampling cut detection delay compared with the baseline** in these scenarios. A suspect or threshold-crossing sample switches to the alert interval, so the Hampel and confirmation stages finish quickly. Examples for the 'sudden' scenario: water 165 s vs 420 s; air 210 s vs 420 s; soil 600 s vs 2280 s.
3. **Outage behaviour.** With fixed reporting, a 2.4 h outage overflowed the 128-packet queue (19–21 packets of the lowest priority were evicted). The raw samples are still in local storage, but they were not delivered. The adaptive strategies queued far fewer packets and lost none. In every strategy the critical alert raised during the outage was delivered after reconnection, flagged as `stored`. Gateway notification delay in the water outage scenario: fixed 5736 s, adaptive 5886 s; the minimum possible is about 5616 s, the remaining outage time after the event started.
4. **Packet-loss sweep** (water, outage): at 30 % loss, fixed needed 1660 transmissions (308 duplicates at the gateway) and adaptive needed 242 (33 duplicates). Every generated packet was eventually delivered except the 19 evicted from the fixed queue.

### 5.3 Costs and trade-offs (reported, not hidden)

- **Sensor-fault detection is slower with adaptive sampling,** because a fault needs 3 consecutive invalid samples at the current (possibly long) interval. Soil: 4890 s for adaptive vs 690 s for fixed; water: 690 s vs 120 s. A possible fix is to drop to the active interval on the first invalid reading.
- **Oscillating events cost samples.** In the 'recovery' scenario the adaptive node stays in alert or recovery mode and takes *more* samples than fixed (water 880 vs 720, soil 837 vs 576), but still sends 4–5× fewer packets and uses less energy.
- **The gateway sees less detail.** The mean reconstruction error rises (soil: 0.13 → 0.42 of `delta_report`) but stays below 1 reporting tolerance. Full-resolution data remains available from the local log.
- **Stable-interval sweep** (sudden scenario): longer stable intervals save energy but delay detection. Water: 150 s → 54.6 J / 165 s; 300 s → 43.2 J / 165 s; 600 s → 34.5 J / 465 s; 1200 s → 27.5 J / 1065 s.
- **Duty cycle:** the fixed baseline uses about 0.49 % airtime (water, air) at SF9, within a 1 % regional limit. The model reports duty cycle but does not enforce it.

### 5.4 Processing load and emulation

- The host execution time is about 3–9 µs per node wake-up on an x86 PC. This is a host timing, not MCU timing, and the clock resolution is coarse.
- A platform-independent proxy is the DSP multiply-accumulate count reported in `summary.json`.
- **Renode (STM32F407 model, Cortex-M4, soft-float, `-O2`):** the image uses 69,804 B flash (about 45 KB of it is the embedded trace) and 35,908 B RAM (data + bss). Running the 3 h water/sudden trace under both strategies (282 wake-ups in total, including UART printing) executed **11,068,739 instructions**, about 39 k instructions per wake-up including soft-float and console output. For that trace the ARM results (samples, packets, TX attempts, deliveries, airtime, CRC failures) are **identical** to the native build; energy differs only in the last printed digit because of floating-point rounding.

## 6. Verification

- `tests/test_firmware.c`: 172 checks covering profile selection, registration and probing, every driver's decoding, ADC and calibration (including a deliberately wrong calibration), invalid readings (rail, NACK, UART timeout, checksum, CRC-8), circular storage, queue overflow policy, MA/FIR/Hampel, the event FSM with hysteresis (and the same oscillation without hysteresis, which chatters), adaptive sampling, packet round-trip, detection of every single-bit error by CRC-16, LoRa airtime against reference values (46.336 ms SF7 and 1155.072 ms SF12 for 13 B), outage store-and-forward, gateway duplicate suppression, fixed vs adaptive, and reproducibility.
- `tests/test_pipeline.py`: 7 end-to-end tests (bit-identical outputs for the same seed, deterministic traces, outage recovery, gateway CRC count equal to injected corruption, config-file option).
- `matlab/validate_c_output.m` (MATLAB R2026a): the C filter outputs match an independent MATLAB reference (`filter()` and a reference Hampel) with max |error| of 5.4e-6 (MA), 8.6e-7 (FIR) and 2.4e-7 (Hampel), at single-precision rounding level.

Two real bugs were found by the tests and fixed (both in the virtual backend): loading "no board" kept the previous board's devices, and the EC device ignored its own fault flag.

## 7. Limitations

- No physical hardware. Sensors, the radio channel and the gateway are models; the Renode run emulates the CPU, memory and USART only.
- Energy figures are estimates. Only the three radio currents come from a datasheet; the rest are documented assumptions. MCU timing is not measured.
- No LoRaWAN stack (no join, ADR, MAC commands, or real RX1/RX2 timing). Airtime uses the Semtech formula; ACKs are an application-level abstraction.
- The traces are synthetic. Thresholds are demonstration values, not regulatory limits.
- Truth critical episodes are few (4 per profile), so the detection statistics are indicative, not statistically strong.
- Sampling and filtering are per sample, not time-weighted, so the filters behave differently when the interval changes.
- Duty-cycle limits are reported, not enforced.

## 8. Future work

- Port `hal_virtual.c` to a real STM32WL/RAK3172 board and measure current with a power analyser.
- Add fault-aware sampling (re-check sooner on the first invalid reading).
- Compare against an ML-based adaptation such as that suggested by [P3], once the rule-based baseline is characterised.
- Enforce duty cycle and add a LoRaWAN stack (for example LoRaMac-node, after a licence check).
