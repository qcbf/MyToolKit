@echo off
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
setlocal enabledelayedexpansion
call "!VCVARS!" >nul 2>&1

echo [BUILD] Compiling resource.rc ...
rc /nologo resource.rc

echo [BUILD] Compiling mytoolkit.c (MSVC, size-optimized) ...

cl /nologo /O1 /Os /GS- /W3 /utf-8 ^
   mytoolkit.c resource.res ^
   /link /OPT:REF /OPT:ICF ^
   user32.lib shell32.lib advapi32.lib ^
   /OUT:mytoolkit.exe

if %errorlevel%==0 (
    echo [OK] mytoolkit.exe built successfully.
    for %%f in (mytoolkit.exe) do echo     Size: %%~zf bytes
) else (
    echo [FAILED]
)

:: Cleanup intermediate files
del /q mytoolkit.obj mytoolkit.res resource.res 2>nul
