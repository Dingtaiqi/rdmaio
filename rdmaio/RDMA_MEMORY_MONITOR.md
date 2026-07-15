# RDMA 共享内存监控 — 设计与实现

## 概述

共享内存监控是 rdmaio 的一个实时内存可视化功能。一端（写入器）通过 **RDMA Write** 直接将数据写入另一端（显示器）的共享缓冲区，显示器实时看到数据变化——**接收端 CPU 在数据到达路径上完全为零介入**。

```
写入器 (发送端)                          显示器 (接收端)
┌─────────────────────┐                 ┌──────────────────────┐
│  应用数据            │                 │  共享缓冲区           │
│        │            │                 │                      │
│        ▼            │   RDMA Write    │  ┌──┬──┬──┬──┬──┬──┐  │
│  staging buffer ────────── 零CPU ────►│  │0 │1 │2 │...│N │  │  │
│                     │                 │  └──┴──┴──┴──┴──┴──┘  │
│                     │                 │       ▲               │
│                     │                 │       │               │
└─────────────────────┘                 │  CQ 完成 → 应用可见   │
                                        └──────────────────────┘
```

## 架构

### 通信协议

内存监控使用以下协议：

1. **连接建立** (Send/Recv):
   - 显示器监听端口，等待写入器连接
   - 连接后显示器将共享缓冲区的 `(remote_addr, remote_token, size)` 发送给写入器

2. **数据传输** (RDMA Write):
   - 写入器调用 `rdma_mem_write()` → 数据复制到 staging buffer → 非静默 RDMA Write
   - 或零拷贝路径：`rdma_mem_staging()` 写入数据 → `rdma_mem_write_from_staging()` / `rdma_mem_write_from_staging_silent()`
   - 显示器端数据直接到达共享缓冲区，**CPU 零参与**

3. **完成可见性**:
   - 非静默写：CQ 完成即表示数据已写入远端共享缓冲区
   - 静默写：最终必须跟一次非静默写（或调用 stop），利用 QP in-order 语义保证前序静默写落地
   - 显示器端可直接读取 `rdma_mem_buffer()` 对应的内存，无需 doorbell

### 缓冲区布局

```
单个 MR (Memory Region)，包含所有控制结构和数据：

┌─────────────────────────────────────────────────────┐
│ MemSetupInfo (remote_addr + token + size)           │
├─────────────────────────────────────────────────────┤
│ MemDoorbellMsg (control recv buffer)                │
├─────────────────────────────────────────────────────┤
│ MemDoorbellMsg (control send buffer — writer only)  │
├─────────────────────────────────────────────────────┤
│ Staging buffers (CHUNK_SIZE × RDMA_STAGING_SLOT_COUNT│
│                 = 16 MB × 8 = 128 MB — writer only) │
├─────────────────────────────────────────────────────┤
│ Shared data buffer (size_bytes 可配置)              │
│ 写入器 RDMA Write 直接写入此区域                    │
└─────────────────────────────────────────────────────┘
```

### 流控

- 非静默写：每次 RDMA Write 等待 CQ 完成，天然流控。
- 静默写（`ND_OP_FLAG_SILENT_SUCCESS`）：批量连续写，不等待 CQ。最终通过一次非静默写或停止接口 flush，利用 QP in-order 语义保证所有前序静默写已完成。

### 取消机制

- `rdma_transfer_cancel()` 设置全局 `g_cancel_flag`
- `rdma_mem_wait()` 的轮询循环检查该标志，检测到后立即返回 -1
- `rdma_mem_stop()` / `rdma_mem_stop_mode()` 先设标志 → 清理资源
- 写入器停止前会自动 flush 所有未完成的静默写，确保 QP 上所有 DMA 活动完成后再 Disconnect
- 避免了 use-after-free 竞态

## NetworkDirect Disconnect 合规性

本实现遵循 [Microsoft NetworkDirect Disconnect 方案](https://learn.microsoft.com/zh-cn/windows-hardware/drivers/network/networkdirect-disconnect-scheme)：

1. **所有 WR 完成后再 Disconnect**：`CleanupNd()` 会 drain CQ 并等待所有已发布 Send/Recv/Write 的完成通知，然后才调用 `IND2Connector::Disconnect`。
2. **静默写 flush**：写入器在 `rdma_mem_stop()` / `rdma_mem_stop_mode(1)` 时，若还有未完成的 `ND_OP_FLAG_SILENT_SUCCESS` Write，会发出一个 zero-length 非静默 Write 并等待其 CQ。根据 QP in-order 语义，该 CQ 到达时所有前序静默写必然已完成。
3. **资源释放顺序**：QP/CQ 在 Disconnect 后释放，MR 在 QP 释放后 `Deregister`，避免远端仍在访问时注销内存。
4. **错误路径**：一旦 CQ 出现失败完成，设置 `disconnected` 标志，禁止再在该连接上发布新 WR。

## API 参考

```c
// 启动共享内存会话
//   mode: 0 = 显示器 (接收端), 1 = 写入器 (发送端)
//   ip/port: 连接参数
//   size_bytes: 共享缓冲区大小 (1 ~ 256MB)
int rdma_mem_start(int mode, const char* ip, unsigned short port, unsigned int size_bytes);

// 写入数据到远程共享缓冲区 (写入器端，兼容路径，内部有复制)
//   offset: 偏移量
//   data: 源数据
//   len: 数据长度 (最大 16 MB)
int rdma_mem_write(unsigned int offset, const void* data, unsigned int len);

// 零拷贝写入路径 (写入器端)
void* rdma_mem_staging(void);
int   rdma_mem_write_from_staging(unsigned int offset, unsigned int len);
int   rdma_mem_write_from_staging_silent(unsigned int offset, unsigned int len);

// 多 slot 零拷贝路径
void* rdma_mem_staging_slot(int slot_index);
int   rdma_mem_write_from_staging_slot(int slot_index, unsigned int offset, unsigned int len);
int   rdma_mem_write_from_staging_silent_slot(int slot_index, unsigned int offset, unsigned int len);

// 获取共享缓冲区指针 (显示器端)
const void* rdma_mem_buffer(void);

// 获取缓冲区大小
unsigned int rdma_mem_size(void);

// 等待数据更新 (显示器端)
//   timeout_ms: 超时 (0=无限, -1=非阻塞)
int rdma_mem_wait(int timeout_ms);

// 获取上次写入信息
void rdma_mem_last_write(unsigned int* out_offset, unsigned int* out_len);

// 停止指定模式 (0=显示器, 1=写入器)
void rdma_mem_stop_mode(int mode);

// 停止所有共享内存会话
void rdma_mem_stop(void);
```

## 性能

| 指标 | 值 |
|---|---|
| 单次写入时延 | ~2-5 μs (RDMA Write) |
| 最大写入大小 | 16 MB (`CHUNK_SIZE`) |
| Staging slot 数 | 8 |
| 缓冲区大小 | 1 ~ 256 MB (可配置) |
| 接收端 CPU (数据路径) | **0%** |

## 代码结构

```
rdma_transfer.h          — C API 声明 (rdma_mem_* 系列)
rdma_transfer.cpp        — 原生实现 (MemShareState 全局状态)
```

GUI / C# 显示器应用层封装见项目根目录 `README.md` 与 `RustScreen/README.md`。
