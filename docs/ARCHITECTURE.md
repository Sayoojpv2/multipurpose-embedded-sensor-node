# Architecture

One sensor node and one simulated gateway. Everything under `firmware/` is portable C99 with no dynamic memory; everything under `simulation/` exists only because there is no hardware.

```
profile table (water | air | soil)
        |
        v
+----------------------------- firmware/ ------------------------------+
| sensor_manager -> dsp (Hampel -> MA/FIR) -> event FSM -> node state   |
|      |                                          |          |          |
|   drivers                                   storage    telemetry      |
|      |                                   (circular log)    |          |
|     HAL (ADC, I2C, SPI, UART, GPIO, timer, power)     packet + CRC-16 |
|      |                     sampler -> next wake-up    link (ACK/retry, |
|      |                                                 queue)         |
+------|--------------------------------------------------|------------+
       | hal_virtual.c                                    | radio_transmit()
+------v-----------------------+          +---------------v--------------+
| simulation/sensors           |          | simulation/radio              |
| virtual devices produce ADC  |          | outage, loss, bit errors, ACK |
| counts, register bytes, UART |          | loss -> gateway (CRC check,   |
| frames from a trace          |          | duplicate filter, log)        |
+------------------------------+          +-------------------------------+
```

## Design decisions

| Decision | Why |
|---|---|
| **HAL separated from drivers** | Drivers only call `hal_adc_read()`, `hal_i2c_read()`, and so on. Here the HAL is backed by virtual devices; a hardware port rewrites only `firmware/hal/hal_virtual.c`, while drivers, processing and tests stay unchanged. |
| **Profiles are `const` data** | Water, air and soil share every line of processing code. A profile (`firmware/profiles/profile_*.c`) selects the drivers and holds calibration, thresholds and timing. Constants can live in flash. |
| **Drivers are function-pointer tables** (`probe`, `read_raw`, `convert`) | The sensor manager handles every sensor the same way. The water and soil EC channels share one driver with different calibration. |
| **Virtual devices emit bus-level data** | The drivers decode real formats (ADC counts, ADS1115-style registers, MAX31865-style SPI bytes, PMS-style UART frames, Sensirion-style CRC-8 words), so driver bugs show up as wrong values in tests. |
| **Hampel before the moving average** | An average spreads a spike over several samples; a median test removes it. |
| **Hysteresis + 2-sample confirmation** | Prevents alarm chatter when a value hovers at a threshold. |
| **Burst sampling on *suspect* samples** | A long stable interval would otherwise delay confirming a real change by several intervals. |
| **Wake-up period measured from the wake time** | An RTC alarm fires at `wake + interval`, so sensor warm-up does not stretch the period. |
| **ACK + bounded retries + priority queue** | The node knows what was delivered, and alerts go first after an outage. Every sample is also kept in the local log. |

## One wake-up through the code

`simulation/main_sim.c` calls `node_step(&node, t)` in a loop. Each call is one wake-up and returns the next wake-up time. The ARM image (`firmware/targets/stm32f4/main_arm.c`) runs the same loop.

| Step | Code | What happens |
|---|---|---|
| 1. Acquire | `sensor_manager_sample()` (`src/sensor_manager.c`) | Powers sensor rails, advances the model clock by the longest warm-up, calls each driver's `read_raw()` then `convert()`, and range-checks the result (`sensor_validate()`). Temperature-compensated channels are converted in a second pass. |
| 2. Filter | `chan_dsp_process()` (`src/dsp.c`) | Hampel test, then moving average or FIR. |
| 3. Events | `event_update()` (`src/event.c`) | Per-channel state machine; the node state is the most severe channel. |
| 4. Store | `storage_append()` (`src/storage.c`) | Every sample goes into a circular log with a sequence number. |
| 5. Report | `telemetry_on_sample()` (`src/telemetry.c`) | Decides whether to send and what. |
| 6. Transmit | `packet_encode()`, `link_submit()`, `link_service()` (`src/packet.c`, `src/comms.c`) | Encodes the packet, queues it, and sends it with ACK, retries and backoff. |
| 7. Schedule | `sampler_next()` (`src/sampling.c`) | Chooses the next interval; sensors whose warm-up exceeds it stay powered. |

