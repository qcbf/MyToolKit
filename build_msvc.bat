@echo off
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
setlocal enabledelayedexpansion
call "!VCVARS!" >nul 2>&1

echo [BUILD] Compiling resource.rc ...
rc /nologo resource.rc

echo [BUILD] Compiling capsremap.c (MSVC, size-optimized) ...

cl /nologo /O1 /Os /GS- /W3 /utf-8 ^
   capsremap.c resource.res ^
   /link /OPT:REF /OPT:ICF ^
   user32.lib shell32.lib advapi32.lib ^
   /OUT:capsremap.exe

if %errorlevel%==0 (
    echo [OK] capsremap.exe built successfully.
    for %%f in (capsremap.exe) do echo     Size: %%~zf bytes
) else (
    echo [FAILED]
)

:: Cleanup intermediate files
del /q capsremap.obj capsremap.res resource.res 2>nul
