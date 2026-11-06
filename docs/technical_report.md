# Technical report

## 1. Objective and scope

The objective is to design firmware for a single battery-powered environmental sensor node. The firmware serves three applications (water, air, soil) through configuration only, and reduces radio traffic and energy without missing critical events. The evaluation compares conventional fixed-period reporting with adaptive sampling and adaptive telemetry on identical inputs.

Scope:

- **Implemented firmware:** everything under `firmware/` (portable C99, no dynamic memory).
- **Virtual sensors:** `simulation/sensors/`. The sensor physics is replaced by traces; the bus data the drivers decode is realistic.
- **Simulated radio and gateway:** `simulation/radio/`.
- **Estimated energy:** `firmware/src/energy.c`.
- **Renode:** emulates the STM32F4 CPU, memory and USART2 only.
- **Physical hardware testing:** none.

## 2. Design decisions

| Decision | Reason |
|---|---|
| Profiles are `const` data tables, not separate builds | Water, air and soil use the same processing code. Adding a profile does not touch shared modules, and constants can live in flash. |
| Drivers are function-pointer tables (`probe`, `read_raw`, `convert`) | The sensor manager calls every sensor the same way. The water and soil EC channels share one driver with different calibration. |
| HAL backed by virtual devices that emit bus-level data | The drivers decode ADC counts, ADS1115-style registers, MAX31865-style SPI bytes, PMS-style UART frames and CRC-8 words exactly as they would on hardware. A port replaces only `hal_virtual.c`. |
| Hampel filter before the moving average or FIR | A moving average smears an isolated spike over several samples; a median test removes it. In the `noisy` scenario, spikes beyond the critical limit produced no alarms. |
| Hysteresis plus 2-sample confirmation | Prevents alarm chatter when a value hovers at a threshold; see the `recovery` scenario. |
| Burst sampling on a *suspect* sample, not only on a confirmed event | Without it, a long stable interval would delay confirmation by several intervals. |
| Wake-up period measured from the wake time | An RTC alarm fires at `wake + interval`; sensor warm-up happens inside the period. An earlier version measured from the end of warm-up, which stretched the "60 s" period to 63 s (water) and 91 s (air). Fixed and covered by a regression test. |
| ACK + bounded retries + priority store-and-forward queue | Lets the node know what was delivered, and keeps alerts first during outages. Every sample is also kept in the local log. |

## 3. Implementation summary

A wake-up (`node_step()` in `firmware/src/node.c`) does the following:

1. Powers the sensors and waits for the slowest warm-up (model clock).
2. Reads, converts and range-checks each channel.
3. Filters each channel (Hampel, then moving average or FIR).
4. Updates the per-channel event state machine.
5. Appends the sample to the circular log.
6. Asks the telemetry policy whether to send.
7. Queues and transmits the packet.
8. Picks the next interval and schedules the next wake-up.

The adaptive telemetry rules, in order: a new critical state sends an immediate ALERT; a channel still critical repeats the ALERT every `crit_repeat_s`; a state change sends an UPDATE or FAULT; a value moving by `delta_report` sends an UPDATE (rate-limited); otherwise a SUMMARY goes out every `summary_s`.

Sampling intervals (seconds):

| Profile | fixed | stable | active | alert | recovery | summary |
|---|---|---|---|---|---|---|
| water | 60 | 300 | 60 | 15 | 60 | 900 |
| air | 60 | 180 | 60 | 30 | 60 | 600 |
| soil | 300 | 1800 | 300 | 60 | 300 | 3600 |

**Timing model.** A sensor rail switched on at a wake-up stays on until the slowest switched sensor is ready: all rails switch together. Readings are time-stamped after that warm-up, and the radio uses one millisecond timeline (uplink airtime, then ACK or timeout, then retry backoff). Staggering the power-up so each sensor is on only for its own warm-up would reduce sensor energy; it is not implemented.

## 4. Energy model

Charge (mA·s) is summed per component and converted to energy at 3.3 V:

