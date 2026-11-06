@echo off
rem ---------------------------------------------------------------------------
rem run_renode.bat - cross-compile and run the firmware on Renode's emulated
rem STM32F4 Discovery (Cortex-M4). Sensors and radio remain VIRTUAL.
rem Renode: set RENODE=path\to\renode.exe, or unzip the portable release to
rem %USERPROFILE%\tools\renode\  (https://github.com/renode/renode/releases)
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"
rem The ARM image embeds a 3 h water trace; it is generated, not stored in git.
call "%~dp0build.bat" || exit /b 1
python python\generate_data.py --emit-c-header firmware\targets\stm32f4\trace_data.h --profile water --scenario sudden --hours 3 --dt 5 || exit /b 1
call "%~dp0firmware\targets\stm32f4\build_arm.bat" || exit /b 1
if "%RENODE%"=="" for /d %%D in ("%USERPROFILE%\tools\renode\renode_*") do if exist "%%~D\renode.exe" set "RENODE=%%~D\renode.exe"
if "%RENODE%"=="" (echo ERROR: renode.exe not found - set RENODE & exit /b 1)
set "ROOT=%~dp0"
set "ROOT=%ROOT:\=/%"
if exist build\renode_uart.log del build\renode_uart.log
"%RENODE%" --disable-gui --console -e "$elf=@%ROOT%build/sensor_node_stm32f4.elf; $uart_log=@%ROOT%build/renode_uart.log; include @%ROOT%firmware/targets/stm32f4/renode/sensor_node.resc; emulation RunFor \"00:00:20\"; sysbus.cpu ExecutedInstructions; quit" > build\renode_console.log 2>&1
echo ---- USART2 output (build\renode_uart.log) ----
type build\renode_uart.log
findstr /r "^0x" build\renode_console.log
