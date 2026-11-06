# Technical report

## 1. Objective

The objective is firmware for one battery-powered environmental sensor node. The same code serves water, air and soil monitoring through configuration, and it should reduce radio traffic and energy without missing critical events. Fixed-period reporting is compared with adaptive sampling and adaptive telemetry on identical inputs. The design itself is described in [ARCHITECTURE.md](ARCHITECTURE.md).

### What kind of evidence each result is

| Item | Nature |
|---|---|
| Firmware behaviour (drivers, filters, events, packets, link) | C code executed natively and checked by unit tests |
| Environmental input | **Synthetic** traces from `python/generate_data.py` |
| Packet delivery, delays, losses | **Simulated** channel and gateway (`simulation/radio/`) |
| Energy | **Model estimate** (`firmware/src/energy.c`) |
| Radio TX/RX/sleep currents | **Datasheet** (RAK3172) |
| MCU and sensor currents, processing times | **Assumed** (documented in code) |
| Cortex-M4 execution | **Emulated** in Renode (CPU, memory, USART only) |
| Physical hardware, sensor accuracy, real LoRa links, battery life | **Not validated** |

## 2. Method

**Traces.** There are 8 scenarios per profile, generated with seed 42. Water and air runs are 12 h at 1 s resolution; soil runs are 48 h at 10 s resolution.

| Scenario | Content |
|---|---|
| stable | daily cycle + noise |
| gradual | ramp beyond the critical limit and back |
| sudden | step beyond the limit for 8 % of the run |
| noisy | 4× noise + isolated spikes beyond the limit |
| sensor_failure | stuck reading, then disconnected sensor |
| missing | 20 % random missing readings + a dropout |
| outage | radio down for 20 % of the run, with a critical event inside |
| recovery | true value oscillates around the critical limit by ±0.6 × hysteresis band |

**Channel.** 2 % uplink loss, 2 % ACK loss, 0.5 % bit corruption. The LoRa PHY is SF9 at 125 kHz.

**Strategies.** All three use identical traces and seeds:

- `fixed`: fixed sampling, every sample reported.
- `adaptive_tx`: fixed sampling, adaptive reporting. This is an ablation that separates the effect of reporting from that of sampling.
- `adaptive`: adaptive sampling and adaptive reporting.

**Metrics** (`python/analyse_results.py`):

- **Crossing / event:** a *crossing* is any excursion of the noise-free signal beyond a critical limit. An *event* merges crossings less than 1800 s apart, because a value hovering at the limit is one environmental event and the hysteresis is designed to hold one alarm through it. Both counts are reported.
- **Detected:** the node is CRITICAL at some sample within ±600 s of the event. **Delay** is the time from the true onset to the first such sample. The **gateway delay** is the time to the first CRITICAL packet *received* (`t_rx`).
- **Alarm coverage:** the share of truly-critical time during which the node state was CRITICAL.
- **False alarm:** a node CRITICAL episode that overlaps no event.
- **Reconstruction error:** RMS difference between the gateway's picture and the noise-free truth, in units of the channel's `delta_report`.
  - *Live*: only packets already received at each moment count, and a late queued packet never overwrites newer data.
  - *Offline*: every packet is placed at its sample time.

**Robustness.** The main matrix uses one seed. `python/seed_study.py` repeats 5 water scenarios × 2 strategies over 10 seeds.

**Corrections made during development.** Both are recorded in the git history.

- An earlier analysis counted each crossing as a separate event, which turned the `recovery` oscillation into 6–22 "missed" events while the node was in fact in alarm 97–99 % of the time.
- An earlier firmware version measured the wake-up period from the end of sensor warm-up, stretching the 60 s baseline to 63–91 s.

## 3. Results

Totals over 8 scenarios per profile. Full tables: [results_summary.md](../experiments/results/results_summary.md).

| Profile | Strategy | Samples | TX attempts | Energy [J] est. | Events | Alarm coverage | False alarms | Node delay [s] | Gateway delay [s] | Recon. error live / offline |
|---|---|---|---|---|---|---|---|---|---|---|
| water | fixed | 5760 | 6130 | 2022 | 4/4 | 92.0 % | 0 | 350 | 2071 | 0.16 / 0.11 |
| water | adaptive_tx | 5760 | 740 | 1569 | 4/4 | 92.0 % | 0 | 350 | 2150 | 0.17 / 0.12 |
| water | adaptive | 3436 | 725 | 964 | 4/4 | 95.5 % | 0 | 187 | 1638 | 0.19 / 0.13 |
| air | fixed | 5760 | 6140 | 57800 | 4/4 | 90.0 % | 0 | 415 | 1751 | 0.29 / 0.22 |
| air | adaptive | 3345 | 1125 | 31831 | 4/4 | 93.8 % | 0 | 254 | 2132 | 0.33 / 0.23 |
| soil | fixed | 4608 | 4988 | 604 | 4/4 | 89.5 % | 0 | 1460 | 6560 | 0.23 / 0.14 |
| soil | adaptive | 2716 | 870 | 185 | 4/4 | 96.4 % | 0 | 545 | 6230 | 0.52 / 0.42 |

Delays are means over 4 events per profile. The gateway delay is dominated by the `outage` scenario, where an alert can only arrive after the link returns (about 5616 s after onset for water and air). Sensor faults were detected in all 3/3 cases per profile for every strategy.

**Seed study (water, 10 seeds).**

