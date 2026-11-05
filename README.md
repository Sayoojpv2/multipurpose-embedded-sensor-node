# Multipurpose Embedded Environmental Sensor Node

Portable C99 firmware for a **single configurable environmental sensor node**. The same firmware can be configured for water, air or soil monitoring by selecting an application profile.

The project focuses on the software inside one node: sensor drivers, signal processing, event detection, sampling control, local storage, telemetry and a low-power link model. A single simulated gateway is used as the receiver. There is no multi-node network or routing.

The project is software-only. Sensor devices and the radio are virtual models. The firmware runs in a native software-in-the-loop simulator and can also be cross-compiled for a Cortex-M4 image and executed on Renode's STM32F4 Discovery model.

## Applications

| Profile | Measurements |
|---|---|
| Water | pH, turbidity, electrical conductivity and temperature |
| Air | PM2.5, CO2, relative humidity and temperature |
| Soil | Soil moisture, soil temperature, electrical conductivity and light |

The application profile contains the sensor list, driver selection, calibration, thresholds, sampling intervals and reporting settings. The common firmware modules are shared by all profiles.

## Firmware

The C firmware includes:

- A hardware abstraction layer for ADC, I2C, SPI, UART, GPIO, timers and sensor power.
- Sensor drivers and profile-based configuration.
- Calibration, range checks, Hampel outlier rejection, moving-average and FIR filters.
- Event detection with hysteresis and confirmation.
- Adaptive sampling for stable, active, alert and recovery conditions.
- A common telemetry packet format with CRC-16.
- A link model with serialized airtime, ACK windows, bounded retries and outage backoff.
- Local circular storage and store-and-forward transmission.
- A configurable energy model.

## Build and run on Windows

Requirements: a C compiler (MinGW-w64 or the compiler bundled with OMNeT++), Python 3, NumPy and Matplotlib.

Install the Python dependencies:

```powershell
python -m pip install -r requirements.txt
```

Build and run the complete test and experiment pipeline:

```bat
run.bat
```

To build only the native simulator and C test executable:

```bat
build.bat
```

Run one profile and strategy:

```bat
build\sensor_node.exe --profile water --strategy adaptive --trace experiments\traces\water_sudden.csv --out out
```

Generate scenario traces first with `python python\generate_data.py`.

## Build and test on Linux

A Makefile is provided for GCC-based systems:

```bash
make
make test
```

`make test` builds the firmware and runs the C tests, experiment matrix, result analysis, plots and Python pipeline tests.

## Cortex-M4 emulation

The optional ARM target is built with the Arm GNU Toolchain and run on Renode's `stm32f4_discovery` model:

```bat
run_renode.bat
```

The emulated target executes the compiled Cortex-M4 firmware and emulates the CPU, memory and USART console. Sensor devices and the radio remain virtual. This is not physical hardware validation.

## Results

The experiment scripts compare fixed-period reporting, adaptive telemetry with fixed sampling, and fully adaptive sampling on identical synthetic traces. Results include packet attempts, bytes, airtime, estimated energy, event detection, notification delay, data recovery and gateway reconstruction error.

The generated summary is in [experiments/results/results_summary.md](experiments/results/results_summary.md). The analysis uses synthetic traces and demonstration thresholds; the energy model is an estimate, not a power measurement.

## Documentation

- [Architecture](docs/architecture.md)
- [Design and evaluation report](docs/technical_report.md)
- [Related work and research comparison](docs/research_comparison.md)

## Limitations

- No physical sensor, microcontroller or radio hardware was used.
- The virtual sensors reproduce selected bus formats and conversion behaviour; they do not model every electrical or environmental effect.
- The radio uses a LoRa-style airtime calculation and application-level ACK model. It is not a LoRaWAN stack.
- Energy values combine selected datasheet radio currents with documented assumptions for sensors, MCU activity and processing time.
- Input traces are synthetic, and profile thresholds are demonstration settings rather than regulatory limits.
