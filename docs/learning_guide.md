# Learning guide

This guide explains the project so you can rebuild it yourself and defend it in an interview. Each section names the file to open next to it.

## 1. Overall architecture

The firmware is a stack of small modules that talk through structs, not global state:

`HAL → drivers → sensor manager → DSP → event FSM → (storage, telemetry) → packet → link → radio`

`node_step()` in `firmware/src/node.c` is the whole application: one call = one wake-up. It returns the time of the next wake-up, which on a real MCU you would program into the RTC before entering STOP mode. The native simulator (`simulation/main_sim.c`) simply calls `node_step()` in a loop with simulated time.

There is no `malloc` anywhere in `firmware/`. All buffers have fixed sizes (`MAX_CHANNELS`, `MA_MAX_LEN`, `FIR_MAX_TAPS`, `HAMPEL_WIN`, `TXQ_CAP`). Fixed sizes make worst-case RAM known at link time; the `arm-none-eabi-size` output reports it.

## 2. How the three profiles work

Open `firmware/profiles/profile_water.c`. It is just a `const app_profile_t` initialiser with four `sensor_channel_cfg_t` entries plus sampling, event and telemetry settings. To select a profile, `node_init()` calls `profile_get(id)`, and every module then reads its parameters from `n->prof`.

On the command line:

```bash
build\sensor_node.exe --profile air --strategy adaptive --trace experiments\traces\air_sudden.csv --out out
```

or `--config node.ini` with `profile = air`.

Exercise: add a fourth profile (for example "greenhouse") by copying `profile_soil.c`, changing the thresholds, and registering it in `profile_registry.c`. No other file needs to change.

## 3. Sensor-driver abstraction

`sensor_driver_t` (in `sensor.h`) is a table of three function pointers:

- `probe(cfg)`: is the device there? Used at registration, and again later if the device was missing.
- `read_raw(cfg, &raw)`: one bus transaction. It returns a status such as `RD_NO_RESPONSE` or `RD_CHECKSUM`.
- `convert(cfg, raw, aux, &value)`: raw value to engineering units, using `cfg->cal.p[]`. `aux` carries a compensation input, such as temperature for EC.

This is the same idea as a C++ virtual interface, written in C with function pointers. The sensor manager calls `c->driver->read_raw(c, &raw)` without knowing which sensor it is. The soil and water EC channels share **one** driver (`DRV_EC_ADS_I2C`) with different `cal.p[0]` values.

## 4. ADC, I2C, SPI and UART (as used here)

- **ADC** (`analog_drivers.c`): 12-bit, Vref 3.3 V, so `V = raw × 3.3 / 4095`. Four conversions are averaged (oversampling). A raw value near 0 or 4095 means the input is at a rail (open or shorted sensor), giving `RD_RAIL`.
- **I2C** (`digital_drivers.c`): 7-bit address, write a command or register pointer, then read bytes. A NACK (no device, or no power) becomes `RD_NO_RESPONSE`. The Sensirion-style words carry a CRC-8 (polynomial 0x31) per 16-bit word.
- **SPI**: full duplex, so the master clocks out the address byte and reads the data in the same transfer. MISO idles high, so 0xFF… means no device. The MAX31865-style code has a fault bit in its LSB.
- **UART**: the PM sensor streams 32-byte frames: header `0x42 0x4D`, length, 13 big-endian words, then a 16-bit sum checksum. A timeout returns fewer than 32 bytes.

## 5. Calibration and filtering

- **Two-point calibration** (pH): measure the voltage in pH 7 and pH 4 buffers, which gives the offset (Vmid) and slope. The test `test_adc_and_calibration` shows that the uncalibrated Nernst values give an error larger than 0.1 pH.
- **Non-linear conversions:** NTC Beta equation `1/T = 1/T0 + ln(R/R0)/B`; PT100 Callendar-Van Dusen `R = R0(1 + A·T + B·T²)` solved for T with the quadratic formula.
- **Temperature compensation:** `EC25 = EC_T / (1 + 0.02 (T − 25))`.
- **Hampel identifier:** a sample is an outlier if it differs from the window median by more than `k · 1.4826 · MAD`. It is replaced by the median, so an isolated spike disappears while a real step is accepted after 3 samples.
- **Moving average:** a running sum over a circular buffer, so each update is O(1).
- **FIR:** `y[n] = Σ h[k] x[n−k]` with the binomial taps `[1 6 15 20 15 6 1]/64`, whose DC gain is 1. Your FPGA FIR projects implemented the same equation in hardware; here it is a circular buffer in C. `matlab/validate_c_output.m` checks the C output against MATLAB `filter()`.

## 6. Circular buffers

Two kinds are used:

1. **Storage** (`storage.c`): `head` is the next slot; once `count == capacity` the oldest record is overwritten and `overwritten++` is incremented. A record with sequence number `seq` lives at slot `(head + capacity − (next_seq − seq)) % capacity`.
2. **Filter delay lines** (`dsp.c`): the same modulo-index trick, with no data shifting.

The TX queue (`link_t.q`) is a fixed array searched for the highest priority, and within that the oldest packet. With 128 entries a linear search is cheap and simple.

## 7. State machines

- **Per-channel event FSM** (`event.c`). The levels are 0 (normal), 1 (warning) and 2 (critical). To go *up* a level, the value must exceed the threshold for `confirm_n` consecutive samples. To come *down*, the value must be inside the threshold by more than the hysteresis band (`relax = hyst`) for `confirm_n` samples. Leaving WARNING or CRITICAL goes through RECOVERY, which lasts `recover_n` normal samples. `fault_n` invalid readings lead to FAULT. The test runs the same oscillating signal with and without hysteresis: without it, the state chatters.
- **Sampling-mode FSM** (`sampling.c`): STABLE, ACTIVE, ALERT and RECOVERY.
- **Link state:** up or down, with exponential backoff.

