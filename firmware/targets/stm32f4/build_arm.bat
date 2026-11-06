@echo off
rem Cross-compile the firmware for STM32F407 (Cortex-M4, soft-float) with the Arm GNU Toolchain.
setlocal
cd /d "%~dp0\..\..\.."
set "ARMCC=arm-none-eabi-gcc"
where %ARMCC% >nul 2>nul || (
  for /d %%D in ("C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\*" "C:\Program Files\Arm GNU Toolchain arm-none-eabi\*") do (
    if exist "%%~D\bin\arm-none-eabi-gcc.exe" set "PATH=%%~D\bin;%PATH%"
  )
)
where %ARMCC% >nul 2>nul || (echo ERROR: arm-none-eabi-gcc not found & exit /b 1)
if not exist firmware\targets\stm32f4\trace_data.h (echo ERROR: trace_data.h missing - run run_renode.bat, which generates it & exit /b 1)
if not exist build mkdir build
set FLAGS=-mcpu=cortex-m4 -mthumb -mfloat-abi=soft -O2 -std=c99 -Wall -Wextra -ffunction-sections -fdata-sections -Ifirmware/include -Isimulation
set FW=firmware/src/dsp.c firmware/src/event.c firmware/src/sampling.c firmware/src/packet.c firmware/src/comms.c firmware/src/storage.c firmware/src/sensor_manager.c firmware/src/telemetry.c firmware/src/energy.c firmware/src/node.c firmware/drivers/analog_drivers.c firmware/drivers/digital_drivers.c firmware/profiles/profile_water.c firmware/profiles/profile_air.c firmware/profiles/profile_soil.c firmware/profiles/profile_registry.c firmware/hal/hal_virtual.c
set SIM=simulation/sensors/virtual_sensors.c simulation/radio/radio_sim.c
set TGT=firmware/targets/stm32f4/main_arm.c firmware/targets/stm32f4/startup.c
%ARMCC% %FLAGS% %FW% %SIM% %TGT% -nostartfiles --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -Wl,-Map=build/sensor_node_stm32f4.map -T firmware/targets/stm32f4/linker.ld -lm -o build/sensor_node_stm32f4.elf || exit /b 1
arm-none-eabi-size build/sensor_node_stm32f4.elf
echo built build\sensor_node_stm32f4.elf