**Example: pH sample (ADC path).** The virtual electrode at pH 7.00 outputs 1.680 V → `raw = 1.680 / 3.3 × 4095 ≈ 2085`. The driver `ph_convert()` computes `V = 2085 × 3.3 / 4095 = 1.6803 V` and `pH = 7 + (1.680 − 1.6803) / 0.17393 ≈ 6.998`. A raw value ≤ 2 or ≥ 4093 is flagged `RD_RAIL` (open or shorted input).

## Signal processing

- **Hampel** (window 5): with samples `[7.40, 7.41, 7.39, 7.40, 5.60]`, the median is 7.40 and the MAD is 0.01, so the limit is `max(3 × 1.4826 × 0.01, min_dev = 0.15) = 0.15`. The 5.60 is replaced by 7.40 and flagged *suspect*. A genuine step is accepted once 3 of the 5 samples sit at the new level.
- **Moving average:** a running sum over a circular buffer, re-summed once per buffer cycle so float rounding does not accumulate.
- **FIR:** `y[n] = Σ h[k] x[n−k]` with binomial taps `[1 6 15 20 15 6 1]/64` (DC gain 1). It is used for PM2.5.

All three are checked against an independent MATLAB reference (`matlab/validate_c_output.m`).

## Event state machine

The states are NORMAL, WARNING, CRITICAL, RECOVERY and FAULT. Example for water pH (`crit_lo = 6.0`, `hyst = 0.1`, `confirm_n = 2`):

- The channel becomes CRITICAL after 2 consecutive filtered values ≤ 6.0.
- It stays CRITICAL while the value is ≤ 6.1; `event_level(v, relax = hyst)` moves the limit 0.1 towards normal.
- Above 6.1 for 2 samples it drops to WARNING, or straight to RECOVERY if it is also back above 6.6. RECOVERY becomes NORMAL after 5 normal samples.
- 3 consecutive invalid readings → FAULT; the next valid reading → RECOVERY.

Node severity order: CRITICAL > FAULT > WARNING > RECOVERY > NORMAL.

## Adaptive sampling and telemetry

`sampler_next()` maps cues to a mode:

| Cue | Mode | Water interval |
|---|---|---|
| critical, or a critical level pending confirmation | ALERT | 15 s |
| warning, Hampel-suspect sample or fast rate of change | ACTIVE | 60 s |
| recovery or sensor fault | RECOVERY | 60 s |
| 5 calm samples in a row | STABLE | 300 s |

The `fixed` strategy always uses `fixed_s` (60 s for water).

Adaptive telemetry (`adaptive_decide()`) checks these rules in order:

1. A new critical state sends an ALERT immediately.
2. A channel still critical repeats the ALERT every `crit_repeat_s`.
3. A channel state change sends an UPDATE (or FAULT).
4. A value that moved by `delta_report` sends an UPDATE with only the changed channels.
5. If `summary_s` has elapsed, a SUMMARY (mean/min/max) is sent.
6. Otherwise nothing is sent.

The `fixed` strategy sends a DATA packet on every sample.

## Packet format (all profiles)

```
byte 0     version(4) | type(4)        DATA=1 SUMMARY=2 UPDATE=3 ALERT=4 FAULT=5
1-2        node id (little endian)
3          profile id                  1 water, 2 air, 3 soil
4-5        sequence number
6-9        timestamp [s]
10         priority(2) | node state(3) | flags(3)   flag bit 0 = sent late from the queue
11         channel mask (bit i = channel i present; bits beyond the profile are invalid)
12..       per channel: sensor id, status [state(3)|valid(1)|read status(4)], value int16
           SUMMARY adds min int16, max int16
last 2     CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over all previous bytes
```

