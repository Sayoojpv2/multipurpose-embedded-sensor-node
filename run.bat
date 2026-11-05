@echo off
rem ---------------------------------------------------------------------------
rem run.bat - full pipeline: build, unit tests, traces, experiments, analysis,
rem           plots, pipeline tests.  Optional: "run.bat matlab" also runs the
rem           MATLAB filter validation (needs MATLAB on PATH).
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"
call "%~dp0build.bat" || exit /b 1
echo.
echo === C unit tests ===
"%~dp0build\test_firmware.exe" || exit /b 1
echo.
echo === scenario traces ===
python python\generate_data.py || exit /b 1
echo.
echo === experiments ===
python python\run_experiments.py || exit /b 1
python python\analyse_results.py || exit /b 1
python python\plots.py || exit /b 1
echo.
echo === pipeline tests ===
python -m unittest discover -s tests -p "test_*.py" -v || exit /b 1
if /i "%1"=="matlab" (
  matlab -batch "cd matlab; validate_c_output" || exit /b 1
)
echo.
echo Done. Results: experiments\results\results_summary.md
exit /b 0
