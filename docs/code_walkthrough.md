# Code walkthrough

This guide reads the repository in the order the data flows. For each step it lists the files and functions to open. **Must know** marks what you should be able to explain without notes; **Look up** marks detail you can check in the code when needed.

---

## 1. How the program starts and runs

**Files:** `simulation/main_sim.c` → `main()`; `firmware/src/node.c` → `node_init()`, `node_step()`, `node_finish()`.

`main()` parses `--profile/--strategy/--trace/...`, loads the trace CSV, installs the virtual "board" for the profile (`vdev_load_board()`), sets up the radio and gateway models, then runs:

```c
while (t < dur) { hal_sim_set_time(t); t = node_step(&node, t); }
```

Each call is one wake-up of the node, and the return value is the next wake-up time. There is no real sleeping: simulated time jumps straight to the next wake-up. On the ARM image (`firmware/targets/stm32f4/main_arm.c`) the same loop runs inside Renode.

**Must know:** one `node_step()` = one wake-up. Its order is: sensors → filters → events → storage → telemetry → radio → next interval.
**Look up:** CSV output callbacks (`on_sample`, `on_attempt`, `on_gateway`) and `summary.json` fields.

## 2. How a profile is selected

**Files:** `firmware/profiles/profile_water.c` (and `air`, `soil`), `profile_registry.c` → `profile_find()`, `profile_get()`; `firmware/include/profile.h`.

`--profile water` → `profile_find("water")` returns `&PROFILE_WATER_CFG`, a `const app_profile_t`. `node_init()` copies a pointer to it, and every module reads its parameters from there: driver pointer, calibration `cal.p[]`, thresholds `thr`, sampling intervals, reporting periods.

**Must know:** a profile is data, not code. The EC driver is shared by water (`cal.p[0]=0.001`) and soil (`0.0005`).
**Look up:** individual field values.

## 3. How a measurement is obtained

**Files:** `firmware/src/sensor_manager.c` → `sensor_manager_sample()`; `firmware/drivers/analog_drivers.c`, `digital_drivers.c`; `firmware/hal/hal_virtual.c`; `simulation/sensors/virtual_sensors.c` → `vdev_adc()`, `vdev_i2c_read()`, …

`sensor_manager_sample()` works in four steps:

1. Switches on the power rails of sensors that are off.
2. Advances the model clock by the longest warm-up of those sensors (pH: 2.0 + 0.1 s).
3. Calls each driver's `read_raw()` (a bus transaction through the HAL).
4. Calls `convert()` (raw value → engineering units), then `sensor_validate()` (range check).

Channels that need compensation (EC needs temperature) are converted in a second pass.

**Example (pH, ADC).** The virtual electrode at pH 7.00 outputs 1.680 V, so `raw = 1.680 / 3.3 × 4095 ≈ 2085` counts. The driver (`ph_convert`) computes `V = 2085 × 3.3 / 4095 = 1.6803 V` and `pH = 7 + (1.680 − 1.6803) / 0.17393 ≈ 6.998`. A raw value ≤ 2 or ≥ 4093 is treated as an open or shorted input (`RD_RAIL`).

**Must know:** the `probe/read_raw/convert` interface; why the virtual device is the *inverse* of the driver (so driver bugs show up as wrong values); the failure statuses (`RD_RAIL`, `RD_NO_RESPONSE`, `RD_CHECKSUM`, `RD_OUT_OF_RANGE`).
**Look up:** each sensor's register or frame layout, NTC Beta and PT100 Callendar–Van Dusen formulas.

## 4. How the filters work

**Files:** `firmware/src/dsp.c` → `chan_dsp_process()`, `hampel_update()`, `ma_update()`, `fir_update()`; checked by `matlab/validate_c_output.m`.

The chain per channel is Hampel → moving average (or FIR for PM2.5).

