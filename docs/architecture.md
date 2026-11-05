# Architecture

One sensor node, one simulated gateway. There is no network, routing or clustering.

```
                 +------------------------- firmware/ (portable C99, no malloc) -------------------------+
                 |                                                                                         |
 profile table ->|  sensor_manager --> dsp (Hampel -> MA -> FIR) --> event FSMs --> node state             |
 (water|air|soil)|        |                                              |             |                   |
                 |     drivers (10)                                   storage     telemetry policy          |
                 |        |                                        (circular log)      |                   |
                 |       HAL  (ADC, I2C, SPI, UART, GPIO, timer, sensor power)      packet + CRC-16         |
                 |        |                                                            |                   |
                 |        |                         adaptive sampler <-- cues      link layer (ACK, retry,  |
                 |        |                         (next wake-up)                  backoff, TX queue)      |
                 +--------|------------------------------------------------------------|-------------------+
                          |  hal_virtual.c                                             | radio_transmit()
                 +--------v-----------------------+                      +-------------v-----------------+
                 | simulation/sensors             |                      | simulation/radio              |
                 | virtual devices: ADC counts,   |                      | outage windows, loss, bit     |
                 | I2C registers, SPI bytes, UART |                      | errors, ACK loss  -> gateway  |
                 | frames  <- environment trace   |                      | (CRC check, dedupe, log)      |
                 +--------------------------------+                      +-------------------------------+
```

## Layers

| Layer | Files | Responsibility |
|---|---|---|
| Types | `firmware/include/node_types.h` | `sensor_reading_t` (id, type, unit, status, raw, value, timestamp), enums |
| HAL | `include/hal.h`, `hal/hal_virtual.c` | Bus API. In this project it is backed by virtual devices; a hardware port replaces only this file |
| Drivers | `drivers/analog_drivers.c`, `drivers/digital_drivers.c` | `probe / read_raw / convert` function tables, one per sensor family |
| Profiles | `profiles/profile_*.c`, `profile_registry.c` | **All** application-specific data |
| Sensor manager | `src/sensor_manager.c` | Registration and probing, power rails, acquisition, two-pass conversion (compensation channels last), range validation, fault status |
| DSP | `src/dsp.c` | Fixed-buffer Hampel identifier, O(1) moving average, circular-buffer FIR |
| Events | `src/event.c` | Per-channel FSM: NORMAL / WARNING / CRITICAL / RECOVERY / FAULT, hysteresis, N-sample confirmation |
| Sampling | `src/sampling.c` | STABLE / ACTIVE / ALERT / RECOVERY intervals from the profile; calm-sample confirmation before returning to STABLE |
| Telemetry | `src/telemetry.c` | FIXED: DATA every sample. ADAPTIVE: ALERT, UPDATE, FAULT, SUMMARY with delta and rate limits |
| Packet | `src/packet.c` | Common binary format with channel mask and CRC-16/CCITT-FALSE |
| Comms | `src/comms.c` | CRC-16, CRC-8, LoRa time on air, link layer with ACK, retries, exponential backoff, urgency cap, store-and-forward queue (128 packets) |
| Storage | `src/storage.c` | Circular log with sequence numbers and a pluggable backend (RAM or file) |
| Energy | `src/energy.c` | Charge accounting by component (model estimate) |
| Node | `src/node.c` | One `node_step()` per wake-up, which ties all of the above together |

## Hardware model per profile (virtual boards)

| Profile | Channel | Driver / bus | Modelled after |
|---|---|---|---|
| Water | pH | ADC ch0 | glass electrode + gain stage, 2-point calibration |
| | Turbidity | ADC ch1 | optical sensor, linear V–NTU |
| | Conductivity | I2C 0x48 AIN0 | ADS1115-style ADC, temperature compensated to 25 °C |
| | Water temperature | SPI CS0 | MAX31865-style PT100 converter, Callendar-Van Dusen |
| Air | PM2.5 | UART | PMS-style 32-byte frame with checksum, 30 s fan warm-up |
| | CO2 | I2C 0x62 | SCD4x-style command words with CRC-8 |
| | RH, temperature | I2C 0x44 | SHT3x-style T/RH words with CRC-8 (one device, two channels) |
| Soil | Moisture | ADC ch0 | capacitive probe, dry/wet calibration |
| | Soil temperature | ADC ch1 | NTC 10k, Beta equation |
| | Soil EC | I2C 0x48 AIN1 | **same EC driver as water**, different calibration |
| | Light | I2C 0x23 | BH1750-style lux sensor |

## What changes between profiles and what does not

Changes (profile data only): the channel list, driver pointers, bus addresses, power rails, calibration `p[]`, valid range, packet scale, filter choice (MA or FIR), Hampel settings, thresholds and hysteresis, delta and rate-of-change cues, sensor power parameters, the five sampling intervals, event confirmation counts, and the summary, update and alert-repeat periods.

Unchanged: every file in `firmware/src`, `firmware/hal` and `firmware/drivers`.

## Packet format (all profiles)

```
byte 0     version(4) | type(4)        DATA=1 SUMMARY=2 UPDATE=3 ALERT=4 FAULT=5
1-2        node id (LE)
3          profile id                  1 water, 2 air, 3 soil
4-5        sequence number
6-9        timestamp [s]
10         priority(2) | node state(3) | flags(3)   flag0 = delivered from store-and-forward queue
11         channel mask (bit i = channel i present)
12..       per channel: sensor id, status [state(3)|valid(1)|read status(4)], value int16
           SUMMARY: + min int16 + max int16
last 2     CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over all previous bytes
```
Sizes: DATA with 4 channels is 30 B; ALERT with 1 channel is 18 B; SUMMARY with 4 channels is 46 B. A LoRaWAN-style MAC overhead of 13 B is added for airtime only.

## ARM / Renode target

`firmware/targets/stm32f4/`: startup code, linker script, and a `main_arm.c` that prints through **real USART2 register accesses** on Renode's `stm32f4_discovery` model. The sensors and the radio are still the virtual backend, because Renode's STM32F4 model has no ADC and no sensor models on its buses. Sensor input comes from an embedded trace (`trace_data.h`, generated by `python/generate_data.py`).


## Timing model

When a sensor rail is switched on, the sensor manager finds the longest configured warm-up plus measurement interval among the newly enabled sensors. The virtual clock advances by that interval before the drivers read the devices. The energy model charges each newly enabled sensor for the same shared power-up interval. This models elapsed application time; it does not simulate analog settling physics or measure MCU cycles.

The radio link keeps a shared millisecond timeline. Each queued packet starts after the preceding uplink airtime and ACK receive interval. Failed attempts include the receive timeout and configured retry backoff. Gateway receive timestamps are recorded at the end of uplink airtime. This is a LoRa-style link model, not a LoRaWAN MAC implementation.