- Every strategy detected 40/40 events with 0 false alarms.
- `sudden`: TX attempts were 752.6 ± 4.4 (fixed) vs 90.2 ± 1.9 (adaptive); node delay was 423 s vs 168 s.
- `gradual`: node delay was 232 ± 112 s vs 118 ± 60 s.

## 4. Discussion

1. **Traffic.** Adaptive reporting removes 82–88 % of transmissions in every profile. The `adaptive_tx` ablation shows that this comes from the reporting policy, not from sampling less.
2. **Energy depends on the budget.** The estimated saving is 52 % for water, 45 % for air and 69 % for soil.
   - In air, sensors are 98 % of the estimate (mostly the PM2.5 fan), so adaptive reporting alone saves 0.7 %.
   - In soil the radio dominates, so reporting alone saves 56 %.
   - Because these are model estimates, the *relative* comparison is the meaningful part.
3. **Detection.** Burst sampling on suspect readings confirmed events sooner than a 60 s fixed schedule (`sudden`: water 168 s vs 423 s; soil 602 s vs 2282 s).
4. **Costs.**
   - **Fault detection:** slower at long intervals (soil 4892 s vs 692 s), because 3 invalid samples are needed.
   - **Oscillating events:** cost *more* samples than fixed (water `recovery`: 883 vs 720), although packets are still 4–5× fewer.
   - **Probing after an outage:** the link is only probed at wake-ups, so a node in a 300 s interval reconnects later than one in a 60 s interval (water `outage`: 6009 s vs 5622 s).
   - **Gateway view:** the live reconstruction error roughly doubles in soil (0.23 → 0.52 of `delta_report`); full-rate data stays in the local log.
5. **Queue limit.** In the 2.4 h outage the fixed strategy overflowed its 128-packet queue and dropped 17–19 low-priority packets; the adaptive strategy dropped none.
6. **Stable interval trade-off** (water, `sudden`): raising the stable interval from 300 s to 1200 s lowers the estimate from 108.5 J to 66.5 J but raises the detection delay from 168 s to 1068 s.

## 5. Related work

The bibliographic details were checked against the Crossref DOI registry. Each description below is limited to the paper's title or abstract; the full texts were not reviewed.

1. F. Leccese, R. Arciulo, "A Modular Low-Power Environmental Monitoring Node with RAK3172 LoRaWAN Core, Dual-Path Logging and Low-Cost Sensors," *IEEE MetroInd4.0&IoT*, pp. 523–528, 2026. doi:10.1109/MetroInd4.0IoT69397.2026.11653126. The node is built on the same radio module whose datasheet currents this project's energy model uses.
2. O. Oyinlola et al., "Octopus: A low-cost, modular environmental sensing platform for makers," *HardwareX*, vol. 26, e00760, 2026. doi:10.1016/j.ohx.2026.e00760. A modular, Arduino-compatible open hardware platform (https://senseable.mit.edu/octopus/).
3. M. J. C. S. Reis, "Low-Power Embedded Sensor Node for Real-Time Environmental Monitoring with On-Board Machine-Learning Inference," *Sensors*, 26(2):703, 2026. doi:10.3390/s26020703. According to its abstract, it is a Simulink study using adaptive duty-cycling and on-device quantised neural classification, and it reports about 88 % fewer transmissions than periodic reporting.

This project does not claim a new concept. Its scope is one profile-configured firmware core and a reproducible comparison on identical traces. It uses rule-based detection rather than ML because threshold events with hysteresis meet the requirement and can be verified line by line. Ref. 3's 88 % is of the same order as the 82–88 % here, but the models and scenarios differ, so the two numbers are not directly comparable.

Other sources: the RAK3172 datasheet (radio currents), the Semtech SX1276 datasheet (LoRa time-on-air formula) and Renode 1.17.0 (`stm32f4_discovery` platform). The driver formats are re-implemented from public datasheet descriptions, not copied.

## 6. Verification

| Check | Result |
|---|---|
| C unit/integration tests (`build\test_firmware.exe`) | 198 checks pass. They cover drivers, filters, events, sampling, packet bounds and CRC, airtime, outage/store-and-forward, wake-up timing and reproducibility. |
| Same tests with clang `-Wpedantic -Wshadow` and the UB sanitizer | no warnings, no undefined behaviour trapped |
| Python tests (`python -m unittest discover -s tests`) | 12 pass. They cover reproducibility, malformed-trace rejection, option ranges, CRC accounting and the live/offline gateway view. |
| MATLAB reference for MA/FIR/Hampel | max error ≤ 1.6e-6 (single-precision rounding) |
| Renode `stm32f4_discovery` | runs to completion; samples, packets, attempts, airtime and energy are identical to the native build on the same trace |
| CI (GitHub Actions, Ubuntu, `make test`) | builds and tests read-only; a separate job commits refreshed results on `main` |

## 7. Limitations

- No physical hardware. The virtual devices model bus formats and conversion equations, not sensor physics, drift or electrical faults.
- No LoRaWAN stack (no join, ADR or RX1/RX2 timing). Duty cycle is reported but not enforced.
- The energy model relies on assumed MCU and sensor currents. Sensors are switched on together, not staggered.
- Synthetic traces and demonstration thresholds; 4 critical events per profile; multi-seed evidence only for water.
- Filters work per sample, so their time constant changes with the sampling interval. The link is probed only at wake-ups.

## 8. Future work

1. Port `hal_virtual.c` to an STM32WL/RAK3172 board and measure current to replace the assumed parameters.
2. Stagger sensor power-up, and wake early to probe the link when alerts are queued.
3. Switch to the active interval on the first invalid reading, to speed up fault detection.
