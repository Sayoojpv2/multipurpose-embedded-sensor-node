# Multipurpose Embedded Environmental Sensor Node

C99 firmware for **one** configurable environmental sensor node. The same firmware runs a water, air or soil monitoring profile. It reads and validates sensors, filters noise, detects abnormal conditions, adapts how often it samples and what it transmits, and sends packets to one simulated gateway. A simulation measures packet traffic, detection behaviour and estimated energy.

The project is software-only. The firmware is real C code; the sensors, radio channel and gateway are simulation models. Energy figures are model estimates, not measurements.

## Why

Battery-powered environmental nodes spend most of their energy on sensor warm-up and radio transmission. Conventional firmware samples and transmits at a fixed period. This project asks how much traffic and energy an event-driven node saves, and what it costs in detection delay and data detail. Three application profiles share one firmware core, which keeps profile-specific code out of the shared modules.

## Profiles

| Profile | Channels | Status |
|---|---|---|
| **Water** (primary demonstration) | pH, turbidity, conductivity, temperature | used for the seed study and the Renode ARM run |
| Air | PM2.5, CO2, humidity, temperature | secondary virtual profile |
| Soil | moisture, soil temperature, conductivity, light | secondary virtual profile |

A profile is a constant table (`firmware/profiles/profile_*.c`) holding the sensor list, driver choice, calibration, thresholds, sampling intervals and reporting periods.

## Main features

- HAL for ADC, I2C, SPI, UART, GPIO, timer and sensor power, backed by virtual devices that produce realistic bus data (ADC counts, register words with CRC-8, UART frames).
- Range checks, a Hampel outlier filter, moving-average and FIR filtering.
- Per-channel event state machine (normal / warning / critical / recovery / fault) with hysteresis and confirmation.
- Adaptive sampling (stable / active / alert / recovery intervals) and event-driven telemetry (alert, update, summary).
- Common binary packet with CRC-16; ACK, bounded retries, outage backoff and a store-and-forward queue.
- Circular local log of every sample; charge-based energy estimate.

## Requirements

- Windows: a C compiler (MinGW-w64 `gcc`, or the clang `gcc.exe` bundled with OMNeT++ 6.x, which `build.bat` finds automatically). Linux: `gcc` and `make`.
- Python 3.10+ with `numpy` and `matplotlib` (`python -m pip install -r requirements.txt`).
- Optional: MATLAB (filter cross-check), Arm GNU Toolchain + Renode 1.17 (Cortex-M4 emulation).

## Build, run and test

```bat
run.bat
```

`run.bat` builds the code, runs the C unit tests, generates the traces, runs 90 simulations plus the 100-run seed study, writes the results and plots, and runs the Python tests. `run.bat matlab` adds the MATLAB filter check. On Linux, `make test` does the same (without MATLAB).

Individual steps:

```bat
build.bat
build\test_firmware.exe
python python\generate_data.py
build\sensor_node.exe --profile water --strategy adaptive --trace experiments\traces\water_sudden.csv --out build
python -m unittest discover -s tests -p "test_*.py"
run_renode.bat
```

`--strategy` is `fixed`, `adaptive_tx` (fixed sampling, adaptive reporting) or `adaptive`. Each run writes `samples.csv`, `tx.csv`, `gateway.csv`, `storage_dump.csv` and `summary.json` to `--out`.

## Results

Synthetic 12 h (water, air) and 48 h (soil) traces; 8 scenarios per profile, totals over all 8; seed 42. Full tables are in [experiments/results/results_summary.md](experiments/results/results_summary.md), and the multi-seed check is in [seed_study.md](experiments/results/seed_study.md).

| Profile | Strategy | TX attempts | Est. energy [J] | Events detected | False alarms | Mean node detection delay [s] |
|---|---|---|---|---|---|---|
| water | fixed (60 s) | 6130 | 2022 | 4/4 | 0 | 350 |
| water | adaptive | 725 | 964 | 4/4 | 0 | 187 |
| air | fixed (60 s) | 6140 | 57800 | 4/4 | 0 | 415 |
| air | adaptive | 1125 | 31831 | 4/4 | 0 | 254 |
| soil | fixed (300 s) | 4988 | 604 | 4/4 | 0 | 1460 |
| soil | adaptive | 870 | 185 | 4/4 | 0 | 545 |

- **Traffic:** adaptive reporting cuts radio transmissions by 82–88 %.
- **Energy:** the estimated saving (45–69 %) depends on what dominates the budget. In the air profile the PM2.5 sensor's fan dominates, so adaptive *reporting* alone saves under 1 %; only sampling less often helps.
- **Costs:**
  - Sensor faults are detected later at long intervals (soil: 4892 s vs 692 s).
  - Oscillating events take more samples than fixed reporting.
  - The gateway sees a coarser picture.
  - After a radio outage, alerts reach the gateway later on average (water seed study: 5961 s vs 5619 s).
- **Seed study:** over 10 seeds (water), every event was detected and no false alarms occurred, with both strategies.

## Repository layout

```
firmware/     C99 firmware: include/, src/, drivers/, profiles/, hal/, targets/stm32f4/ (Renode image)
simulation/   virtual sensor devices, radio channel + gateway, native harness (main_sim.c)
python/       trace generation, experiment runner, analysis, plots, seed study
matlab/       independent filter reference and C-vs-MATLAB check
tests/        test_firmware.c (C unit/integration), test_pipeline.py (end-to-end)
experiments/  configs/ and generated results/ (raw traces and runs are regenerated, not committed)
docs/         architecture, technical report, related work, code walkthrough
```

## Limitations

- No physical hardware was used. Virtual devices reproduce bus formats and conversion equations, not real sensor physics.
- The radio is a LoRa time-on-air model with application-level ACKs. It is not a LoRaWAN stack, and duty-cycle limits are reported but not enforced.
- Only the three radio currents come from a datasheet (RAK3172); MCU and sensor currents are documented assumptions.
- Traces are synthetic and thresholds are demonstration values. There are only 4 critical events per profile, so the detection statistics are indicative.
- Renode emulates the Cortex-M4, memory and USART only; sensors and radio stay virtual inside the image.

Further reading: [technical report](docs/technical_report.md) (design, method, results discussion), [architecture](docs/architecture.md), [code walkthrough](docs/code_walkthrough.md) (where to start reading the code) and [related work](docs/related_work.md).

## Licence

MIT.
