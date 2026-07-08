@echo off
setlocal
set IP=192.168.100.2
set SRC=D:\rdma\test_1gb.bin
set DST=D:\rdma\dll_test_out.bin

echo === DLL Direct Transfer Test ===

REM Start receiver
del "%DST%" > nul 2>&1
start /B "" cmd /c "D:\rdma\rdmaio\x64\Release\cancel_test.exe recv %IP% %DST% > D:\rdma\dll_recv.log 2>&1"
ping -n 4 127.0.0.1 > nul

REM Run sender
D:\rdma\rdmaio\x64\Release\cancel_test.exe send %IP% %SRC% > D:\rdma\dll_send.log 2>&1
set RET=%errorlevel%

ping -n 2 127.0.0.1 > nul
taskkill /F /IM cancel_test.exe > nul 2>&1

echo Exit: %RET%
echo --- SENDER ---
type D:\rdma\dll_send.log
echo --- RECEIVER ---
type D:\rdma\dll_recv.log

if %RET%==0 (echo PASS) else (echo FAIL)
