@echo off
rem Windows counterpart of run.sh (scripts/run_windows.py): builds the port through MSYS2 when
rem needed and starts the game. Usage: run.bat [--game-dir DIR] [bb-probe options...]
rem MSYS2 is expected in C:\msys64 (set BB_MSYS2 otherwise); see README "Windows".
setlocal
if not defined BB_MSYS2 set "BB_MSYS2=C:\msys64"
set "BB_PYTHON=%BB_MSYS2%\clang64\bin\python.exe"
if not exist "%BB_PYTHON%" (
    echo MSYS2 Python not found at %BB_PYTHON%. Install MSYS2 and the packages listed in README.md, or set BB_MSYS2.
    exit /b 1
)
"%BB_PYTHON%" "%~dp0scripts\run_windows.py" %*
exit /b %ERRORLEVEL%
