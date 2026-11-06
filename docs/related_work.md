# Related work

Low-power, modular environmental sensor nodes are an established topic. This project does not claim a new concept. Its contribution is narrower: one firmware core configured by profile data, and a reproducible software comparison of fixed and adaptive sampling/telemetry on identical traces.

## Publications

The bibliographic details below were checked against the Crossref DOI registry. Each description is limited to what the title or published abstract states; the full texts were not reviewed for this project.

1. **F. Leccese and R. Arciulo**, "A Modular Low-Power Environmental Monitoring Node with RAK3172 LoRaWAN Core, Dual-Path Logging and Low-Cost Sensors," *2026 IEEE International Workshop on Metrology for Industry 4.0 & IoT (MetroInd4.0 & IoT)*, pp. 523–528, 2026. doi:10.1109/MetroInd4.0IoT69397.2026.11653126.
   As the title states, it describes a modular low-power node built around the RAK3172 LoRaWAN module, with dual-path logging and low-cost sensors. This project uses the RAK3172 datasheet currents for its radio energy model, but it does not implement LoRaWAN.

2. **O. Oyinlola, Å. Håtveit, C. Elalami, S. Mora, R. Dubey, F. Duarte, J. Biagioli, M. Gregis and C. Ratti**, "Octopus: A low-cost, modular environmental sensing platform for makers," *HardwareX*, vol. 26, e00760, 2026. doi:10.1016/j.ohx.2026.e00760. Project page: https://senseable.mit.edu/octopus/.
   It is an open hardware platform. Per the project page, it is modular and Arduino-compatible, can be mounted stationary, wearable or on vehicles, and targets heat, air-quality and biodiversity sensing. No code from it is used here.

3. **M. J. C. S. Reis**, "Low-Power Embedded Sensor Node for Real-Time Environmental Monitoring with On-Board Machine-Learning Inference," *Sensors*, vol. 26, no. 2, 703, 2026. doi:10.3390/s26020703.
   According to the abstract, this is a MATLAB/Simulink system-level study of an air-quality node. It combines adaptive duty-cycling with on-device quantised neural classification (normal/anomalous/critical) and an adaptive LoRaWAN strategy, which reduced transmissions by about 88 % relative to periodic reporting.

## Relation to this project

| | Ref. 1 | Ref. 2 | Ref. 3 | This project |
|---|---|---|---|---|
| Focus (per title/abstract) | low-power node, RAK3172 LoRaWAN, dual logging | low-cost modular maker platform | low-power node with on-board ML, Simulink study | one firmware core with water/air/soil profiles |
| Event detection | – | – | quantised neural classifier | thresholds + hysteresis + confirmation |
| Communication | LoRaWAN (RAK3172) | – | adaptive LoRaWAN strategy | LoRa time-on-air model, ACK/retry, store-and-forward (no LoRaWAN stack) |
| Evaluation | not stated in the metadata | hardware platform (project page) | simulation (abstract) | native simulation + Renode CPU emulation; no hardware |

Ref. 3 reports a transmission reduction of the same order as this project (82–88 %). The scenarios, models and metrics differ, so the numbers should not be compared directly. This project deliberately uses rule-based detection: threshold events with hysteresis meet the requirement and can be explained and tested line by line.

## Datasheets, standards and tools used

- RAKwireless, *RAK3172 WisDuo LPWAN Module Datasheet*: TX 87 mA (20 dBm, 868 MHz), RX 5.22 mA, sleep 1.69 µA. https://docs.rakwireless.com/product-categories/wisduo/rak3172-module/datasheet/
- Semtech SX1276/77/78/79 datasheet, LoRa time-on-air formula, implemented in `lora_airtime_ms()` and checked against reference values in the unit tests.
- Renode 1.17.0 (Antmicro), `stm32f4_discovery` platform. https://renode.readthedocs.io/
- Driver register and frame layouts follow the public datasheet descriptions of the sensor families named in `firmware/drivers/` (ADS1115-style ADC, MAX31865-style RTD converter, Plantower PMS-style UART frame, Sensirion-style CRC-8 words, BH1750-style light sensor). They are re-implemented, not copied.
