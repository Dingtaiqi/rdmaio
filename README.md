# rdmaio

Windows NetworkDirect (NDSPI) RDMA file transfer and benchmark tool over RoCE v2 / InfiniBand.

## Hardware

- Mellanox ConnectX-3 Pro (HP 544+FLR-QSFP, 40G RoCE / 56G IB FDR)
- Two-port loopback: 192.168.100.2 ↔ 192.168.100.3
- Visual Studio 2026 Community (v145 toolset)

## Build

```cmd
# Step 1 — build ndutil.lib (once)
msbuild D:\rdma\NetworkDirect\src\netdirect.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145

# Step 2 — build rdmaio
msbuild D:\rdma\rdmaio\rdmaio.slnx /p:Configuration=Release /p:Platform=x64
```

## Usage

### File transfer (Send/Recv)

```cmd
# Receiver
rdmaio.exe -recv -ip 192.168.100.2 -o output.bin

# Sender
rdmaio.exe -send -ip 192.168.100.2 -file input.bin
```

### RDMA Write benchmark (pure memory, no disk I/O)

```cmd
# Receiver
rdmaio.exe -bench -recv -ip 192.168.100.2

# Sender
rdmaio.exe -bench -send -ip 192.168.100.2
```

## Performance (ConnectX-3 Pro, RoCE 40G loopback)

| Mode | Throughput |
|---|---|
| File transfer (1 GB) | ~360 MB/s (disk-limited) |
| RDMA Write (512 MB) | ~2.35 GB/s (**18.8 Gbps**) |

## Tech Notes

See [RDMA_PROJECT_TECH_NOTES.md](RDMA_PROJECT_TECH_NOTES.md) for complete technical documentation.
