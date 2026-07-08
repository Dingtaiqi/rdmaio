# rdma_transfer.dll — API Reference

基于 Windows NetworkDirect (NDSPI) 的 RDMA 库，纯 C 接口，任何 C/C++ 项目均可调用。

**架构**：RDMA Write + Credit 流控（文件传输）/ Doorbell 通知（共享内存监控）。

**依赖**：Mellanox/Intel/Chelsio ND 驱动。

---

## 目录

| 分组 | 函数 |
|---|---|
| [生命周期](#生命周期) | `rdma_transfer_init`, `rdma_transfer_cleanup` |
| [文件传输](#文件传输) | `rdma_send_file`, `rdma_recv_file` |
| [共享内存监控](#共享内存监控) | `rdma_mem_start`, `rdma_mem_write`, `rdma_mem_buffer`, `rdma_mem_size`, `rdma_mem_wait`, `rdma_mem_last_write`, `rdma_mem_stop` |
| [性能测试](#性能测试) | `rdma_bench` |
| [回调](#回调) | `rdma_set_progress_callback`, `rdma_set_metadata_callback` |
| [控制](#控制) | `rdma_transfer_cancel` |
| [枚举](#枚举) | `rdma_list_adapters` |

---

## 生命周期

### `rdma_transfer_init`

```c
int rdma_transfer_init(void);
```

初始化 RDMA 栈和 Winsock。首次调用执行 `WSAStartup` → `NdStartup`。

| 属性 | 值 |
|---|---|
| 返回值 | `0` 成功, `-1` 失败 |
| 线程安全 | 引用计数，允许多次调用 |

### `rdma_transfer_cleanup`

```c
void rdma_transfer_cleanup(void);
```

清理 RDMA 栈。引用计数归零时执行 `NdCleanup` → `WSACleanup`。

---

## 文件传输

基于 **RDMA Write + Credit 流控**。发送端 RDMA Write 数据到接收端 ring buffer（零 CPU），通过 doorbell/credit 做流控。

### `rdma_send_file`

```c
int rdma_send_file(
    const char*    remote_ip,    // 远端 IPv4, 如 "192.168.100.2"
    unsigned short port,         // 端口号, 如 54321
    const wchar_t* file_path     // 待发送文件的绝对路径
);
```

发送文件。**阻塞**，传输完成或出错才返回。

| 属性 | 值 |
|---|---|
| 返回值 | `0` 成功, `-1` 失败 |
| 可取消 | `rdma_transfer_cancel()` |
| 进度 | `rdma_set_progress_callback` |
| 块大小 | 4 MB |

### `rdma_recv_file`

```c
int rdma_recv_file(
    const char*    local_ip,     // 本地 RDMA 网卡 IP
    unsigned short port,         // 端口号，需与发送端一致
    const wchar_t* output_path   // 保存路径 (目录则自动拼接文件名)
);
```

监听并接收文件。**阻塞**。如果 `output_path` 是目录，自动用元数据中的文件名拼接。

| 属性 | 值 |
|---|---|
| 返回值 | `0` 成功, `-1` 失败 |
| 可取消 | `rdma_transfer_cancel()` |
| 进度 | `rdma_set_progress_callback` |
| 元数据 | `rdma_set_metadata_callback` |

---

## 共享内存监控

一端（写入器）通过 **RDMA Write** 直接写入另一端（显示器）的共享缓冲区，显示器实时看到数据变化——接收端 CPU 在数据路径上完全为零介入。

详细设计见 [RDMA_MEMORY_MONITOR.md](RDMA_MEMORY_MONITOR.md)。

### `rdma_mem_start`

```c
int rdma_mem_start(int mode, const char* ip, unsigned short port, unsigned int size_bytes);
```

启动共享内存会话。

| 参数 | 说明 |
|---|---|
| `mode` | `0` = 显示器（接收端，拥有缓冲区）, `1` = 写入器（发送端，写数据） |
| `ip` | 显示器端 IP（mode=0 时监听此 IP，mode=1 时连接到此 IP） |
| `port` | 端口号 |
| `size_bytes` | 共享缓冲区大小（1 ~ 64 MB） |

| 返回值 | 含义 |
|---|---|
| `0` | 成功 |
| `-1` | 失败（可用 `rdma_transfer_last_error()` 获取错误信息） |

### `rdma_mem_write`

```c
int rdma_mem_write(unsigned int offset, const void* data, unsigned int len);
```

**仅写入器端**。将数据写入远程共享缓冲区。内部先复制到 staging buffer，然后 RDMA Write + doorbell。

| 参数 | 说明 |
|---|---|
| `offset` | 写入偏移量（从共享缓冲区起始） |
| `data` | 源数据指针 |
| `len` | 数据长度（最大 4 MB） |

| 返回值 | 含义 |
|---|---|
| `0` | 成功 |
| `-1` | 失败 |

### `rdma_mem_buffer`

```c
const void* rdma_mem_buffer(void);
```

**仅显示器端**。获取共享缓冲区指针。通过此指针直接读取写入器写入的数据。返回 `NULL` 表示未连接。

### `rdma_mem_size`

```c
unsigned int rdma_mem_size(void);
```

获取共享缓冲区大小（字节）。返回 `0` 表示未连接。

### `rdma_mem_wait`

```c
int rdma_mem_wait(int timeout_ms);
```

**仅显示器端**。阻塞直到下一个 doorbell（数据更新）到达。

| `timeout_ms` | 行为 |
|---|---|
| `> 0` | 最多等待 `timeout_ms` 毫秒 |
| `0` | 无限等待 |
| `< 0` | 非阻塞，立即返回 |

| 返回值 | 含义 |
|---|---|
| `1` | 有新数据（调用 `rdma_mem_buffer` / `rdma_mem_last_write` 读取）|
| `0` | 超时 |
| `-1` | 错误或已取消 |

### `rdma_mem_last_write`

```c
void rdma_mem_last_write(unsigned int* out_offset, unsigned int* out_len);
```

获取最近一次写入的偏移和长度。可在 `rdma_mem_wait()` 返回 `1` 后调用。

### `rdma_mem_stop`

```c
void rdma_mem_stop(void);
```

停止共享内存会话并释放所有资源。幂等，可多次调用。

---

## 性能测试

### `rdma_bench`

```c
int rdma_bench(
    int            side,         // 0 = 接收端, 1 = 发送端
    const char*    ip,           // IP 地址
    unsigned short port,         // 端口号
    int            size_mb       // 传输数据量 (MiB)
);
```

纯内存 RDMA Write 带宽测试，**不涉及磁盘 I/O**。发送端输出：

```
RDMA Write: 1024 MB in 429.8 ms  |  2382.7 MB/s  (19.06 Gbps)
```

| 属性 | 值 |
|---|---|
| 返回值 | `0` 成功, `-1` 失败 |
| 可取消 | `rdma_transfer_cancel()` |
| 接收端 | 分配 `size_mb` MB 内存并注册为远端可写 |
| 发送端 | 连续 RDMA Write + SILENT_SUCCESS |

---

## 回调

### `rdma_set_progress_callback`

```c
typedef void (*rdma_progress_cb)(
    double percent,              // 0.0 ~ 100.0
    double speed_mbps,           // 瞬时速度 MB/s
    void*  user_data             // 用户上下文
);

void rdma_set_progress_callback(
    rdma_progress_cb callback,
    void*             user_data
);
```

每个数据块完成时调用（文件传输约每 4 MB 一次；共享内存监控无进度回调）。传 `NULL` 取消。

> 回调在传输线程内**同步**调用，不要做耗时操作。

### `rdma_set_metadata_callback`

```c
typedef void (*rdma_metadata_cb)(
    const char* filename,        // 原始文件名 (ANSI)
    uint64_t    file_size,       // 文件总字节数
    void*       user_data        // 用户上下文
);

void rdma_set_metadata_callback(
    rdma_metadata_cb callback,
    void*             user_data
);
```

**仅文件传输接收端**。收到发送端元数据时调用，可在回调中确定保存文件名。传 `NULL` 取消。

---

## 控制

### `rdma_transfer_cancel`

```c
void rdma_transfer_cancel(void);
```

从**另一个线程**调用，中断正在进行的 `rdma_send_file` / `rdma_recv_file` / `rdma_bench` / `rdma_mem_wait`。

- 被中断的阻塞调用返回 `-1`
- 空闲时调用无副作用
- 微秒级响应（CQ 轮询立即退出）

### `rdma_transfer_last_error`

```c
const char* rdma_transfer_last_error(void);
```

最后一次错误的描述信息。返回的指针指向静态缓冲区，下次 `rdma_*` 调用可能被覆盖。

---

## 枚举

### `rdma_list_adapters`

```c
typedef struct rdma_adapter_info {
    char     ip_address[64];        // IPv4 地址
    UINT64   adapter_id;            // 适配器 ID
    UINT16   vendor_id;             // PCI vendor (0x15B3 = Mellanox, 0x02C9 = HP)
    UINT16   device_id;             // PCI device (0x1007 = ConnectX-3 Pro)
    UINT32   max_transfer_mb;       // 最大单次传输 (MiB)
    UINT32   max_inline_data;       // 最大内联数据 (字节)
    UINT32   max_cq_depth;          // CQ 深度上限
    UINT32   max_initiator_depth;   // 发送队列深度上限
    UINT32   flags;                 // 原始 AdapterFlags
    int      has_in_order_dma;      // 支持有序 DMA
    int      has_multi_engine;      // 支持多 QP 并行
    int      has_loopback;          // 支持 loopback
} rdma_adapter_info;

int rdma_list_adapters(
    rdma_adapter_info* info,        // NULL 仅取数量
    int                max_count    // 数组容量
);
```

枚举所有 RDMA 网卡。按 IP 去重。

| 返回值 | 含义 |
|---|---|
| `> 0` | 适配器数量 |
| `0` | 无 RDMA 网卡 |
| `-1` | 调用失败 |

---

## 示例

### 共享内存监控

```c
#include "rdma_transfer.h"

// 显示器端
rdma_transfer_init();
rdma_mem_start(0, "192.168.100.2", 54323, 65536);  // 65536 字节缓冲区

while (true) {
    int ret = rdma_mem_wait(1000);  // 等 doorbell, 1 秒超时
    if (ret == 1) {
        unsigned char* buf = (unsigned char*)rdma_mem_buffer();
        unsigned int size = rdma_mem_size();
        unsigned int off, len;
        rdma_mem_last_write(&off, &len);
        printf("写入 @%u +%u: 首字节 = 0x%02X\n", off, len, buf[0]);
    }
}
rdma_mem_stop();
rdma_transfer_cleanup();
```

```c
// 写入器端
rdma_transfer_init();
rdma_mem_start(1, "192.168.100.2", 54323, 65536);

const char* msg = "Hello RDMA Write!";
rdma_mem_write(0, msg, strlen(msg) + 1);

rdma_mem_stop();
rdma_transfer_cleanup();
```

### 文件传输

```c
#include "rdma_transfer.h"

void on_progress(double pct, double speed, void* ctx) {
    printf("\r%.1f%%  %.1f MB/s", pct, speed);
    fflush(stdout);
}

int main() {
    rdma_transfer_init();
    rdma_set_progress_callback(on_progress, NULL);

    int ret = rdma_send_file("192.168.100.2", 54321, L"D:\\data.bin");
    printf("\n%s\n", ret == 0 ? "OK" : "FAILED");

    rdma_transfer_cleanup();
    return ret;
}
```

### 枚举网卡

```c
rdma_transfer_init();

int n = rdma_list_adapters(NULL, 0);
rdma_adapter_info* list = calloc(n, sizeof(*list));
rdma_list_adapters(list, n);

for (int i = 0; i < n; i++)
    printf("[%d] %s  max=%uMB  loopback=%d\n",
           i, list[i].ip_address, list[i].max_transfer_mb, list[i].has_loopback);

free(list);
rdma_transfer_cleanup();
```

### 多线程取消

```c
// 传输线程
rdma_send_file("192.168.100.2", 54321, L"D:\\big.bin");  // 阻塞

// 用户点了取消按钮 → 另一个线程调用
rdma_transfer_cancel();  // 传输线程立即返回 -1
```

---

## 返回值约定

| 返回值 | 含义 |
|---|---|
| `0` | 成功 |
| `-1` | 失败（传输错误 / 被取消） |
| `> 0` | 仅 `rdma_list_adapters`: 适配器数量 |

---

## 限制

- 仅 IPv4 (RoCE v2)
- 仅 x64
- 单线程、单 QP
- 静态链接 CRT (`/MT`)
- 需要 VC++ 2022+ 运行时
