@echo off
call "F:\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 > nul 2>&1
"F:\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\rdma\rdmaio\rdma_transfer.vcxproj" /t:Rebuild /p:Configuration=Release /p:Platform=x64
