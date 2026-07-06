@echo off
call "F:\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (
    echo VCVARS_FAILED
    exit /b 1
)
"F:\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" D:\rdma\NetworkDirect\src\netdirect.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /m
exit /b %errorlevel%
