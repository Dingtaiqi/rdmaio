# NetworkDirect Disconnect 合规性修改总结

## 背景

按 [Microsoft NetworkDirect Disconnect 方案](https://learn.microsoft.com/zh-cn/windows-hardware/drivers/network/networkdirect-disconnect-scheme) 要求：

> 调用 `Disconnect` 前，QP 上的所有 DMA 活动必须已完成（包括 `ND_OP_FLAG_SILENT_SUCCESS` 工作请求）。
> 消费者通常只有在收到发布到发起程序队列的所有工作请求的完成通知后，才应当调用 `Disconnect`。

原实现 `CleanupNd` 直接 `Deregister` → `Disconnect`，未 drain CQ，存在合规风险。

---

## 代码修改

### 1. `rdmaio/rdma_transfer.cpp`（核心 DLL）

#### 新增 WR 计数器与发布包装
- `NdContext` 新增：
  - `pendingSends`、`pendingRecvs`、`pendingWrites`
  - `disconnected` 标志
- 新增 `PostSend()` / `PostRecv()` / `PostWrite()`：
  - 成功提交 WR 后递增对应计数器
  - `disconnected=true` 后禁止再发布新 WR
- 新增 `IsSendContext()` / `IsRecvContext()` / `IsWriteContext()`：按 CQ RequestContext 分类完成项
- 新增 `ConsumeCompletion()`：正常路径消费完成时递减计数器，遇到失败完成则置 `disconnected=true`

#### 重写 `CleanupNd()`
释放顺序改为：
1. 设置 `disconnected = true`
2. `DrainCompletionQueue()` + `WaitForAllPendingWork()`：取出并等待所有已发布 WR 完成
3. `IND2Connector::Disconnect()` 并等待 `GetOverlappedResult`
4. 释放 `QP` → `CQ` → `Listener`
5. `MR::Deregister()`（QP 已释放后再注销，避免远程访问未完成）
6. 释放 `Connector` → `Adapter` → 事件句柄

#### 共享内存 silent-write flush
- `MemShareState` 新增 `pendingSilentWrites`
- `rdma_mem_write_from_staging_silent*()` 成功后递增 `pendingSilentWrites`
- `rdma_mem_write_from_staging*()`（非静默）CQ 成功后清零 `pendingSilentWrites`
- `MemCleanup()` / `rdma_mem_stop()` / `rdma_mem_stop_mode(1)` 在断开前调用 `MemFlushSilentWrites()`：
  - 发出 zero-length 非静默 Write
  - 等待其 CQ（QP in-order 语义保证所有前序静默写已完成）

#### 所有正常路径补 `ConsumeCompletion()`
文件传输 `InternalSend` / `InternalRecv`、Bench `InternalBenchSend` / `InternalBenchRecv`、内存监控 `rdma_mem_start` / `MemWriteFromStagingSlot` 等处的 `GetResults` 循环后均调用 `ConsumeCompletion()`，保证计数器不泄漏。

### 2. `rdmaio/rdma_file_transfer.cpp`（CLI 工具）

- 新增 `DrainCompletionQueue()`，在 `Disconnect` 前 drain CQ
- 重写 `CleanupNd()`：drain → Disconnect → 释放 QP/CQ → Deregister MR
- 修复 `RunSender` / `RunReceiver` 的 `done:` 释放顺序：先显式 `CleanupNd(ctx)`，再 `FreeAligned(pBuf)`，最后 `NdCleanup()` / `WSACleanup()`（避免全局清理在所有 ND 对象释放之前调用）

---

## 文档更新

### `rdmaio/API_REFERENCE.md`
- 补充 `rdma_mem_staging*` / `rdma_mem_write_from_staging*` / `rdma_mem_write_from_staging_silent*` / `rdma_mem_stop_mode` 说明
- 修正 `rdma_mem_write` 最大长度为 16 MB
- 新增“NetworkDirect Disconnect 合规性”小节
- 新增零拷贝批量写入示例

### `rdmaio/RDMA_MEMORY_MONITOR.md`
- 移除过时的 doorbell 架构图与通知机制描述
- 更新缓冲区布局（8 slot staging，最大 256 MB）
- 补充 silent-write flush 与 NetworkDirect Disconnect 合规说明
- 移除 GUI/C# 应用层内容，改为引用顶层 README / RustScreen/README

### `README.md`
- 更新共享内存监控描述（去 doorbell）
- 补充零拷贝 API 示例
- 新增“NetworkDirect Disconnect 合规”小节

### `RustScreen/README.md`
- 在“阶段 7：Write 流水线”后新增阶段 7b，记录底层 Disconnect 合规修改及 `rdma_mem_stop_mode(1)` 自动 flush 行为

---

## 验证结果

### 编译
```cmd
D:\rdma> build_all.bat
已成功生成。
    0 个警告
    0 个错误
```

### 文件传输（rdmaio.exe，loopback 192.168.100.2 ↔ 192.168.100.3）
- 发送 100 MB 测试文件
- 发送端输出：`Sender: total time 90.56 ms, average bandwidth 1104.21 MB/s`
- 接收端输出：`Receiver: total time 96.72 ms, average bandwidth 1033.94 MB/s`
- 文件大小一致：104,857,600 字节
- 内容一致：`content match: True`
- 发送端与接收端均正常退出

### 共享内存监控（mem_demo.exe）
- `display` 模式：可正常启动并进入 `rdma_mem_wait()` 轮询
- `writer` 模式：**运行时崩溃，返回码 `0xC00000FD`（STATUS_STACK_OVERFLOW）**
- 该崩溃与 display 是否运行无关，writer 单独启动也会崩溃
- 崩溃位置尚未定位，需要进一步调试

### 网卡枚举
- `list_adapters.exe` 成功识别两个 loopback IP：192.168.100.2 / 192.168.100.3
- `InOrder DMA: yes`，`Loopback: yes`

---

## 已知问题

1. **`mem_demo.exe -write` 崩溃 `0xC00000FD`**
   - 已排除 display 未启动的因素
   - 崩溃发生在 writer 路径，可能与 `rdma_mem_write` 内部或 `rdma_mem_stop` 的清理路径有关
   - 需进一步用调试器或添加日志定位

2. **`file_transfer.c` 示例未在本次修改范围内验证**
   - 仅验证了 `rdmaio.exe`（基于 `rdma_file_transfer.cpp`）

---

## 文件变更

```
README.md                     |  25 +-
rdmaio/API_REFERENCE.md       |  89 ++++-
rdmaio/RDMA_MEMORY_MONITOR.md |  97 +++---
rdmaio/rdma_file_transfer.cpp |  70 +++-
rdmaio/rdma_transfer.cpp      | 765 +++++++++++++++++++++++++++++++-----------
rdmaio/rdma_transfer.h        |  40 ++-
RustScreen/README.md          |   8 +
```

（注：`RustScreen/README.md` 位于 RustScreen 独立仓库，本次改动未在 `rdmaio` git 子仓库中体现。）
