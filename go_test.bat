@echo off
setlocal
set EXE=D:\rdma\rdmaio\x64\Release\simple_send.exe
set SRC=D:\rdma\test_1gb.bin
set DST=D:\rdma\go_out.bin
del /q "%DST%" > nul 2>&1
taskkill /F /IM simple_send.exe > nul 2>&1
ping -n 8 127.0.0.1 > nul
echo %time% RECV
start "R" cmd /c "%EXE% recv 192.168.100.2 %DST% > D:\rdma\go_recv.log 2>&1"
ping -n 8 127.0.0.1 > nul
echo %time% SEND
cmd /c "%EXE% send 192.168.100.2 %SRC% > D:\rdma\go_send.log 2>&1"
ping -n 4 127.0.0.1 > nul
taskkill /F /IM simple_send.exe > nul 2>&1
echo --- SEND --- & type D:\rdma\go_send.log
echo --- RECV --- & type D:\rdma\go_recv.log
