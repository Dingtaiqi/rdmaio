# RDMA File Transfer over Windows NetworkDirect (NDSPI) — 完整技术记录

## 1. 硬件环境

| 项目 | 详情 |
|---|---|
| 网卡型号 | HP InfiniBand FDR/Ethernet 10Gb/40Gb 2-port 544+FLR-QSFP |
| 芯片 | Mellanox ConnectX-3 Pro (MT04103 VPI) |
| 驱动 | mlx4 (WinOF), 同时 mlx5 驱动已安装（可能有另一张 CX4/CX5） |
| 端口速率 (RoCE) | **40 Gbps** Ethernet × 2 |
| 端口速率 (IB FDR) | **56 Gbps** InfiniBand × 2 |
| 当前模式 | Ethernet / RoCE v2 |
| IP 地址 | 192.168.100.2 / 192.168.100.3 (同一张卡两个口直连) |
| ND Adapter Flags | 0x00010009 (InOrder=1, MultiEngine=1, Loopback=1) |
| MaxTransferLength | 1024 MB |
| InitQDepth / RecvQDepth | 16351 |
| MaxCQDepth | 4194303 |
| 操作系统 | Windows 11 Pro for Workstations 10.0.26200 |
| VS 版本 | Visual Studio 2026 Community v18.0 (PlatformToolset v145) |
| Mellanox SDK | C:\Program Files\Mellanox\MLNX_VPI\IB\SDK\ |
|  | inc\ndv2\ndspi.h 存在 |
|  | lib\user\objWin2019Debug\x64\ (ibal.lib, complib.lib, mlx4u.lib) |
|  | ndspi.lib 未找到（链接用 ndutil.lib 替代） |

---

## 2. 工程结构

```
D:\rdma\
├── rdmaio\
│   ├── rdmaio.slnx              # VS2022 解决方案 (XML)
│   └── rdmaio\
│       ├── rdmaio.vcxproj        # 项目文件 (已修改)
│       └── rdmaio.cpp            # 原始 hello world (已排除编译)
├── rdma_file_transfer.cpp        # 主源代码
├── build_rdmaio.bat              # 编译脚本
├── build_netdirect.bat           # 编译 ndutil.lib 的脚本
├── x64\Release\rdmaio.exe        # 生成的可执行文件
└── NetworkDirect\                # 微软官方 ND 示例 (git clone)
    └── src\
        ├── ndutil\               # ND helper 库
        │   ├── ndspi.h           # ND2 API 头文件
        │   ├── ndsupport.h       # NdStartup/NdCleanup/NdOpenAdapter 等
        │   ├── nddef.h           # ND2_ADAPTER_INFO, ND2_RESULT, ND2_SGE
        │   └── ndstatus.h        # ND_SUCCESS, ND_IO_TIMEOUT 等状态码
        ├── examples\
        │   ├── ndpingpong\       # Send/Recv ping-pong (客户端/服务器连接模式)
        │   ├── ndrpingpong\      # RDMA Write ping-pong (RDMA Write 模式)
        │   └── ndtestutil\       # NdTestBase 封装类 (CQ/MR/QP/Connector)
        ├── x64\Release\
        │   ├── ndutil.lib        # ND helper 静态库
        │   └── ndtestutil.lib    # 测试工具静态库
        └── netdirect.sln         # NetworkDirect 解决方案 (v143 工具集)
```

### .vcxproj 关键修改

```xml
<!-- 源文件 -->
<ClCompile Include="..\rdma_file_transfer.cpp" />
<ClCompile Include="rdmaio.cpp">
  <ExcludedFromBuild>true</ExcludedFromBuild>
</ClCompile>

<!-- 头文件路径 -->
<AdditionalIncludeDirectories>
  D:\rdma\NetworkDirect\src\ndutil;
  C:\Program Files\Mellanox\MLNX_VPI\IB\SDK\inc\ndv2;
  %(AdditionalIncludeDirectories)
</AdditionalIncludeDirectories>

<!-- 编译选项 -->
<AdditionalOptions>/utf-8 %(AdditionalOptions)</AdditionalOptions>
<RuntimeLibrary>MultiThreaded</RuntimeLibrary>   <!-- /MT, 与 ndutil.lib 一致 -->

<!-- 链接 -->
<AdditionalDependencies>ndutil.lib;Ws2_32.lib;%(AdditionalDependencies)</AdditionalDependencies>
<AdditionalLibraryDirectories>
  D:\rdma\NetworkDirect\src\$(Platform)\$(Configuration);
  %(AdditionalLibraryDirectories)
</AdditionalLibraryDirectories>
```

