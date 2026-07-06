# rdma_transfer.dll API Reference

## 概述

`rdma_transfer.dll` 是一个基于 Windows NetworkDirect (NDSPI) 的 RDMA 文件传输库，提供纯 C 接口，可在任何 C/C++ 项目中调用。

依赖：需要 Mellanox WinOF 驱动和 NDSPI provider 已安装。

---

## 快速集成

### 1. 复制文件

```
your_project/
├── rdma_transfer.h      # 头文件
├── rdma_transfer.lib    # 导入库（链接用）
├── rdma_transfer.dll    # 运行时 DLL（放 exe 同目录）
└── your_app.cpp
```

### 2. 代码

```cpp
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "rdma_transfer.h"
#pragma comment(lib, "rdma_transfer.lib")

int main() {
    rdma_transfer_init();

    // 发送文件到 192.168.100.2:54321
    rdma_send_file("192.168.100.2", 54321, L"D:\\data.bin");

    rdma_transfer_cleanup();
    return 0;
}
```

---

## API 详细说明

### `rdma_transfer_init`

```c
int rdma_transfer_init(void);
```

初始化 RDMA 传输栈。必须在任何其他 API 调用之前调用。

- **返回值**：`0` 成功，`-1` 失败
- **线程安全**：内部用引用计数，可多次调用，只需最后一次 `cleanup` 时真正卸载
- **内部操作**：`WSAStartup` → `NdStartup`

---

### `rdma_transfer_cleanup`

```c
void rdma_transfer_cleanup(void);
```

清理 RDMA 传输栈。与 `rdma_transfer_init` 配对使用。

- **内部操作**：`NdCleanup` → `WSACleanup`
- 如果有未完成的传输，行为未定义（确保传输完成后再调用）

---

### `rdma_send_file`

```c
int rdma_send_file(
    const char*  remote_ip,    // 远端 IPv4 地址，如 "192.168.100.2"
    unsigned short port,       // 端口号，如 54321
    const wchar_t* file_path   // 待发送文件的绝对路径
);
```

通过 RDMA Send/Recv 发送文件到远端。**阻塞调用**，传输完成后返回。

**传输协议**：
1. 发送 268 字节元数据（文件大小 + 文件名）
2. 按 4 MB 分块发送文件数据
3. 远端磁盘写入失败时接收终止信号

- **返回值**：`0` 成功，`-1` 失败
- **进度**：可通过 `rdma_set_progress_callback` 注册回调
- **远端**：必须在接收端先调用 `rdma_recv_file` 等待连接

---

### `rdma_recv_file`

```c
int rdma_recv_file(
    const char*   local_ip,     // 本地 RDMA 网卡 IPv4，如 "192.168.100.2"
    unsigned short port,        // 端口号，需与发送端一致
    const wchar_t* output_path  // 接收文件的保存路径
);
```

监听并接收来自远端的文件。**阻塞调用**，直到连接建立、传输完成或出错才返回。

- **返回值**：`0` 成功，`-1` 失败
- **行为**：创建 `output_path` 指定的文件（覆盖已存在的），写入接收到的数据
- **磁盘写入失败**：向发送端发送终止信号 (cmd=0xDEADBEEF)
- **进度**：可通过 `rdma_set_progress_callback` 注册回调

---

### `rdma_bench`

```c
int rdma_bench(
    int            side,        // 0 = 接收端, 1 = 发送端
    const char*    ip,          // IP 地址
    unsigned short port,        // 端口号
    int            size_mb      // 传输数据量（MiB），接收端据此分配内存
);
```

纯内存 RDMA Write 带宽测试。**不涉及磁盘 I/O**，发送端连续 RDMA Write 到接收端预注册的内存缓冲区。

- **返回值**：`0` 成功，`-1` 失败
- **发送端**：发起连接，接收远端内存令牌后执行 RDMA Write，打印吞吐量到 stdout
- **接收端**：分配 `size_mb` MB 内存并注册为 RDMA 可写，等待发送端写入
- **打印格式**：`RDMA Write: 512 MB in 213.4 ms  |  2399.7 MB/s  (19.20 Gbps)`

---

### `rdma_set_progress_callback`

```c
typedef void (*rdma_progress_cb)(
    double percent,       // 进度百分比 0.0 ~ 100.0
    double speed_mbps,    // 瞬时速度 MB/s
    void*  user_data      // 用户自定义上下文
);

void rdma_set_progress_callback(
    rdma_progress_cb callback,
    void*             user_data
);
```

注册传输进度回调。在每次数据块传输完成后调用（大约每 4 MB 一次）。

- **callback**：传入 `NULL` 取消回调
- **user_data**：透传给回调的上下文指针
- **线程**：回调在传输线程中同步调用，不要在回调中做耗时操作

---

## 示例：带进度的文件发送

```cpp
void on_progress(double pct, double speed, void* ctx) {
    printf("\rProgress: %.1f%%  (%.1f MB/s)", pct, speed);
    fflush(stdout);
}

int main() {
    rdma_transfer_init();
    rdma_set_progress_callback(on_progress, NULL);

    int ret = rdma_send_file("192.168.100.2", 54321, L"D:\\bigfile.bin");
    printf("\nTransfer %s\n", ret == 0 ? "OK" : "FAILED");

    rdma_transfer_cleanup();
    return ret;
}
```

## 典型部署拓扑

```
┌─────────────────┐         RDMA (RoCE v2)         ┌─────────────────┐
│   发送端         │ ◄──────────────────────────► │   接收端         │
│                  │   192.168.100.3 → 100.2       │                  │
│ rdma_send_file() │                               │ rdma_recv_file() │
│   (主动连接)      │                               │   (被动监听)      │
└─────────────────┘                               └─────────────────┘
```

## 错误处理

所有函数返回值：`0` = 成功，`-1` = 失败。库内部使用 `printf` 输出错误详情到 stdout。常见错误码：

| HRESULT | 含义 |
|---|---|
| 0xC00000B5 | ND_IO_TIMEOUT — 远端未及时投递 receive（RNR 超时） |
| 0xC0000120 | ND_CANCELED — QP 被 flush，通常因远端断开 |
| 0xC0000236 | ND_CONNECTION_REFUSED — 远端未监听 |

## 限制

- 仅支持 IPv4 (RoCE v2)
- 仅支持 x64 平台
- 单线程、单 QP
- 与 ndutil.lib 静态链接 (/MT)
- 需要 Visual Studio 2022+ 运行时
