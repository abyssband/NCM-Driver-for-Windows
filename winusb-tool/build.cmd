@echo off
setlocal

REM Locate vcvars64.bat via vswhere
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set VSPATH=%%i
if "%VSPATH%"=="" (
    echo VS not found
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo vcvars64 failed
    exit /b 1
)

cd /d "%~dp0"
cl /nologo /EHsc /W4 /O2 /std:c++17 peer_probe.cpp /link setupapi.lib winusb.lib /OUT:peer_probe.exe
exit /b %errorlevel%
