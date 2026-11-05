# Design and Evaluation Report

## 1. Scope

This project implements a reusable C99 firmware core for one environmental sensor node. It supports water, air and soil application profiles through configuration data. The evaluation compares fixed-period reporting with adaptive sampling and telemetry.

The node is evaluated with virtual sensor devices and a simulated radio link. One simulated gateway receives the node's packets. The project does not implement a wireless sensor network, routing or multi-hop communication.

No physical hardware measurements are claimed.

## 2. Node architecture

The firmware is organised into the following layers:

1. **Hardware abstraction:** ADC, I2C, SPI, UART, GPIO, timer and sensor-power functions.
2. **Sensor drivers:** convert bus-level readings into engineering units.
3. **Application profiles:** select sensor channels, drivers, calibration, thresholds and timing parameters.
4. **Processing:** validate readings, reject isolated outliers and apply moving-average or FIR filtering.
5. **Event handling:** apply warning and critical thresholds with hysteresis and consecutive-sample confirmation.
6. **Sampling control:** select the next sampling interval from the node's current state and recent measurement changes.
7. **Telemetry:** select full data, summary, update, alert or fault packets.
8. **Storage and link:** retain raw records locally and deliver queued packets through the simulated radio link.

A profile is data rather than a separate firmware build. The sensor manager, processing, event, sampling, packet, storage and communication modules are shared.

## 3. Application profiles

| Profile | Channels | Example interfaces represented |
|---|---|---|
| Water | pH, turbidity, conductivity, water temperature | ADC, I2C and SPI |
| Air | PM2.5, CO2, humidity and air temperature | UART and I2C |
| Soil | moisture, soil temperature, conductivity and light | ADC and I2C |

The driver implementations model selected, documented device formats (for example, ADS1115-style I2C conversion data, MAX31865-style RTD data, PMS-style UART frames and Sensirion-style CRC-8 words). These are virtual device models; the project does not include vendor sensor firmware or claim electrical compatibility testing.

## 4. Signal processing and event detection

Each valid channel reading is range-checked and passed through its configured processing chain. The implementation supports a Hampel outlier identifier, moving-average filtering and a 7-tap binomial FIR filter.

The event state machine distinguishes normal, warning, critical, recovery and sensor-fault states. Hysteresis reduces repeated transitions around a threshold. Consecutive-sample confirmation is used before changing event level.

The sampling controller has stable, active, alert and recovery modes. It uses event state, pending threshold crossings, suspect samples and rate-of-change cues to select the next interval.

## 5. Sensor warm-up and model time

When a sensor rail is switched on, the sensor manager determines the longest configured warm-up plus measurement interval among the sensors enabled for that wake-up. The virtual clock advances by that interval before sensor reads are performed.

The energy estimator charges newly enabled sensors for the shared power-up interval, then accounts for off-state current during the remainder of the interval where applicable. This aligns the simulated read timing with the configured warm-up assumptions.

This is a time-domain software model. It does not emulate analog settling, sensor noise physics or real MCU sleep behaviour.

## 6. Telemetry and radio model

The fixed strategy transmits all active channels on each sample. The adaptive strategy sends an alert on a critical event, a fault or state update when channel status changes, a delta update when a reading changes sufficiently, and a periodic summary when no other update is required. Every sample is retained in the local circular log.

Packets use a common header, profile identifier, sequence number, timestamp, channel mask, status information and CRC-16. Summary packets include minimum and maximum values.

The radio link uses a LoRa-style time-on-air formula. A shared millisecond timeline serialises queued uplinks. Each attempt occupies uplink airtime and an ACK receive interval or timeout; retries include a configurable backoff. Packet reception is timestamped at the end of uplink airtime.

This is not a LoRaWAN implementation. Join procedures, network-server behaviour, adaptive data rate, MAC commands and full RX1/RX2 timing are outside the project scope.

## 7. Energy estimation

The energy model accounts for sensor active/off current, sensor warm-up and measurement intervals, MCU processing and idle assumptions, radio transmit airtime, receive/ACK time and sleep current.

Radio TX, RX and sleep currents are taken from the RAK3172 module datasheet cited in the source. MCU current, processing durations, several sensor currents and warm-up values are configurable assumptions documented in the code and experiment configuration.

Results are model estimates. They are not measured current, battery-life or hardware power results. The relative comparison is meaningful only under the stated model assumptions.

## 8. Experimental method

Synthetic traces are generated for stable operation, gradual and sudden changes, noisy readings, sensor faults, missing readings, radio outages and recovery. The same trace and random seed are used when comparing strategies.

Three strategies are evaluated:

- **Fixed:** fixed sampling and a full data packet on every sample.
- **Adaptive telemetry:** fixed sampling with change-driven and summary reporting.
- **Adaptive:** both sampling intervals and telemetry policy adapt to the observed conditions.

The experiment scripts calculate sample counts, packet attempts, bytes, airtime, estimated energy, event detection, false alarms, detection delay, packet recovery and gateway reconstruction error.

Current generated values are recorded in [results_summary.md](../experiments/results/results_summary.md) and the CSV files in [experiments/results](../experiments/results). These files are regenerated by the experiment pipeline and CI workflow.

## 9. Validation

The repository includes C unit tests, Python end-to-end tests and an independent MATLAB reference for the moving-average, FIR and Hampel filters.

The optional Renode target cross-compiles the common firmware for a Cortex-M4 image and runs it on the STM32F4 Discovery model. The CPU, memory and USART console are emulated; sensor devices and radio remain virtual.

The CI workflow builds the native code and runs the automated tests and experiment pipeline. MATLAB and Renode checks are optional local steps because they require those applications to be installed.

## 10. Limitations and future work

- Physical sensor, MCU, radio and battery validation has not been performed.
- Virtual devices model selected bus transactions and conversion equations, not full hardware behaviour.
- The radio model is not a LoRaWAN stack and does not enforce regional duty-cycle limits.
- Energy results depend on assumptions where datasheet values are unavailable.
- Environmental traces and thresholds are synthetic and illustrative.
- Detection statistics are based on a limited number of generated event episodes.
- Filtering is sample-based; a future version could use time-weighted filtering when the sampling interval changes.

Future work includes porting the HAL to a physical low-power MCU, measuring actual current, validating the sensor drivers with real devices, and implementing a standards-compliant radio stack.
