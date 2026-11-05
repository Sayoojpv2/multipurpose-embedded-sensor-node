# Interview questions and answers

These answers describe a software-only project: native simulation plus Renode emulation. Never say you measured a physical device.

## Project and architecture

**1. What does the project do, in one sentence?**
It is the C firmware for a single environmental sensor node that can be reconfigured for water, air or soil monitoring through a profile table, and it adapts its sampling and its transmissions to save radio traffic and energy. I evaluated it in simulation and on an emulated Cortex-M4.

**2. Why is this a "single node" project and not a WSN?**
The focus is on what happens *inside* the node: drivers, signal processing, events, power and telemetry policy. The other side is just one simulated gateway. There is no routing or clustering.

**3. How do you switch between water, air and soil?**
`--profile water|air|soil` or a config file. `node_init()` looks up a `const app_profile_t`. Every module reads its parameters from that struct, so no `#ifdef` or profile-specific code exists in the common modules.

**4. What exactly changes between profiles?**
The sensor list, driver pointers, bus addresses and power rails, calibration constants, valid ranges, packet scale factors, filter choice, thresholds and hysteresis, sampling intervals, event confirmation counts, and the summary, update and alert periods.

**5. How would you add a new sensor?**
Write a driver with `probe`, `read_raw` and `convert`, export the `sensor_driver_t`, and reference it from a profile channel. The manager, DSP, events, telemetry and packet code stay the same, because the packet already carries a sensor ID and a channel mask.

**6. Why the HAL layer?**
It separates "what the firmware wants" (read ADC channel 0) from "how this chip does it". Here it is backed by virtual devices; porting to an STM32WL would mean rewriting only `hal_virtual.c` against the vendor LL/HAL drivers.

## Embedded C, pointers, structures, memory

**7. How do you implement polymorphism in C?**
With a struct of function pointers (`sensor_driver_t`). The caller does `cfg->driver->read_raw(cfg, &raw)`, the same idea as a vtable.

**8. Why no `malloc`?**
Heap fragmentation and allocation failures are hard to test on small MCUs. With fixed buffers, RAM use is known at link time. The ARM build reports about 35.9 KB of data + bss; most of it is the 512-record log and the 128-packet queue.

**9. What does `const` buy you in the profiles?**
The profiles can live in flash instead of RAM. The compiler also rejects accidental writes. The node keeps one runtime copy only so that the stable interval can be overridden for experiments.

**10. What is `volatile` and where did you use it?**
It tells the compiler that every access has side effects and must not be cached or removed. It is used for the USART2 registers in `main_arm.c`; without it the compiler could optimise away the busy-wait on the TXE flag.

**11. How are multi-byte packet fields encoded?**
Explicitly, little endian, byte by byte (`put16`). I never `memcpy` a struct onto the wire, because padding and endianness would make the format compiler-dependent.

**12. Why int16 fixed point in packets instead of float?**
It is half the size of float32, and each channel has a scale (pH × 100, EC × 1, light × 0.5) chosen to cover its range at the needed resolution. `0x8000` is reserved for "no value".

## Sensor interfaces and ADC

**13. How do you convert ADC counts to volts?**
`V = raw × Vref / (2^N − 1)`; with 12 bits and 3.3 V that is about 0.8 mV per LSB. I average four conversions to reduce noise.

**14. How do you detect a broken analog sensor?**
A reading stuck near 0 or 4095 means open or shorted (`RD_RAIL`). The value is also checked against a plausible physical range.

**15. I2C vs SPI vs UART: when would you choose which?**
I2C uses two wires, addressed devices and a bus shared by many slow sensors (SHT, BH1750). SPI is faster, full duplex, needs one chip select per device, and suits converters such as the RTD chip. UART is point to point and suits streaming modules such as PM sensors.

**16. What happens on an I2C NACK in your code?**
The HAL returns `HAL_ERR_NACK`, the driver returns `RD_NO_RESPONSE`, and the reading is marked invalid. After 3 consecutive invalid readings the channel goes to FAULT and the adaptive telemetry sends a FAULT packet.

**17. What is temperature compensation for EC?**
Conductivity rises about 2 % per °C, so readings are normalised to 25 °C with `EC25 = EC_T / (1 + 0.02(T − 25))`. The manager converts the temperature channel first, then the channels that depend on it.

**18. How did you test drivers without hardware?**
The virtual devices implement the *inverse* transfer function. They take the physical value from a trace and produce ADC counts, register bytes with CRC-8, or a UART frame. The real driver must decode that back, and the tests check that it is within quantisation tolerance for all 12 channels.

## Digital filtering

**19. Why a Hampel filter before the moving average?**
A moving average spreads a spike over N samples, and a spike can still cross a threshold. The median-based Hampel test removes isolated spikes and still accepts a genuine step after 3 samples. In the noisy scenario, spikes beyond the critical limit caused zero false alarms.

