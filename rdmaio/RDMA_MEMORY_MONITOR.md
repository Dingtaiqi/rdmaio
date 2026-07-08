# RDMA 共享内存监控 — 设计与实现

## 概述

共享内存监控是 rdmaio 的一个实时内存可视化功能。一端（写入器）通过 **RDMA Write** 直接将数据写入另一端（显示器）的共享缓冲区，显示器实时看到数据变化——**接收端 CPU 在数据到达路径上完全为零介入**。

```
写入器 (发送端)                          显示器 (接收端)
┌─────────────────────┐                 ┌──────────────────────┐
│  应用数据            │                 │  共享缓冲区 (ring)    │
│        │            │                 │                      │
│        ▼            │   RDMA Write    │  ┌──┬──┬──┬──┬──┬──┐  │
│  staging buffer ────────── 零CPU ────►│  │0 │1 │2 │...│N │  │  │
│        │            │                 │  └──┴──┴──┴──┴──┴──┘  │
│        ▼            │   Send(doorbell)│       ▲               │
│  doorbell ──────────────►              │       │               │
└─────────────────────┘                 │  CQ 完成 → 应用可见   │
                                        └──────────────────────┘
```

## 架构

### 通信协议

内存监控使用以下协议：

1. **连接建立** (Send/Recv):
   - 显示器监听端口，等待写入器连接
   - 连接后显示器将共享缓冲区的 `(remote_addr, remote_token, size)` 发送给写入器

2. **数据传输** (RDMA Write + Doorbell):
   - 写入器调用 `rdma_mem_write()` → 数据复制到 staging buffer → RDMA Write → Send(doorbell)
   - 显示器端数据直接到达共享缓冲区，**CPU 零参与**
   - Doorbell 通知显示器"有新数据"

3. **通知机制** (Doorbell):
   - Doorbell 包含 `(offset, length)` 告诉显示器写入的位置和大小
   - 显示器收到 doorbell → 更新 Hex Dump 显示

### 缓冲区布局

```
单个 MR (Memory Region)，包含所有控制结构和数据：

┌─────────────────────────────────────────────────────┐
│ MemSetupInfo (remote_addr + token + size)           │
├─────────────────────────────────────────────────────┤
│ MemDoorbellMsg (doorbell recv buffer)               │
├─────────────────────────────────────────────────────┤
│ MemDoorbellMsg (doorbell send buffer — writer only) │
├─────────────────────────────────────────────────────┤
│ Staging buffer (CHUNK_SIZE = 4MB — writer only)     │
├─────────────────────────────────────────────────────┤
│ Shared data buffer (size_bytes 可配置)              │
│ 写入器 RDMA Write 直接写入此区域                    │
└─────────────────────────────────────────────────────┘
```

### 流控

每个 doorbell 发送后，发送端等 CQ 完成才继续。显示器端不主动发 credit——因为内存监控场景数据量小，不需要基于信用的流控。

### 取消机制

- `rdma_transfer_cancel()` 设置全局 `g_cancel_flag`
- `rdma_mem_wait()` 的轮询循环检查该标志，检测到后立即返回 -1
- `rdma_mem_stop()` 先设标志 → Sleep(10ms) 等后台线程退出 → 清理资源
- 避免了 use-after-free 竞态

## API 参考

```c
// 启动共享内存会话
//   mode: 0 = 显示器 (接收端), 1 = 写入器 (发送端)
//   ip/port: 连接参数
//   size_bytes: 共享缓冲区大小 (1 ~ 64MB)
int rdma_mem_start(int mode, const char* ip, unsigned short port, unsigned int size_bytes);

// 写入数据到远程共享缓冲区 (写入器端)
//   offset: 偏移量
//   data: 源数据
//   len: 数据长度 (最大 CHUNK_SIZE = 4MB)
int rdma_mem_write(unsigned int offset, const void* data, unsigned int len);

// 获取共享缓冲区指针 (显示器端)
const void* rdma_mem_buffer(void);

// 获取缓冲区大小
unsigned int rdma_mem_size(void);

// 等待下一个 doorbell (显示器端)
//   timeout_ms: 超时 (0=无限, -1=非阻塞)
//   返回: 1=有新数据, 0=超时, -1=错误
int rdma_mem_wait(int timeout_ms);

// 获取上次写入信息
void rdma_mem_last_write(unsigned int* out_offset, unsigned int* out_len);

// 停止共享内存会话
void rdma_mem_stop(void);
```

## 性能

| 指标 | 值 |
|---|---|
| 单次写入时延 | ~2-5 μs (RDMA Write + doorbell) |
| 最大写入大小 | 4 MB (CHUNK_SIZE) |
| 缓冲区大小 | 1 ~ 64 MB (可配置) |
| 接收端 CPU (数据路径) | **0%** |

## 代码结构

```
rdma_transfer.h          — C API 声明 (rdma_mem_* 系列)
rdma_transfer.cpp        — 原生实现 (MemShareState 全局状态)
GUI/
├── RdmaNative.cs        — P/Invoke 声明
├── RdmaMemoryShare.cs   — C# 封装 (Rust 风格的 Result/async 包装)
├── MainWindow.xaml      — Hex Dump 显示 + 控制 UI
└── MainWindow.xaml.cs   — 显示端轮询循环 + 写入器控制
```

## C# 封装说明

`RdmaMemoryShare.cs` 提供:
- `StartAsync(isWriter, ip, port, sizeBytes)` — 启动会话
- `WriteAsync(offset, data, index, count)` — 写入数据 (写入器)
- `WriteStringAsync(offset, text)` — 写入字符串
- `WaitForUpdateAsync(timeoutMs, ct)` — 等待更新 (显示器)
- `GetLastWriteInfo()` — 获取上次写入信息
- `ReadBuffer(offset, count)` — 读取缓冲区快照
- `Stop()` — 停止会话

### 线程安全

- 原生 `rdma_mem_wait` 在后台线程池运行
- 取消通过 `CancellationToken` + 原生 cancel flag 双重机制
- Hex dump 快照在访问原生缓冲区之前复制到托管数组
- 所有 UI 更新通过 `DispatcherQueue.TryEnqueue`
