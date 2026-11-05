@echo off
rem ---------------------------------------------------------------------------
rem build.bat - builds the native simulator and the unit tests.
rem   build.bat          native build (simulator + tests)
rem   build.bat arm      also cross-compile the STM32F4 image for Renode
rem Compiler search order: %CC%, gcc on PATH, OMNeT++ 6.x bundled clang/gcc.
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

if "%CC%"=="" (
  where gcc >nul 2>nul && set "CC=gcc"
)
if "%CC%"=="" (
  for /d %%D in ("%USERPROFILE%\Downloads\omnetpp-*" "%USERPROFILE%\omnetpp-*" "C:\omnetpp-*") do (
    for /d %%E in ("%%~D\omnetpp-*" "%%~D") do (
      if exist "%%~E\tools\win32.x86_64\clang64\bin\gcc.exe" (
        set "CC=%%~E\tools\win32.x86_64\clang64\bin\gcc.exe"
        set "PATH=%%~E\tools\win32.x86_64\clang64\bin;%PATH%"
      )
    )
  )
)
if "%CC%"=="" (
  echo ERROR: no C compiler found. Install MinGW-w64 gcc or set CC.
  exit /b 1
)
echo Using compiler: %CC%
if not exist build mkdir build

set CFLAGS=-std=c99 -O2 -Wall -Wextra -Ifirmware/include -Isimulation
set FW=firmware/src/dsp.c firmware/src/event.c firmware/src/sampling.c firmware/src/packet.c firmware/src/comms.c firmware/src/storage.c firmware/src/sensor_manager.c firmware/src/telemetry.c firmware/src/energy.c firmware/src/node.c firmware/drivers/analog_drivers.c firmware/drivers/digital_drivers.c firmware/profiles/profile_water.c firmware/profiles/profile_air.c firmware/profiles/profile_soil.c firmware/profiles/profile_registry.c firmware/hal/hal_virtual.c
set SIM=simulation/sensors/virtual_sensors.c simulation/radio/radio_sim.c

"%CC%" %CFLAGS% %FW% %SIM% simulation/main_sim.c -lm -o build/sensor_node.exe || exit /b 1
echo built build\sensor_node.exe
"%CC%" %CFLAGS% %FW% %SIM% tests/test_firmware.c -lm -o build/test_firmware.exe || exit /b 1
echo built build\test_firmware.exe

if /i "%1"=="arm" call "%~dp0firmware\targets\stm32f4\build_arm.bat" || exit /b 1
exit /b 0
