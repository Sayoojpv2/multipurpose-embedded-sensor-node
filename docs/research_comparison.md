# Research comparison

## Access to the sources: what was actually read

| Source | What was accessible from this environment (2026-11-05) |
|---|---|
| **[P1]** *A Modular Low-Power Environmental Monitoring Node with RAK3172 LoRaWAN Core, Dual-Path Logging and Low-Cost Sensors*, IEEE MetroInd4.0&IoT 2026, doi:10.1109/MetroInd4.0IoT69397.2026.11653126 | **Full text not accessible.** The DOI resolves to IEEE Xplore document 11653126, which returned an empty page to the automated fetch; the browser route also failed. Only the **title and publication information** were available. |
| **[P2]** *Octopus: A Low-Cost, Modular Environmental Sensing Platform for Makers*, HardwareX 2026, doi:10.1016/j.ohx.2026.e00760, https://senseable.mit.edu/octopus/ | **Full text not accessible** (the Elsevier link returned only a redirect stub, and the PDF download timed out). The **project web page was read.** It says the platform is modular, Arduino-compatible, can be used stationary, wearable or on vehicles, displays data, "connects to other devices based on configuration", targets heat, air quality and biodiversity sensing, and comes with 3D-printing, soldering and coding guides. It links to a documentation site, which was not read. **No source code or licence was inspected, and no code from it was reused.** |
| **[P3]** *Low-Power Embedded Sensor Node for Real-Time Environmental Monitoring with On-Board Machine-Learning Inference*, MDPI Sensors 26(2):703, 2026, doi:10.3390/s26020703 | **Full text not accessible**: mdpi.com returned HTTP 403 to automated access. Only the **title and publication information** were available. The topics listed in the project brief (adaptive duty cycling, local processing) could not be checked against the text. |
| RAK3172 module datasheet (RAKwireless) | **Read.** Based on STM32WLE5CC; TX 87 mA @ 20 dBm 868 MHz, RX 5.22 mA, sleep 1.69 µA typical @ 3.3 V. These are the only datasheet currents used in the energy model. |
| Renode supported boards + releases | **Read.** `stm32f4_discovery` and `nrf52840` are listed; no STM32WL/RAK3172 board was found. Renode 1.17.0 portable was used. |

Because of these access limits, the table below separates **what the title or page says** from **what is inferred**. Cells marked "not verified" must not be quoted as facts about the papers.

## Comparison table

| | [P1] RAK3172 node (IEEE 2026) | [P2] Octopus (HardwareX 2026) | [P3] Low-power node + on-board ML (Sensors 2026) | **This project** |
|---|---|---|---|---|
| Processor / architecture | RAK3172 module, which per its datasheet contains an STM32WLE5 (Cortex-M4 + sub-GHz radio in one chip). Board architecture: not verified | Arduino-compatible microcontroller (project page); exact MCU not verified | not verified | Portable C99 firmware core; native host build plus a Cortex-M4 (STM32F407) image run in Renode |
| Sensor interfaces | "Low-cost sensors" (title); buses not verified | Modular attachments (page); buses not verified | not verified | HAL for ADC, I2C, SPI, UART, GPIO, timer and sensor power; 10 drivers covering all four buses |
| Applications | Environmental monitoring (title) | Heat, air quality, biodiversity (page) | Real-time environmental monitoring (title) | Water, air and soil, as **profiles of one firmware** |
| Local processing | not verified | not verified | On-board machine-learning inference (title) | Hampel outlier rejection, moving average, FIR, hysteresis events, adaptive sampling. **No ML** (not needed for threshold events) |
| Data storage | "Dual-path logging" (title); probably local plus remote, details not verified | not verified | not verified | Circular record store with sequence numbers (RAM or file-backed flash emulation); store-and-forward TX queue |
| Communication | LoRaWAN through the RAK3172 (title) | "Connects to other devices based on configuration" (page) | not verified | LoRa-style airtime model, ACK, bounded retries, outage backoff, CRC-16 common packet. **Not a LoRaWAN stack** |
| Power management | "Low-power" (title); method not verified | not verified | "Low-power" (title); adaptive duty cycling is mentioned in the brief but not verified | Explicit charge model (sensor warm-up, MCU, TX/RX, sleep); adaptive sampling and sensor power switching |
| Limitations (of our knowledge) | Text not read | Text not read; licence not checked | Text not read | Software only: virtual sensors and radio; energy is a model estimate |
| Possible extension | Profile-driven firmware on top of a RAK3172-class core | Common firmware and telemetry policy for modular attachments | Compare rule-based adaptation with ML-based adaptation | See "gap" below |

## Gap addressed (and what is *not* claimed)

Modular, low-power environmental nodes are well established. All three references belong to that line of work, and the concept itself is **not** new. This project does not claim novelty for modular sensing, LoRa telemetry, adaptive sampling or event-driven reporting.

The realistic, narrower gap this project works on:

1. **One firmware core, configured as data.** The sensor list, driver selection, calibration, thresholds, hysteresis, sampling intervals and telemetry rules all live in a profile table (`firmware/profiles/*.c`). The sensor manager, DSP, event, sampling, telemetry, link and storage modules are byte-for-byte identical across the water, air and soil profiles.
2. **Adaptive sampling and adaptive telemetry evaluated separately and together** (ablation `adaptive_tx` vs `adaptive`), on **identical input traces** with ground truth, across three environmental domains. The evaluation also states the costs: slower sensor-fault detection, more samples during oscillating events, and a coarser picture at the gateway.
3. **Testing without hardware that can be reproduced:** a virtual sensor backend behind a real HAL, so the drivers decode realistic bus data; a unit and pipeline test suite; a MATLAB reference for the DSP; and the same core running on an emulated Cortex-M4 (Renode) with results matching the native build.

## Licences

No third-party source code was copied. The register layouts, frame layouts and formulas that the drivers model (ADS1115-style ADC, MAX31865-style RTD converter, PMS-style UART frame, Sensirion-style CRC-8 words, BH1750-style light sensor, Callendar-Van Dusen, Beta equation, Semtech LoRa time-on-air) are taken from public datasheet descriptions and re-implemented here. Renode is used as an external tool and is not redistributed.
