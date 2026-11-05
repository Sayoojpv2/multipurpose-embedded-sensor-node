# Resume bullets (accurate to what was implemented and verified)

**Multipurpose Embedded Environmental Sensor Node (C, Python, MATLAB, Renode)**, software-only project

- Designed portable C99 firmware for a single environmental sensor node, with a hardware abstraction layer (ADC/I2C/SPI/UART/GPIO/timer/power), 10 function-pointer sensor drivers and three configuration-only application profiles (water, air, soil). No dynamic memory is used.
- Implemented per-channel signal processing: Hampel outlier rejection, a moving average and a 7-tap FIR. The C output matches an independent MATLAB reference to within 5.4e-6 (single-precision level).
- Built an event state machine with hysteresis and N-sample confirmation, plus an adaptive sampling engine (stable/active/alert/recovery) driven by suspect-sample and rate-of-change cues.
- Developed an event-driven telemetry policy, a compact common packet format with CRC-16, and a link layer with ACKs, bounded retries, outage backoff and a priority store-and-forward queue.
- In software-in-the-loop experiments on identical traces (3 profiles × 8 scenarios), radio transmissions fell by 82–88 % and model-estimated energy by 42–72 % compared with fixed-period reporting. All 12 critical events were detected with no false alarms.
- Created a virtual sensor backend that produces realistic bus data (ADC counts, CRC-8 register words, UART frames) and a test suite of 172 C checks and 7 Python end-to-end tests with fault injection and reproducibility checks.
- Cross-compiled the firmware for Cortex-M4 (arm-none-eabi-gcc) and ran it on Renode's STM32F4 Discovery model. Results were identical to the native build; the image uses 70 KB flash and 36 KB RAM.

*Do not add claims about physical hardware, measured power or a LoRaWAN stack; none of these were part of the work.*