- **Hampel:** with window `[7.40, 7.41, 7.39, 7.40, 5.60]` the median is 7.40 and the MAD is 0.01. The limit is `max(3 × 1.4826 × 0.01, min_dev = 0.15) = 0.15`. Since |5.60 − 7.40| = 1.8 > 0.15, the 5.60 is replaced by 7.40 and the sample is flagged **suspect**. A real step is accepted once 3 of the 5 window samples are at the new level.
- **Moving average (length 4):** primed with 7.4, then input 5.8 → (7.4 × 3 + 5.8) / 4 = 7.0. It uses a running sum, re-summed once per buffer cycle to stop float drift.
- **FIR:** `y[n] = Σ h[k] x[n−k]` with `h = [1 6 15 20 15 6 1] / 64` (DC gain 1). The delay line is circular.

**Must know:** why the median filter comes first (spikes); that filtering adds lag.
**Look up:** priming details, the MATLAB comparison script.

## 5. How event detection works

**Files:** `firmware/src/event.c` → `event_update()`, `event_level()`, `event_node_state()`.

Each channel has a state: NORMAL, WARNING, CRITICAL, RECOVERY or FAULT. Example for water pH, where `crit_lo = 6.0` and `hyst = 0.1`:

- **Enter CRITICAL:** the filtered pH is ≤ 6.0 for `confirm_n = 2` consecutive samples.
- **Stay CRITICAL:** while pH ≤ 6.1. `event_level(v, relax = hyst)` moves the limit 0.1 towards normal.
- **Leave:** pH is above 6.1 for 2 samples → WARNING. If it is also above 6.6 (the 6.5 warning limit plus hysteresis), it goes straight to RECOVERY instead. RECOVERY becomes NORMAL after 5 normal samples.
- **Fault:** 3 consecutive invalid readings → FAULT; the next valid reading → RECOVERY.

The node state is the most severe channel state (CRITICAL > FAULT > WARNING > RECOVERY > NORMAL).

**Must know:** hysteresis and confirmation, and why they stop alarm chatter (`tests/test_firmware.c`, `test_events`).
**Look up:** the counter bookkeeping in `event_update()`.

## 6. How adaptive sampling works

**Files:** `firmware/src/sampling.c` → `sampler_next()`; cue collection in `node_step()`.

`node_step()` collects cues: the node state, a pending (unconfirmed) level, a raw value beyond a threshold, a Hampel-suspect sample, and the rate of change. `sampler_next()` maps them to a mode:

- critical or pending critical → **ALERT** (water: 15 s);
- warning, suspect sample or fast change → **ACTIVE** (60 s);
- recovery or fault → **RECOVERY** (60 s);
- otherwise **STABLE** (300 s), but only after 5 calm samples in a row.

The next wake-up is `wake_t + interval`. A sensor whose warm-up is longer than the interval stays powered.

**Must know:** the reason for burst sampling on *suspect* samples; the trade-off shown in `stable_interval_tradeoff.png` (longer stable interval = less energy, longer delay).
**Look up:** the calm-counter details.

## 7. How a packet is built and sent

**Files:** `firmware/src/telemetry.c` → `telemetry_on_sample()`, `adaptive_decide()`; `firmware/src/packet.c` → `packet_encode()`; `firmware/src/comms.c` → `link_submit()`, `link_service()`, `send_item()`, `lora_airtime_ms()`, `crc16_ccitt_false()`.

- **FIXED:** every sample produces a DATA packet. **ADAPTIVE:** `adaptive_decide()` checks, in order: new critical → ALERT; still critical → repeat ALERT; state change → UPDATE/FAULT; value moved by `delta_report` → UPDATE with only the changed channels; summary period elapsed → SUMMARY; otherwise nothing.
- **Size:** a 12-byte header, 4 bytes per channel (id, status, int16 value), and a 2-byte CRC. A DATA packet with 4 channels is 12 + 16 + 2 = 30 B. pH 6.12 is sent as the int16 612 (scale 100).
- **CRC-16/CCITT-FALSE:** polynomial 0x1021, initial value 0xFFFF; "123456789" → 0x29B1.
- **Airtime (SF9, 125 kHz):** 30 B + 13 B MAC overhead = 43 B → 58 symbols × 4.096 ms + 12.25-symbol preamble ≈ 288 ms.
- **Link:** `link_submit()` puts the packet in a 128-entry priority queue. `send_item()` makes up to 4 attempts, each costing airtime plus an ACK receive or timeout, with 2–6 s backoff between them. After the 4th failure the link is marked down and probed again with exponential backoff, capped at 300 s while an alert is queued.