### .slnx 格式

```xml
<Solution>
  <Configurations>
    <Platform Name="x64" />
    <Platform Name="x86" />
  </Configurations>
  <Project Path="rdmaio/rdmaio.vcxproj" />
</Solution>
```

### 编译命令

```cmd
# Step 1: 编译 ndutil.lib (只需一次)
msbuild D:\rdma\NetworkDirect\src\ndutil\ndutil.vcxproj ^
  /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145

# Step 2: 编译 rdmaio
msbuild D:\rdma\rdmaio\rdmaio.slnx ^
  /p:Configuration=Release /p:Platform=x64

# 一键编译脚本: D:\rdma\build_rdmaio.bat
```

---

## 3. NDSPI API 核心模式 (NDv2)

### 3.1 初始化/清理

```cpp
HRESULT NdStartup();          // 加载 ND provider
HRESULT NdCleanup();          // 卸载

HRESULT NdResolveAddress(     // 解析远程地址，返回本地应使用的地址
    const sockaddr* remote, SIZE_T cbRemote,
    sockaddr* local, SIZE_T* pcbLocal);

HRESULT NdOpenAdapter(        // 打开本地 RDMA 适配器
    REFIID iid, const sockaddr* local, SIZE_T cbLocal, void** ppAdapter);
```

### 3.2 对象创建顺序 (ND2)

```
NdOpenAdapter
  → CreateEvent + CreateOverlappedFile
  → CreateCompletionQueue(IID_IND2CompletionQueue, hOvFile, depth, 0, 0, &pCq)
  → CreateConnector(IID_IND2Connector, hOvFile, &pConnector)
  → CreateQueuePair(IID_IND2QueuePair, pCq, pCq, NULL, recvDepth, initDepth, nSge, nSge, 0, &pQp)
  → CreateMemoryRegion(IID_IND2MemoryRegion, hOvFile, &pMr)
  → pMr->Register(pBuf, size, flags, &ov)   // flags: ND_MR_FLAG_ALLOW_LOCAL_WRITE
```

### 3.3 清理顺序

```cpp
// 严格顺序：先释放 ND 对象，再 NdCleanup
ctx.pMr->Deregister(&ov)                    // 1. 注销内存
ctx.pConnector->Disconnect(&ov)             // 2. 断开连接
ctx.pQp->Release()                          // 3. 释放 QP
ctx.pCq->Release()                          // 4. 释放 CQ
ctx.pConnector->Release()                   // 5. 释放 Connector
ctx.pListener->Release()                    // 6. 释放 Listener
ctx.pMr->Release()                          // 7. 释放 MR
CloseHandle(ctx.hOvFile)                    // 8. 关闭 Overlapped 文件
ctx.pAdapter->Release()                     // 9. 释放 Adapter
CloseHandle(ctx.ov.hEvent)                  // 10. 关闭事件
NdCleanup()                                 // 11. 必须在所有对象 Release 之后！
WSACleanup()
```

> **致命错误**：如果在 `Release()` 所有 ND 对象之前调用 `NdCleanup()`，会导致
> Provider 被卸载后 Adapter/QP 等指针变成悬空指针，析构时触发 ACCESS_VIOLATION (0xC0000005)。

### 3.4 客户端连接 (主动方)

```cpp
connector->Bind(&localAddr, sizeof(localAddr));
connector->Connect(pQp, &remoteAddr, sizeof(remoteAddr), 0, 0, NULL, 0, &ov);
// 连接建立后，接收端 Accept 之前，应投递 receive 避免 RNR 超时
ctx.pQp->Receive(...);  // 必须在 CompleteConnect 之前！
connector->CompleteConnect(&ov);
```

### 3.5 服务端监听 (被动方)

```cpp
adapter->CreateListener(IID_IND2Listener, hOvFile, &pListener);
listener->Bind(&localAddr, sizeof(localAddr));
listener->Listen(backlog);
// 接收端在 Accept 之前就应该投递 receive
ctx.pQp->Receive(...);  // 在 Accept 之前！
listener->GetConnectionRequest(pConnector, &ov);
connector->Accept(pQp, 0, 0, NULL, 0, &ov);
```

### 3.6 CQ 轮询

