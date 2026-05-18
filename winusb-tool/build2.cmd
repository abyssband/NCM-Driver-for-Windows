@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\Users\abyss\Documents\apple-ncm-windows\winusb-tool
cl /nologo /EHsc /W4 /O2 /std:c++17 peer_probe.cpp /link setupapi.lib winusb.lib /OUT:peer_probe.exe
