@echo off
REM Build all examples against rdma_transfer.dll
REM Requires: Visual Studio 2022+, rdma_transfer.dll built first (run build_all.bat)
setlocal

call "F:\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (echo VCVARS failed & exit /b 1)

set DLLDIR=D:\rdma\rdmaio\x64\Release
set OUTDIR=%DLLDIR%
set INCLUDES=/I D:\rdma\rdmaio
set LIBS=%DLLDIR%\rdma_transfer.lib Ws2_32.lib

echo.
echo === Building list_adapters.c ===
cl.exe /nologo /O2 /MD /utf-8 %INCLUDES% ^
  D:\rdma\rdmaio\examples\list_adapters.c ^
  /Fe:%OUTDIR%\list_adapters.exe ^
  /link %LIBS%
if errorlevel 1 (echo FAILED & exit /b 1)

echo === Building file_transfer.c ===
cl.exe /nologo /O2 /MD /utf-8 %INCLUDES% ^
  D:\rdma\rdmaio\examples\file_transfer.c ^
  /Fe:%OUTDIR%\file_transfer.exe ^
  /link %LIBS%
if errorlevel 1 (echo FAILED & exit /b 1)

echo.
echo === Build OK ===
echo   %OUTDIR%\list_adapters.exe
echo   %OUTDIR%\file_transfer.exe
