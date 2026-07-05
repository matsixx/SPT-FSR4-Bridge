@echo off
REM Builds FSR4Native.dll (native FSR4 D3D11->D3D12 bridge) with MSVC.
REM Generic D3D interop — no VR / no Unity dependency.
setlocal

set SCRIPT_DIR=%~dp0
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

if not exist %VCVARS% (
    echo [build] vcvars64.bat not found. Adjust VCVARS in build.bat to your VS install.
    exit /b 1
)

call %VCVARS% >nul
if errorlevel 1 ( echo [build] failed to init MSVC env. & exit /b 1 )

pushd "%SCRIPT_DIR%"
if not exist build mkdir build

echo [build] compiling FSR4Native.cpp ...
REM /MT = static CRT so the DLL needs no VC++ redist on the user's machine.
cl /nologo /LD /O2 /EHsc /MT /std:c++17 /DNDEBUG ^
   /Fo"build\\" /Fe"build\FSR4Native.dll" ^
   FSR4Native.cpp ^
   /link d3d11.lib d3d12.lib dxgi.lib dxguid.lib ^
   /IMPLIB:"build\FSR4Native.lib"

set RESULT=%errorlevel%
popd

if %RESULT% neq 0 ( echo [build] FAILED (exit %RESULT%) & exit /b %RESULT% )
echo [build] OK -^> %SCRIPT_DIR%build\FSR4Native.dll
endlocal