| Parameter | Value | Source |
|---|---|---|
| Radio TX / RX / sleep | 87 mA @ 20 dBm / 5.22 mA / 1.69 µA | RAK3172 datasheet |
| MCU active / idle | 4.0 mA / 1.0 mA | assumption |
| Processing | 3 ms per sample, 2 ms per packet | assumption |
| Sensors | `pwr` fields in `profiles/*.c` (e.g. PM2.5: 80 mA, 30 s warm-up) | assumptions for the sensor class |
| LoRa PHY | SF9, 125 kHz, CR 4/5, 8-symbol preamble, +13 B MAC overhead | Semtech time-on-air formula |

Worked example (water, fixed): one wake-up powers four sensors for 2.1 s (the pH warm-up). Turbidity alone draws 30 mA × 2.1 s = 63 mA·s, while a 30-byte packet at SF9 (≈ 0.29 s airtime) costs about 87 mA × 0.29 s ≈ 25 mA·s. This is why the water profile's energy is dominated by sensors, not the radio.

## 5. Evaluation method

- **Traces** (`python/generate_data.py`, seed 42): 8 scenarios per profile, each run for 12 h (water, air) or 48 h (soil). The scenarios are: stable, gradual, sudden, noisy (4× noise plus isolated spikes beyond the limit), sensor_failure, missing (20 % random missing readings plus a dropout), outage (radio down for 20 % of the run, with a critical event inside it), and recovery (the true value oscillates around the critical limit within ±0.6 × the hysteresis band).
- **Channel:** 2 % uplink loss, 2 % ACK loss, 0.5 % bit corruption.
- **Strategies:** `fixed`, `adaptive_tx` (fixed sampling, adaptive reporting; an ablation) and `adaptive`. All strategies use identical traces and seeds.
- **Ground truth:**
  - A *crossing* is any excursion of the noise-free signal beyond a critical limit.
  - An *event* merges crossings less than 1800 s apart. A value hovering at the limit is one environmental event, and the firmware's hysteresis is meant to hold one alarm through it.
  - An event counts as **detected** if the node is CRITICAL at any sample within ±600 s of the event. **Delay** is measured from the true onset.
  - **Alarm coverage** is the share of truly-critical time during which the node was in CRITICAL.
  - A **false alarm** is a node CRITICAL episode that overlaps no event.
- **Method correction:** an earlier analysis counted every crossing as a separate event. That turned the oscillation in `recovery` into 6 (water, air) or 22 (soil) events, most reported as "missed" even though the node was in alarm the whole time (alarm coverage 97–99 %). The current analysis reports both counts.
- **Robustness:** the main matrix uses one seed. `python/seed_study.py` repeats 5 water scenarios × 2 strategies over 10 seeds (100 runs).

## 6. Results

Totals over 8 scenarios per profile (`experiments/results/results_summary.md`):

| Profile | Strategy | Samples | TX attempts | Energy [J] (est.) | Detected events | Crossings | Alarm coverage | False alarms | Node delay [s] | Gateway delay [s] |
|---|---|---|---|---|---|---|---|---|---|---|
| water | fixed | 5760 | 6130 | 2022 | 4/4 | 9 | 92.0 % | 0 | 350 | 2071 |
| water | adaptive_tx | 5760 | 740 | 1569 | 4/4 | 9 | 92.0 % | 0 | 350 | 2150 |
| water | adaptive | 3436 | 725 | 964 | 4/4 | 9 | 95.5 % | 0 | 187 | 1638 |
| air | fixed | 5760 | 6140 | 57800 | 4/4 | 9 | 90.0 % | 0 | 415 | 1751 |
| air | adaptive | 3345 | 1125 | 31831 | 4/4 | 9 | 93.8 % | 0 | 254 | 2132 |
| soil | fixed | 4608 | 4988 | 604 | 4/4 | 25 | 89.5 % | 0 | 1460 | 6560 |
| soil | adaptive | 2716 | 870 | 185 | 4/4 | 25 | 96.4 % | 0 | 545 | 6230 |

The delays are means over 4 events. The gateway delay is dominated by the `outage` scenario, where the alert can only be delivered after the link returns (about 5616 s after onset for water and air).