**Must know:** packet layout, why CRC, why ACK + retry + queue, airtime vs payload size.
**Look up:** the exact Semtech formula terms and the queue eviction rule.

## 8. How the gateway receives it

**Files:** `simulation/radio/radio_sim.c` → `radio_transmit()`, `gateway_receive()`.

`radio_transmit()` decides the attempt's fate: it fails if the trace says the link is down, it fails with probability `p_loss`, and with probability `p_corrupt` it flips one bit. Otherwise `gateway_receive()` runs `packet_decode()`. A CRC mismatch means no ACK. A sequence number already seen means "duplicate": it is ACKed again but not logged twice. Finally the ACK itself can be lost (`p_ack_loss`). The receive time is the end of the uplink airtime.

**Must know:** this is a channel model, not a LoRaWAN network server.

## 9. How energy is estimated

**Files:** `firmware/src/energy.c` → `energy_default_cfg()`, `energy_finalize()`; accounting in `node_step()`.

Charge in mA·s is summed per component, and energy = charge × 3.3 V.

- **Example (one water wake-up):** all four sensors are on for 2.1 s. Turbidity alone draws 30 mA × 2.1 s = 63 mA·s. One 30-byte packet costs 87 mA × 0.288 s ≈ 25 mA·s.
- **Sleep:** 1.69 µA for the rest of the time.
- **Average current** = total charge / duration; **battery life** = capacity / average current.

**Must know:** which values come from the datasheet (radio TX/RX/sleep, RAK3172) and which are assumptions (MCU, sensors); why air-profile energy is dominated by the PM2.5 fan.

## 10. How the experiment pipeline produces results

**Files:** `python/generate_data.py` → `make_trace()`; `python/run_experiments.py`; `python/analyse_results.py` → `analyse_run()`; `python/plots.py`; `python/seed_study.py`.

1. **`make_trace()`** builds the noise-free truth (nominal value + daily cycle + scenario shape), adds noise, spikes and fault flags, and writes `t, v0..v3, f0..f3, link, tv0..tv3`. The thresholds come from the firmware itself (`sensor_node --dump-profile`).
2. **`run_experiments.py`** runs every profile × scenario × strategy on the *same* trace and seed, plus two sweeps.
3. **`analyse_run()`** compares node behaviour with the truth. An event is crossings merged within 1800 s; detection means CRITICAL within ±600 s; it also computes delay, alarm coverage, false alarms, fault detection, data loss and reconstruction error.
4. **`seed_study.py`** repeats the water comparison over 10 seeds.

**Must know:** identical inputs for each strategy, how "detected" and "delay" are defined, and why the earlier crossing-based count was misleading (technical report §5).

---

## Suggested study order

1. `node.c` `node_step()`: get the big picture.
2. `profile_water.c`: see what is configurable.
3. `sensor_manager.c` + `analog_drivers.c` (pH), then `virtual_sensors.c` `vdev_adc()` side by side.
4. `dsp.c`, then run `matlab/validate_c_output.m`.
5. `event.c` together with `test_events` in `tests/test_firmware.c`.
6. `sampling.c`.
7. `telemetry.c` → `packet.c` → `comms.c` (`send_item`).
8. `radio_sim.c`.
9. `energy.c` + the energy lines in `node_step()`.
10. `generate_data.py` → `analyse_results.py`, then read `results_summary.md`.

A good exercise is to change `stable_s` in `profile_water.c`, run `run.bat`, and explain how the energy and delay columns change.