```cpp
ND2_RESULT result;
while (pCq->GetResults(&result, 1) == 0) {}  // 忙等轮询，返回 0 表示无完成
// result.Status == ND_SUCCESS 表示成功
// result.RequestContext 用于区分不同的操作
// result.BytesTransferred 只对 Receive 有效，Send 操作不可用！
```

### 3.7 内存注册

```cpp
// 必须使用 _aligned_malloc(size, 4096)，不能用 new/malloc
void* pBuf = _aligned_malloc(size, 4096);
pMr->Register(pBuf, size, ND_MR_FLAG_ALLOW_LOCAL_WRITE, &ov);

ND2_SGE sge = {};
sge.Buffer = pBuf;                 // 必须指向已注册内存！
sge.BufferLength = size;
sge.MemoryRegionToken = pMr->GetLocalToken();
```

> **致命错误**：SGE 中的 Buffer 必须指向已注册的内存区域。
> 如果指向栈变量或未注册的堆内存，会导致 ACCESS_VIOLATION (0xC0000005) 或 DMA 失败。

### 3.8 Send/Receive

```cpp
// 发送 (任意方向可用)
pQp->Send(requestContext, &sge, nSge, flags);

// 接收 (任意方向可用)
pQp->Receive(requestContext, &sge, nSge);
```

### 3.9 RDMA Write (单向，旁路远端 CPU)

```cpp
// 发送端直接写入远端已注册内存，接收端 CPU 不参与
pQp->Write(requestContext, &sge, nSge,
    remoteAddress, remoteToken, flags);

// flags 可用 ND_OP_FLAG_SILENT_SUCCESS——成功时不生成 CQ 条目，
// 消除轮询开销，仅最后一个 Write 用 flags=0 产生完成通知。
```

### 3.10 Overlapped 等待

```cpp
hr = pConnector->Connect(pQp, ..., &ov);
if (hr == ND_PENDING) {
    hr = pConnector->GetOverlappedResult(&ov, TRUE);  // 阻塞等待
}
```

---

## 4. 文件传输程序设计

### 4.1 命令行

```cmd
rdmaio.exe -send -ip <ip> -file <path>     # 发送端
rdmaio.exe -recv -ip <ip>  -o <path>       # 接收端
rdmaio.exe -bench -send -ip <ip>           # 纯内存 RDMA Write 测速
rdmaio.exe -bench -recv -ip <ip>           # 测速接收端
```

### 4.2 传输协议

```
Phase 1: 元数据 (第一步，固定大小)
  ┌──────────────────────────────────────────┐
  │ FileMeta: file_size(8B) + filename_len(4B) + filename[256] │
  │ 总计 sizeof(FileMeta) = 268 字节          │
  └──────────────────────────────────────────┘

Phase 2: 数据块 (1 MiB 分块，循环发送)
  每个块: ReadFile → Send → CQ Poll → 进度打印

Phase 3: 终止信号 (接收端磁盘写入失败时触发)
  TerminateCmd: cmd = 0xDEADBEEF
```

### 4.3 接收端多缓冲环（关键设计）

**问题**：Send/Recv 模型中，发送端在 Receive 未投递时发送会导致 RNR 超时 (0xC00000B5)。

**方案演进**：

| 阶段 | 缓冲数 | 最大传输量 | 原因 |
|---|---|---|---|
| v1 | 1 (串行) | 第 1 块就超时 | 元数据收完才投递第一个块 |
| v2 | 预投递 1 个块 | 1 块 OK | 和元数据同时投递 |
| v3 | 2 缓冲双缓冲 | ~3 块 (12MB) | repost 跟不上 sender |
| v4 | 4 缓冲环 | ~6 块 (24MB) | 还不够深 |
| v5 | 8 缓冲环 | ~10 块 | 10MB 文件 OK，1GB 不够 |
| v6 | 15 缓冲 (QP_DEPTH-1) | ~62 块 (248MB) | 写入变慢时耗尽 |
| **v7** | **63 缓冲 (QP_DEPTH-1, QP=64)** | **1GB 完整** | 配合 /MT 和无 FILE_FLAG_WRITE_THROUGH |

**最终方案 (QP_DEPTH=128, 127 缓冲环)**：
- 分配 `ALIGNMENT + CHUNK_SIZE * 127 + 8192` = ~509 MB 缓冲区
- 预投递 1 个 meta receive + 127 个 chunk receive
- 主循环：收割 CQ → 立即 repost 该缓冲区 → 写盘 → ringIdx++
- 始终保持接收队列满载，消除 RNR 窗口

