@echo off
call "F:\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (
    echo VCVARS_FAILED
    exit /b 1
)
echo VCVARS_OK
echo PATH=%PATH%
"F:\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" D:\rdma\rdmaio\rdmaio.slnx /p:Configuration=Release /p:Platform=x64 /m
exit /b %errorlevel%
