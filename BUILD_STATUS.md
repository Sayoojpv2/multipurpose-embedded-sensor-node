# Build status

| Step | Status | Notes |
|---|---|---|
| Environment check | done | Windows 11 Home 10.0.26200. Python 3.14.2 + numpy 2.5.3 / matplotlib 3.11.2. MATLAB R2026a Update 4. Arm GNU Toolchain 12.2.MPACBTI-Rel1 (arm-none-eabi-gcc 12.2.1). Native compiler: clang 20.1.5 bundled with OMNeT++ 6.4 (`gcc.exe` driver). Installed: GitHub CLI 2.103.3 (winget), Renode 1.17.0 portable (official release, SHA-256 verified) in `%USERPROFILE%\tools\renode`. |
| Research | done (limited access) | Full texts of all three papers were NOT accessible (IEEE empty response, Elsevier redirect stub, MDPI HTTP 403). Read: Octopus project page, RAK3172 datasheet, Renode board list. See `docs/research_comparison.md`. |
| Firmware core (C99) | done | HAL, 10 drivers, 3 profiles, sensor manager, DSP, events, adaptive sampling, telemetry, packet + CRC, link layer, storage, energy model |
| Native simulator | done | `build/sensor_node.exe` |
| C unit tests | done | 172 checks, 0 failures (2 real backend bugs found and fixed) |
| Experiments | done | 90 runs: 3 profiles × 8 scenarios × 3 strategies + sweeps |
| Python pipeline tests | done | 7/7 |
| MATLAB validation | done | C vs MATLAB max error ≤ 5.4e-6 |
| Renode / ARM | done | STM32F407 image runs on `stm32f4_discovery` (CPU, memory, USART2 emulated; sensors and radio virtual). Results identical to native. |
| Documentation | done | `docs/` |
| GitHub publish | done | https://github.com/Sayoojpv2/multipurpose-embedded-sensor-node (public) |

Note: the session folder path was longer than Windows MAX_PATH allows for the deepest output files, so the runs used a temporary `subst` drive. In a normal folder such as `C:\Users\<you>\Documents\...` this is not needed.