- Values are fixed-point int16 with a per-channel scale; for example pH × 100, so 6.12 → 612. `0x8000` means "no value".
- A DATA packet with 4 channels is 12 + 16 + 2 = 30 B; an ALERT for one channel is 18 B.
- `packet_encode()` computes the length first and writes nothing if the buffer is too small or the packet is invalid. `packet_decode()` checks the length, the CRC, the version, the profile, the type and the mask.

## Link and gateway

- `send_item()` makes up to 4 attempts. Each one occupies the uplink airtime plus either the ACK airtime or a receive timeout, and failed attempts add a 2–6 s random backoff.
- After 4 failures the link is marked down and probed again with exponential backoff (120 s → 1800 s), capped at 300 s while an ALERT is queued.
- The queue holds 128 packets; when full, the lowest-priority, oldest packet is dropped.
- `radio_transmit()` (`simulation/radio/radio_sim.c`) applies outages from the trace, uplink loss, bit corruption and ACK loss. `gateway_receive()` checks the CRC, drops duplicate sequence numbers (it still ACKs them) and time-stamps the packet at the end of its airtime.

Airtime example (SF9, 125 kHz, CR 4/5): a 30 B packet plus 13 B of LoRaWAN-style MAC overhead takes (12.25 + 58) symbols × 4.096 ms ≈ 288 ms. This is a LoRa time-on-air model, not a LoRaWAN implementation.

## Energy estimate

Charge in mA·s is accumulated per component in `node_step()` and converted to energy at 3.3 V (`firmware/src/energy.c`):

- **Sensors:** active current × on-time (warm-up + measurement), and off-state current otherwise. Sensors switched on at the same wake-up stay powered until the slowest one is ready.
- **MCU:** idle current during conversion; active current for 3 ms per sample and 2 ms per packet.
- **Radio:** 87 mA × airtime per attempt, plus 5.22 mA × ACK or timeout time.
- **Sleep:** 1.69 µA for the remaining time.

Example (one water wake-up): turbidity alone draws 30 mA × 2.1 s = 63 mA·s, while one 30 B packet costs 87 mA × 0.288 s ≈ 25 mA·s. Sensors therefore dominate the water budget. The radio and sleep currents are from the RAK3172 datasheet; the MCU and sensor values are assumptions.

## Virtual boards

| Profile | Channel | Bus | Modelled on |
|---|---|---|---|
| Water | pH | ADC ch0 | glass electrode + gain stage, 2-point calibration |
| | Turbidity | ADC ch1 | optical sensor, linear V–NTU |
| | Conductivity | I2C 0x48 AIN0 | ADS1115-style ADC, compensated to 25 °C |
| | Temperature | SPI CS0 | MAX31865-style PT100 converter, Callendar–Van Dusen |
| Air | PM2.5 | UART | PMS-style 32-byte frame with checksum, 30 s fan warm-up |
| | CO2 | I2C 0x62 | SCD4x-style words with CRC-8 |
| | RH, temperature | I2C 0x44 | SHT3x-style words with CRC-8 (one device, two channels) |
| Soil | Moisture | ADC ch0 | capacitive probe, dry/wet calibration |
| | Temperature | ADC ch1 | NTC 10 k, Beta equation |
| | Conductivity | I2C 0x48 AIN1 | same EC driver as water, different calibration |
| | Light | I2C 0x23 | BH1750-style lux sensor |

## Timing model

- Readings are time-stamped after sensor warm-up, but the next wake-up stays `wake_t + interval`.
- The model clock (`hal_sim_advance_time_ms()`) represents elapsed application time. It is not analog settling physics or MCU cycle timing.
- The radio keeps one millisecond timeline (`link_t.next_tx_ms`), so queued packets and retries never overlap.
- The link is serviced, and probed during outages, only when the node wakes.

## ARM / Renode target

`firmware/targets/stm32f4/` holds startup code, a linker script and `main_arm.c`, which prints through USART2 register writes on Renode's `stm32f4_discovery` model. Renode emulates the CPU, memory and that USART. The sensors and the radio are still the virtual backend compiled into the image, fed by a 3 h water trace that `run_renode.bat` generates into `trace_data.h`.