**20. Explain your FIR filter.**
It is a 7-tap binomial low-pass `[1 6 15 20 15 6 1]/64`, implemented as a circular delay line with one multiply-accumulate per tap and unity DC gain. I validated it against MATLAB `filter()`: max error 8.6e-7, which is single-precision rounding.

**21. What is the trade-off of filtering for event detection?**
Filtering adds lag (a moving average of N samples delays a step by about N/2 samples), which increases detection delay. That is why the adaptive engine switches to fast sampling on a *suspect* sample, so the filter and confirmation stages finish quickly.

## Firmware logic: events, sampling, timers

**22. Why hysteresis and confirmation?**
Without them a value hovering at the threshold toggles the state on every sample, generating a burst of alerts. The unit test runs the same oscillation with and without hysteresis: one transition with it, several without.

**23. Describe the adaptive sampling modes.**
STABLE (long interval), ACTIVE (warning, rate of change or a suspect sample), ALERT (critical or pending critical), and RECOVERY (after an event or fault). The node returns to STABLE only after a number of calm samples.

**24. How do timers and low power fit in?**
`node_step()` returns the next wake-up time. On real hardware that time is programmed into the RTC alarm, the MCU enters a stop mode, and the RTC interrupt wakes it. In this project time is simulated; I did not implement interrupt handlers beyond the reset vector, and I say so.

**25. Why keep a sensor powered sometimes?**
If the next sample comes sooner than the warm-up time (the PM sensor needs about 30 s), switching it off and on again costs more than leaving it on, so the node keeps it powered in the 30 s alert mode.

## Communication, CRC, power

**26. How does CRC-16 work and what does it detect?**
The message is treated as a polynomial and divided by 0x1021; the remainder is appended. The receiver recomputes it. It detects all single-bit and all 2-bit errors, and every burst up to 16 bits. My test flips every bit of a packet and all are caught. It is not a security feature.

**27. How is radio airtime calculated?**
With the Semtech LoRa time-on-air formula: symbol time `2^SF/BW`, plus preamble symbols, plus payload symbols that depend on the payload length, SF, coding rate and the low-data-rate flag. I tested it against reference values (46.3 ms at SF7, 1155 ms at SF12 for 13 B).

**28. What happens during a radio outage?**
The first failed send (4 attempts) marks the link down. Packets go into a priority queue, and the node probes again with exponential backoff, capped at 300 s while an alert is waiting. After reconnection the backlog is sent alerts-first, flagged as stored, and the gateway removes duplicates. Raw samples are always in local storage.

**29. Did you implement LoRaWAN?**
No. I modelled LoRa airtime and confirmed-uplink-style ACKs; there is no join procedure, ADR or MAC commands. A real port would use an existing stack, such as RAK's RUI3 or LoRaMac-node.

**30. Where do your energy numbers come from?**
From a charge model. The TX, RX and sleep currents come from the RAK3172 datasheet; the MCU and sensor currents are documented assumptions. Results are estimates, for example "−63 % estimated energy for the water profile in simulation". They are not measurements.

**31. Why did adaptive telemetry alone save almost no energy in the air profile?**
Because the PM2.5 sensor's fan and laser (modelled at 80 mA for 31 s per sample) dominate the energy. Reducing transmissions does not touch that; only sampling less often does. This showed me that the right optimisation depends on the energy budget breakdown.

**32. What did adaptive sampling make worse?**
Sensor-fault detection, which was slower because a fault needs 3 invalid samples at the long interval (soil: about 4890 s vs 690 s with fixed sampling). An oscillating event also caused more samples than fixed. The gateway gets a coarser picture, though full data stays in local storage.

## Testing without hardware

**33. How did you test without any hardware?**
In layers: unit tests per module (172 checks), driver tests through the virtual HAL, fault injection (stuck, missing or NACKing sensors, bit errors, loss, outages), a MATLAB golden reference for the filters, end-to-end pipeline tests for reproducibility, and the same code on an emulated STM32F4 in Renode, which matched the native results.

**34. What does Renode emulate in your setup and what doesn't it?**
It emulates the Cortex-M4 CPU, flash, RAM and the USART2 register model, which I used for console output. It does not emulate the sensors or the radio: Renode's STM32F4 model has no ADC and no sensor models on the buses. So the virtual backend is compiled into the image and reads an embedded trace.

**35. How do you make experiments fair and reproducible?**
All strategies use identical input traces, the same seeds, and the same reliability layer and queue. The traces come from a seeded generator, and ground truth comes from the noise-free signal. Running twice gives bit-identical CSVs, which is tested.

**36. What would you do first on real hardware?**
Port the HAL to an STM32WL/RAK3172 board, keep the same tests as hardware-in-the-loop where possible, measure sleep and active current with a power analyser, replace the assumed parameters in the energy model, and re-run the comparison.
