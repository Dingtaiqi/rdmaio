@echo off
call "F:\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (echo VCVARS failed & exit /b 1)

:: Build ndutil.lib first (if not already built)
if not exist "D:\rdma\NetworkDirect\src\x64\Release\ndutil.lib" (
    echo Building ndutil.lib...
    "F:\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" ^
        D:\rdma\NetworkDirect\src\ndutil\ndutil.vcxproj ^
        /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145
)

:: Build rdmaio solution (DLL + EXE)
echo Building rdmaio...
"F:\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" ^
    D:\rdma\rdmaio\rdmaio.slnx ^
    /p:Configuration=Release /p:Platform=x64 /m

if %errorlevel%==0 (
    echo.
    echo === Build outputs ===
    dir D:\rdma\rdmaio\x64\Release\rdma_transfer.dll  2>nul
    dir D:\rdma\rdmaio\x64\Release\rdma_transfer.lib  2>nul
    dir D:\rdma\rdmaio\x64\Release\rdmaio.exe         2>nul
)
exit /b %errorlevel%
