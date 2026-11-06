# Multipurpose Embedded Environmental Sensor Node

C99 firmware for one configurable environmental sensor node. The same firmware runs a **water** (primary demonstration), **air** or **soil** profile. It reads and validates sensors, filters noise, detects abnormal conditions, adapts how often it samples and what it transmits, and sends packets to one simulated gateway.

This is a software project. The firmware is real, portable C. The sensors, radio channel and gateway are simulation models; energy is a model estimate; the Cortex-M4 build runs in the Renode emulator. Nothing has been tested on physical hardware.

## Why

Battery-powered nodes spend most of their energy on sensor warm-up and radio transmission. Conventional firmware samples and transmits at a fixed period. This project measures, in simulation, how much traffic and energy an event-driven node saves, and what it costs in detection delay and data detail.

## How it works

```
sensors -> HAL -> drivers -> validate -> Hampel + MA/FIR -> event state machine
        -> local log -> telemetry policy -> packet + CRC-16 -> ACK/retry link -> gateway
                                   adaptive sampler -> next wake-up
```

- **HAL** (ADC, I2C, SPI, UART, GPIO, timer, sensor power) backed by virtual devices that produce realistic bus data.
- **Profiles** are `const` tables: sensor list, drivers, calibration, thresholds, sampling and reporting periods. The processing code is shared.
- **Events:** normal / warning / critical / recovery / fault, with hysteresis and 2-sample confirmation.
- **Sampling** switches between stable / active / alert / recovery intervals.
- **Telemetry** sends alerts, changed-channel updates and periodic summaries instead of every sample.
- **Delivery:** a common binary packet (12 B header, int16 values, CRC-16); ACK with bounded retries, outage backoff and a store-and-forward queue; every sample is kept in a circular local log.

Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Quick start (Windows)

```bat
python -m pip install -r requirements.txt
run.bat
```

`run.bat` builds the code, runs the C tests, generates the traces, runs the experiments, analysis, plots and seed study, and runs the Python tests. Linux: `make test`. More commands (single runs, MATLAB, Renode, CI): [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Results

All results come from simulation with synthetic traces. 8 scenarios per profile (12 h water and air, 48 h soil, seed 42); totals are over the 8 scenarios.

| Profile | Strategy | TX attempts | Est. energy [J] | Events detected | False alarms | Mean detection delay [s] |
|---|---|---|---|---|---|---|
| water | fixed 60 s | 6130 | 2022 | 4/4 | 0 | 350 |
| water | adaptive | 725 | 964 | 4/4 | 0 | 187 |
| air | fixed 60 s | 6140 | 57800 | 4/4 | 0 | 415 |
| air | adaptive | 1125 | 31831 | 4/4 | 0 | 254 |
| soil | fixed 300 s | 4988 | 604 | 4/4 | 0 | 1460 |
| soil | adaptive | 870 | 185 | 4/4 | 0 | 545 |

- **Traffic:** adaptive reporting removes 82–88 % of transmissions.
- **Energy:** the estimated saving is 45–69 %. In the air profile the PM2.5 sensor's fan dominates, so reporting less alone saves under 1 %.
- **Costs:**
  - Slower sensor-fault detection at long intervals (soil 4892 s vs 692 s).
  - More samples during oscillating events.
  - A coarser live view at the gateway.
  - Later reconnection after a radio outage.
- **Seed study:** over 10 seeds (water), both strategies detected every event with no false alarms.

Full tables: [experiments/results/results_summary.md](experiments/results/results_summary.md) and [seed_study.md](experiments/results/seed_study.md). Method and discussion: [docs/TECHNICAL_REPORT.md](docs/TECHNICAL_REPORT.md).

## Layout

```
firmware/     include/ src/ drivers/ profiles/ hal/ targets/stm32f4/
simulation/   virtual sensors, radio + gateway model, native harness
python/       trace generation, experiments, analysis, plots, seed study
matlab/       filter reference and C-vs-MATLAB check
tests/        test_firmware.c, test_pipeline.py
experiments/  configs/, results/ (tables and plots)
docs/         ARCHITECTURE.md, TECHNICAL_REPORT.md, DEVELOPMENT.md
```

## Limitations

- No physical sensors, MCU, radio or battery measurements.
- The radio is a LoRa time-on-air model with application-level ACKs. It is not a LoRaWAN stack.
- Only the radio currents come from a datasheet (RAK3172); the MCU and sensor currents are assumptions.
- The traces are synthetic, the thresholds are demonstration values, and there are only 4 critical events per profile.
- Renode emulates the CPU, memory and USART only; the sensors and radio stay virtual inside the image.

Developed with AI assistance (Claude, Anthropic); the git history records the changes and their reasons.

## Licence

MIT
