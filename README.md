# rdmaio

Windows NetworkDirect (NDSPI) RDMA 文件传输 + 共享内存监控库与 GUI 工具。

基于 **RDMA Write + Credit 流控** 架构，以极低的 CPU 占用实现高性能数据传输。

## 架构

```
旧架构 (RDMA Send/Recv):          新架构 (RDMA Write):
                                  │
发送端: Read → Send → 等CQ        │  发送端: Read → RDMA Write(零CPU) → Send(doorbell)
接收端: 等Recv CQ → Write → Re-Post │  接收端: 等doorbell → Write → Send(credit)
      每 chunk CPU 被中断         │        数据到达完全零 CPU
```

### 核心功能

| 功能 | 传输方式 | CPU 占用 |
|---|---|---|
| 文件传输 | RDMA Write + Credit 流控 | 低 (仅磁盘 IO) |
| 纯内存压测 | RDMA Write | 极低 (~18 Gbps, 11-24% CPU) |
| **共享内存监控** | **RDMA Write + Doorbell** | **接收端数据路径 0%** |

### 共享内存监控 (Memory Monitor)

接收端开一块共享缓冲区，写入端通过 RDMA Write 直接写入，接收端实时查看 Hex Dump。

**数据到达接收端 CPU 完全零参与**——适配器硬件直接将数据 DMA 到缓冲区。

详见 [RDMA_MEMORY_MONITOR.md](rdmaio/RDMA_MEMORY_MONITOR.md)。

## 快速开始

### 构建

```cmd
build_all.bat
```

输出:
- `rdmaio/x64/Release/rdma_transfer.dll` — RDMA 传输动态库
- `rdmaio/x64/Release/rdma_transfer.lib` — 导入库
- `rdmaio/x64/Release/rdmaio.exe` — 命令行工具

### 环境要求

- Windows 10/11 x64
- RDMA 网卡 (Mellanox/Chelsio/Intel) 及驱动
- Visual Studio 2022+ (构建用)
- .NET 8.0 (GUI 用)

## 使用方式

### 命令行 — 文件传输

```cmd
:: 接收端 (先启动)
rdmaio.exe -recv -ip 192.168.100.2 -o received.bin

:: 发送端
rdmaio.exe -send -ip 192.168.100.2 -file data.bin
```

### 命令行 — 纯内存压测

```cmd
:: 接收端
rdmaio.exe -bench -recv -ip 192.168.100.2 -s 1024

:: 发送端
rdmaio.exe -bench -send -ip 192.168.100.2 -s 1024
```

### 命令行 — 共享内存演示

```cmd
:: 显示器 (监听)
mem_demo.exe -display 192.168.100.2 54323 65536

:: 写入器
mem_demo.exe -write 192.168.100.2 54323 65536
```

### GUI 工具

双击 `启动GUI.bat`。功能:
- 文件发送/接收
- RDMA Write 性能测试
- **内存监控** (显示器/写入器 + 实时 Hex Dump)

## API (C)

```c
#include "rdmaio/rdma_transfer.h"

// 初始化
rdma_transfer_init();

// 文件传输
rdma_send_file("192.168.100.2", 54321, L"D:\\data.bin");
rdma_recv_file("192.168.100.2", 54321, L"C:\\received.bin");

// 纯 RDMA Write 压测
rdma_bench(1, "192.168.100.2", 54321, 1024); // sender
rdma_bench(0, "192.168.100.2", 54321, 1024); // receiver

// 共享内存监控
rdma_mem_start(1, "192.168.100.2", 54323, 65536); // writer
rdma_mem_write(0, data, len);
rdma_mem_start(0, "192.168.100.2", 54323, 65536); // display
rdma_mem_wait(1000);

// 清理
rdma_transfer_cleanup();
```

完整 API 参考见 [API_REFERENCE.md](rdmaio/API_REFERENCE.md)。

## 项目结构

```
├── README.md
├── rdmaio/                      ← C/C++ DLL + CLI
│   ├── rdma_transfer.h          — 公开 C API
│   ├── rdma_transfer.cpp        — DLL 实现 (文件传输 + 内存监控)
│   ├── rdma_file_transfer.cpp   — 命令行工具
│   ├── API_REFERENCE.md         — API 参考
│   ├── RDMA_MEMORY_MONITOR.md   — 内存监控设计文档
│   └── examples/                — 示例程序
│       ├── file_transfer.c
│       ├── mem_demo.c           — 共享内存 CLI 演示
│       └── ...
├── GUI/                         ← WinUI 3 GUI
│   ├── GUI/
│   │   ├── MainWindow.xaml      — Hex Dump + 控制 UI
│   │   ├── RdmaNative.cs        — P/Invoke
│   │   ├── RdmaMemoryShare.cs   — 内存监控 C# 封装
│   │   └── ...
│   └── ...
└── 启动GUI.bat                  — GUI 启动脚本
```

## 性能

| 场景 | 带宽 | CPU (系统) |
|---|---|---|
| 纯 RDMA Write 压测 (1GB) | **~2,300 MB/s (18.4 Gbps)** | **11-24%** |
| 文件传输 (1GB, NVMe) | ~200 MB/s (瓶颈: 磁盘) | 主要消耗在磁盘 IO |
| 内存监控 (单次写入) | ~2-5 μs 延迟 | 接收端 0% |

## License

MIT