**Seed study (water, 10 seeds).**

- Both strategies detected 40/40 events with 0 false alarms.
- TX attempts: 752.6 ± 4.4 (fixed) vs 90.2 ± 1.9 (adaptive) in `sudden`.
- Node delay in `sudden`: 423 s vs 168 s (identical across seeds, because the step onset is deterministic). In `gradual` the delay varies with noise: 232 ± 112 s vs 118 ± 60 s.

### Discussion

1. **Traffic.** Adaptive reporting removes 82–88 % of radio transmissions in every profile. The `adaptive_tx` ablation shows that this comes from the telemetry policy, not from sampling.
2. **Energy depends on the budget breakdown.** The estimated saving is 52 % (water), 45 % (air) and 69 % (soil). In air, sensors are 98 % of the energy and the PM2.5 fan dominates, so adaptive reporting alone saves 0.7 %. In soil the radio dominates, so reporting alone saves 56 %.
3. **Detection.** Burst sampling on suspect readings makes the adaptive node confirm events sooner than a 60 s fixed schedule. Example (`sudden`): water 168 s vs 423 s; soil 602 s vs 2282 s.
4. **Costs.**
   - **Fault detection:** a fault needs 3 invalid samples, so at long intervals it is slower: soil 4892 s vs 692 s; water 693 s vs 123 s.
   - **Oscillation:** in `recovery` the adaptive node takes *more* samples than fixed (water 883 vs 720), although it still sends 4–5× fewer packets.
   - **After an outage:** the node only probes the link when it wakes, so after the radio returns the adaptive node (waking every 300 s) reconnects later than the fixed node (every 60 s). Water `outage`: 6009 s vs 5622 s.
   - **Gateway detail:** reconstruction error rises (soil 0.14 → 0.42 of `delta_report`), although full-rate data stays in the local log.
5. **Queue limit.** In the 2.4 h outage the fixed strategy overflowed the 128-packet queue and evicted 17–19 low-priority packets. The adaptive strategy lost none.
6. **Stable-interval trade-off** (water, `sudden`): a stable interval of 300 s → 1200 s cuts the estimate from 108.5 J to 66.5 J, but increases detection delay from 168 s to 1068 s.

## 7. Verification

| Check | Command | Result (this run) |
|---|---|---|
| C unit/integration tests (188 checks, incl. timing regression) | `build\test_firmware.exe` | pass |
| Python end-to-end tests (7) | `python -m unittest discover -s tests -p "test_*.py"` | pass |
| C filters vs independent MATLAB reference | `matlab -batch "cd matlab; validate_c_output"` | max error 1.6e-6 (MA), 8.6e-7 (FIR), 2.4e-7 (Hampel) |
| Cortex-M4 image on Renode `stm32f4_discovery` | `run_renode.bat` | runs to completion, 11.8 M instructions. Output identical to the native build on the same 3 h water trace |
| CI (GitHub Actions, Ubuntu) | `make test` | builds, runs the tests and experiments, and commits refreshed results |

## 8. Limitations

- No physical hardware. The virtual devices model bus formats and conversion equations, not sensor physics, drift or electrical faults.
- No LoRaWAN stack: airtime formula plus application ACKs, with no join, ADR or RX1/RX2 timing. Duty cycle is reported, not enforced.
- The energy model relies on assumed MCU and sensor currents; only the radio currents come from a datasheet.
- Synthetic traces and demonstration thresholds; 4 critical events per profile; one seed except for the water seed study.
- Filters are per-sample, not time-weighted, so their time constant changes with the sampling interval.
- Sensors switch on together rather than staggered; the link is probed only at wake-ups.

## 9. Future work

1. Port `hal_virtual.c` to an STM32WL/RAK3172 board, then measure current to replace the assumed parameters.
2. Stagger sensor power-up, and wake early to probe the link when alerts are queued.
3. Drop to the active interval on the first invalid reading, to speed up fault detection.
4. Add a real LoRaWAN stack after a licence review.