### 4.4 Send 的 BytesTransferred 不可用

```cpp
// 错误: Send 完成后 result.BytesTransferred 可能为 0
bytesSent += result.BytesTransferred;  // ❌

// 正确: 自己记录发送的字节数
bytesSent += toRead;  // ✓
```

---

## 5. RDMA Write 基准测试结果

### 5.1 吞吐量

| 方法 | 数据量 | 吞吐量 | 备注 |
|---|---|---|---|
| 4MB × 128 分批 CQ 轮询 | 512 MB | 19.20 Gbps | 基线 |
| 16MB × 32 大批量 | 512 MB | 18.87 Gbps | 无提升 |
| SILENT_SUCCESS 零轮询 | 512 MB | 18.80 Gbps | 瓶颈不在轮询 |
| **单次 512MB 巨量 Write** | 512 MB | **18.14 Gbps** | 方法无关 |
| 文件传输 Send/Recv | 1 GB | ~2.9 Gbps | 磁盘瓶颈 |

**结论**：ConnectX-3 Pro RoCE loopback 硬件上限 ≈ 19 Gbps (47.5% of 40G 线速)。

### 5.2 同机 loopback 性能限制

- 收发两端在同一张卡两个端口之间传输
- 数据路径: 内存 → NIC DMA → 内部交换 → NIC DMA → 内存
- 不经过物理线缆，NIC 内部处理
- ConnectX-3 Pro 这一代在 Windows RoCE loopback 下效率天然偏低

---

## 6. 踩过的坑

### 6.1 C++ goto 跳过变量初始化 (C2362)

在 `/permissive-` 下，`goto cleanup` 跳过带初始化的局部变量会报错。
**解决**: 用 `do { ... } while(0)` + `break` 替代 `goto`。

### 6.2 CRT 链接不匹配 (LNK2038)

ndutil.lib 是 `/MT` 编译的，项目默认 `/MD`。
**解决**: 统一改为 `<RuntimeLibrary>MultiThreaded</RuntimeLibrary>`。

### 6.3 中文注释编码 (C4819)

源文件含中文注释在 GBK 代码页下编译报错。
**解决**: 添加 `<AdditionalOptions>/utf-8</AdditionalOptions>`。

### 6.4 ndspi.lib 不存在

本地 Mellanox SDK 未提供 `ndspi.lib`。
**解决**: 微软示例实际链接的是 `ndutil.lib`（内部加载 provider DLL），用它即可。

### 6.5 NetworkDirect 示例工具集不匹配 (MSB8020)

原示例用 v143 工具集，用户 VS 是 v145 (v18.0)。
**解决**: 加 `/p:PlatformToolset=v145`。

### 6.6 ndadapterinfo / ndcat 需要 MFC (MSB8041)

这两个示例依赖 MFC，用户未安装。
**解决**: 跳过这两个项目，ndutil.lib 和核心示例不依赖 MFC。

### 6.7 编译产物被占用 (LNK1104)

上次运行的 rdmaio.exe 未退出，链接器无法覆盖。
**解决**: `taskkill /F /IM rdmaio.exe`。

---

## 7. 关键状态码速查

| HRESULT | 名称 | 含义 |
|---|---|---|
| 0x00000000 | ND_SUCCESS | 成功 |
| 0x00000103 | ND_PENDING | 异步操作未完成，需 GetOverlappedResult |
| 0xC00000B5 | ND_IO_TIMEOUT | RNR 超时（接收端未投递 receive） |
| 0xC0000120 | ND_CANCELED | 操作被取消（QP Flush/连接断开） |
| 0xC0000236 | ND_CONNECTION_REFUSED | 连接被拒绝（无监听端） |
| 0xC0000005 | ACCESS_VIOLATION | 内存访问违例（SGE 指向未注册内存等） |

---

## 8. Ib 模式备忘

- 当前运行在 RoCE/Ethernet 模式 (40 Gbps)
- 系统已安装 IPoIB 适配器（IB 模式接口已就绪）
- 切到 IB 模式需要: OpenSM (未安装) + 端口模式切换 + IB 寻址
- IB FDR 模式端口速率: 56 Gbps
- 之前测的 28 Gbps 是在 IB 模式下用 ibverbs 工具测得
