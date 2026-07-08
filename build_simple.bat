@echo off
call "F:\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 > nul 2>&1
cl.exe /nologo /O2 /MD /utf-8 /I D:\rdma\rdmaio ^
  D:\rdma\rdmaio\examples\simple_send.c ^
  /Fe:D:\rdma\rdmaio\x64\Release\simple_send.exe ^
  /link D:\rdma\rdmaio\x64\Release\rdma_transfer.lib Ws2_32.lib
if %errorlevel%==0 (echo COMPILE_OK) else (echo COMPILE_FAILED)
