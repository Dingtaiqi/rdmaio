# rdmaio

Windows NetworkDirect (NDSPI) RDMA file transfer & DLL for third-party apps.

## Build

```cmd
build_all.bat
```

Outputs:
- `rdma_transfer.dll` / `.lib` — reusable RDMA transfer library  
- `rdmaio.exe` — standalone CLI tool

## DLL API

```c
#include "rdmaio/rdma_transfer.h"
#pragma comment(lib, "rdma_transfer.lib")

// One-time init (call before any transfer)
rdma_transfer_init();

// File transfer
rdma_send_file("192.168.100.2", 54321, L"D:\\data.bin");
rdma_recv_file("192.168.100.2", 54321, L"C:\\received.bin");

// Pure RDMA Write benchmark (Gbps)
rdma_bench(0, "192.168.100.2", 54321, 512); // recv
rdma_bench(1, "192.168.100.2", 54321, 512); // send

// Progress callback
rdma_set_progress_callback(my_callback, NULL);

rdma_transfer_cleanup();
```

## CLI Usage

```cmd
rdmaio.exe -send -ip 192.168.100.2 -file input.bin
rdmaio.exe -recv -ip 192.168.100.2 -o output.bin
rdmaio.exe -bench -send -ip 192.168.100.2
rdmaio.exe -bench -recv -ip 192.168.100.2
```
