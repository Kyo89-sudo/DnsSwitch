@echo off
rem Build rapida con MSVC: aprire "x64 Native Tools Command Prompt for VS" e lanciare build.bat
setlocal
if not exist build mkdir build
rc /nologo /fo build\app.res res\app.rc || goto :err
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /W4 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   src\main.cpp build\app.res /Fo:build\ /Fe:build\DnsSwitch.exe ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:NO ws2_32.lib iphlpapi.lib comctl32.lib advapi32.lib shell32.lib uxtheme.lib || goto :err
echo.
echo OK: build\DnsSwitch.exe
exit /b 0
:err
echo.
echo Build fallita.
exit /b 1