## 8. Adaptive sampling

The node samples slowly while the environment is calm and fast when something might be happening. The trigger is a *cue*, not only a confirmed event: a suspect sample, a raw value beyond a threshold, or a high rate of change. That is why the adaptive node detected the critical events *sooner* than the 60 s fixed baseline in these runs, even though it samples less in total. Returning to STABLE needs `stable_confirm` calm samples; this is hysteresis on the sampling mode. The cost is that slow sampling delays the detection of sensor faults (see technical report §5.3).

Sensor power management: if the next interval is shorter than a sensor's warm-up plus measurement time, the sensor stays powered. For example, the PM sensor stays on in the 30 s alert mode.

## 9. Packet structure

There is one common format for all profiles (`packet.h`). Its key ideas:

- A **channel mask** lets an UPDATE carry only the channels that changed (an 18 B ALERT instead of a 30 B DATA packet).
- **Fixed-point int16 values** with a per-channel scale (pH × 100, for example), which halves the size compared with float32. `0x8000` means "no value".
- **Priority and node state** sit in the header, so the gateway can triage a packet without decoding it fully.
- The **stored flag** marks data delivered late from the queue.

## 10. CRC

CRC-16/CCITT-FALSE: polynomial 0x1021, initial value 0xFFFF, no reflection, no final XOR. The check value for "123456789" is 0x29B1 (tested in C, and printed by the ARM image in Renode). A CRC detects every single-bit error (the unit test flips each bit of a packet) and all burst errors up to 16 bits. It is **not** security: anyone can recompute it. The sensors use CRC-8 (polynomial 0x31, initial value 0xFF; the check for 0xBEEF is 0x92).

## 11. Communication efficiency

The energy to send one packet ≈ TX current × airtime. LoRa airtime grows with the spreading factor and the payload length (Semtech formula in `comms.c`; 13 B at SF7 takes 46.3 ms, at SF12 1155 ms). Three ways to save:

1. Send fewer packets (event-driven telemetry).
2. Make each packet smaller (channel mask, int16).
3. Avoid retransmissions (ACK + bounded retries; duplicates are removed at the gateway).

Store-and-forward protects data during outages, and the priority queue makes sure alerts go first.

## 12. Energy estimation

Charge Q (mA·s) = Σ current × time for each state. Energy (mJ) = Q × 3.3 V. Average current = Q / duration. Battery life (h) = capacity (mAh) / average current (mA). The model is only as good as its parameters: the three radio currents come from the RAK3172 datasheet, and everything else is an assumption listed in the technical report. Always say "estimated by the model".

## 13. Testing methodology (without hardware)

- **Unit tests** check each module in isolation (filters, CRC, FSMs).
- **Driver tests through the virtual HAL:** the virtual device implements the *inverse* physics, so a driver bug shows up as a wrong decoded value.
- **Fault injection:** stuck, disconnected and NACKing sensors, bad checksums, radio loss, bit errors and outages.
- **Golden-reference comparison:** MATLAB against C.
- **Cross-target comparison:** the native and Renode ARM builds give identical results.
- **Reproducibility:** the same seed gives bit-identical outputs (`test_pipeline.py`).

## 30-second explanation

> I built the firmware for a single environmental sensor node in C that can be reconfigured for water, air or soil monitoring just by selecting a profile. The drivers, filtering, event detection, storage and telemetry code are shared. The node adapts both how often it samples and what it transmits: summaries when things are stable, immediate alerts when a threshold is crossed. In a software-in-the-loop simulation with identical test traces, it cut radio transmissions by about 82–88 % compared with fixed-period reporting, and it still detected every critical event with no false alarms. I tested it with a virtual sensor backend, a MATLAB reference and an emulated Cortex-M4 in Renode. There was no physical hardware.

## Two-minute technical explanation

> The firmware is portable C99 with no dynamic memory. A hardware abstraction layer exposes ADC, I2C, SPI, UART, GPIO, timer and sensor-power functions. Drivers are function-pointer tables with probe, read_raw and convert. An application profile is a constant struct that selects the drivers and holds calibration, thresholds, hysteresis, sampling intervals and telemetry rules, so water, air and soil share every line of the processing code. Each wake-up powers the sensors, reads and validates them, runs a Hampel outlier filter followed by a moving average or FIR, updates a per-channel state machine with hysteresis and confirmation, and stores the record in a circular log. A telemetry policy then decides whether to send an alert, an update with only the changed channels, a periodic summary, or nothing. Packets use one binary format with int16 fixed-point values and a CRC-16. The link layer handles ACKs, bounded retries, outage backoff and a priority store-and-forward queue.
>
> Because there is no hardware, I wrote virtual devices that encode physical values into realistic bus data (ADC counts, register words with CRC-8, UART frames), so the drivers are genuinely exercised. I compared fixed reporting, adaptive telemetry only, and adaptive sampling plus telemetry over eight scenarios per profile, using identical traces and seeds. Transmissions fell by 82–88 %. Estimated energy fell by 42–72 %, depending on whether the sensors or the radio dominate; in the air profile the PM sensor dominates, so only sampling less helped. Detection was faster thanks to burst sampling on suspect readings. The costs were slower sensor-fault detection and a coarser picture at the gateway. Energy is a model estimate based on RAK3172 datasheet currents plus documented assumptions. The same code runs on an emulated STM32F4 in Renode with results identical to the native build.
