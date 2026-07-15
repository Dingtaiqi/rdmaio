// rdma_file_transfer.cpp - NetworkDirect (NDSPI) based file transfer over RoCE v2
//
// 复用说明：
// - ND2 初始化 / 地址解析 / 适配器打开模式复用自
//   D:\rdma\NetworkDirect\src\examples\ndpingpong\ndpingpong.cpp 的客户端/服务器流程。
// - CQ 轮询 (GetResults) 与 Send/Receive 完成处理模式复用自
//   D:\rdma\NetworkDirect\src\examples\ndtestutil\ndtestutil.cpp 的 WaitForCompletion 实现。
// - 内存注册 (Register/Deregister) 与连接器 Bind/Connect/Accept 复用自
//   D:\rdma\NetworkDirect\src\examples\ndtestutil\ndtestutil.cpp。
// - 注意：本地 NetworkDirect SDK 未提供 NdGetLastError，因此使用 GetLastError() 获取系统错误码。
//

#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <initguid.h>
#include <ndspi.h>
#include <ndsupport.h>
#include <nddef.h>
#include <ndstatus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#pragma comment(lib, "Ws2_32.lib")

static const USHORT DEFAULT_PORT = 54321;
static const DWORD CHUNK_SIZE = 4 * 1024 * 1024;      // 4 MiB for better throughput
static bool g_isRead = false;   // global flag for RDMA Read bench mode
static const DWORD CQ_DEPTH = 256;
static const DWORD QP_DEPTH = 128;      // maximum pipeline depth
static const DWORD ALIGNMENT = 4096;

#define META_SEND_CTX    ((void*)0x1001)
#define META_RECV_CTX    ((void*)0x1002)
#define CHUNK_SEND_CTX   ((void*)0x2001)
#define CHUNK_RECV_CTX   ((void*)0x2002)
#define TERM_SEND_CTX    ((void*)0x3001)
#define TERM_RECV_CTX    ((void*)0x3002)

#define TERM_CMD_ABORT   0xDEADBEEFUL

// 元数据包：按题目给出的结构实现。
// 由于 uint64_t + uint32_t + char[256] = 268 字节，实际传输 sizeof(FileMeta) 字节。
#pragma pack(push, 1)
struct FileMeta
{
    uint64_t file_size;
    uint32_t filename_len;
    char     filename[256];
};
#pragma pack(pop)

struct TerminateCmd
{
    uint32_t cmd;
};

struct NdContext
{
    IND2Adapter* pAdapter = nullptr;
    HANDLE       hOvFile = INVALID_HANDLE_VALUE;
    IND2CompletionQueue* pCq = nullptr;
    IND2MemoryRegion*    pMr = nullptr;
    IND2QueuePair*       pQp = nullptr;
    IND2Connector*       pConnector = nullptr;
    IND2Listener*        pListener = nullptr;
    OVERLAPPED           ov = {};

    ~NdContext();
};

static void CleanupNd(NdContext& ctx)
{
    HRESULT hr;

    // Drain CQ first per Microsoft NetworkDirect Disconnect 方案:
    // "QP 上的所有 DMA 活动都已完成" 后才能调用 Disconnect。
    if (ctx.pCq != nullptr) {
        ND2_RESULT drain[16];
        while (ctx.pCq->GetResults(drain, ARRAYSIZE(drain)) > 0) {}
    }

    if (ctx.pConnector != nullptr)
    {
        hr = ctx.pConnector->Disconnect(&ctx.ov);
        if (hr == ND_PENDING)
        {
            ctx.pConnector->GetOverlappedResult(&ctx.ov, TRUE);
        }
    }

    if (ctx.pMr != nullptr)
    {
        hr = ctx.pMr->Deregister(&ctx.ov);
        if (hr == ND_PENDING)
        {
            ctx.pMr->GetOverlappedResult(&ctx.ov, TRUE);
        }
    }

    if (ctx.pQp != nullptr)       { ctx.pQp->Release();       ctx.pQp = nullptr; }
    if (ctx.pCq != nullptr)       { ctx.pCq->Release();       ctx.pCq = nullptr; }
    if (ctx.pConnector != nullptr){ ctx.pConnector->Release();ctx.pConnector = nullptr; }
    if (ctx.pListener != nullptr) { ctx.pListener->Release(); ctx.pListener = nullptr; }
    if (ctx.pMr != nullptr)       { ctx.pMr->Release();       ctx.pMr = nullptr; }
    if (ctx.hOvFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(ctx.hOvFile);
        ctx.hOvFile = INVALID_HANDLE_VALUE;
    }
    if (ctx.pAdapter != nullptr)  { ctx.pAdapter->Release();  ctx.pAdapter = nullptr; }
    if (ctx.ov.hEvent != nullptr)
    {
        CloseHandle(ctx.ov.hEvent);
        ctx.ov.hEvent = nullptr;
    }
}

NdContext::~NdContext()
{
    CleanupNd(*this);
}

static void PrintUsage(const wchar_t* exe)
{
    printf("Usage:\n"
           "  %ls -send -ip <remote_ipv4> -file <input_path>\n"
           "  %ls -recv -ip <local_ipv4>  -o <output_path>\n"
           "Example:\n"
           "  %ls -send -ip 192.168.100.2 -file D:\\data.bin\n"
           "  %ls -recv -ip 192.168.100.1 -o C:\\received.bin\n",
           exe, exe, exe, exe);
}

static void* AllocAligned(size_t size)
{
    void* p = _aligned_malloc(size, ALIGNMENT);
    if (p == nullptr)
    {
        printf("Failed to allocate %zu bytes aligned to %lu\n", size, ALIGNMENT);
    }
    else
    {
        memset(p, 0, size);
    }
    return p;
}

static void FreeAligned(void* p)
{
    if (p != nullptr)
    {
        _aligned_free(p);
    }
}

static int ParseIpv4(const wchar_t* ipStr, USHORT port, struct sockaddr_in* pAddr)
{
    memset(pAddr, 0, sizeof(*pAddr));
    pAddr->sin_family = AF_INET;
    pAddr->sin_port = htons(port);

    int ret = InetPtonW(AF_INET, ipStr, &pAddr->sin_addr);
    if (ret != 1)
    {
        printf("Invalid IPv4 address: %ls\n", ipStr);
        return -1;
    }
    return 0;
}

static HRESULT WaitOverlapped(IND2Overlapped* pObj, OVERLAPPED* pOv)
{
    // 复用自 D:\rdma\NetworkDirect\src\examples\ndtestutil\ndtestutil.cpp 中
    // 对 ND_PENDING 后 GetOverlappedResult 的处理模式。
    return pObj->GetOverlappedResult(pOv, TRUE);
}

static void ExtractFileName(const wchar_t* path, char* out, uint32_t* pLen, size_t maxLen)
{
    const wchar_t* name = wcsrchr(path, L'\\');
    if (name == nullptr)
    {
        name = wcsrchr(path, L'/');
    }
    if (name == nullptr)
    {
        name = path;
    }
    else
    {
        name++;
    }

    int n = WideCharToMultiByte(CP_ACP, 0, name, -1, out, (int)maxLen, nullptr, nullptr);
    if (n > 0)
    {
        n--; // exclude null terminator
    }
    else
    {
        n = 0;
    }
    *pLen = (uint32_t)n;
}

static void PrintProgress(uint64_t bytes, uint64_t total, double elapsedSec)
{
    double pct = total ? (100.0 * (double)bytes / (double)total) : 0.0;
    double mb = (double)bytes / (1024.0 * 1024.0);
    double totalMb = (double)total / (1024.0 * 1024.0);
    double speed = elapsedSec > 0.0 ? (mb / elapsedSec) : 0.0;
    printf("\rTransferred: %.2f%% (%.2f MB / %.2f MB) | Speed: %.2f MB/s",
           pct, mb, totalMb, speed);
    fflush(stdout);
}

static void PrintFinalStats(uint64_t bytes, double elapsedSec, const char* role)
{
    double ms = elapsedSec * 1000.0;
    double mb = (double)bytes / (1024.0 * 1024.0);
    double avgSpeed = elapsedSec > 0.0 ? (mb / elapsedSec) : 0.0;
    printf("\n%s: total time %.2f ms, average bandwidth %.2f MB/s\n",
           role, ms, avgSpeed);
}

static void SendAbortSignal(NdContext& ctx, ND2_SGE& sge, TerminateCmd* pTerm)
{
    pTerm->cmd = TERM_CMD_ABORT;
    sge.Buffer = pTerm;
    sge.BufferLength = sizeof(TerminateCmd);

    HRESULT hr = ctx.pQp->Send(TERM_SEND_CTX, &sge, 1, 0);
    if (FAILED(hr))
    {
        printf("Send abort signal failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
        return;
    }

    ND2_RESULT result = {};
    bool termDone = false;
    while (!termDone)
    {
        while (ctx.pCq->GetResults(&result, 1) == 0) {}
        if (result.Status != ND_SUCCESS)
        {
            printf("Abort send completion error: 0x%08X\n", result.Status);
            break;
        }
        if (result.RequestContext == TERM_SEND_CTX)
        {
            termDone = true;
        }
    }
}

static int RunSender(const wchar_t* remoteIp, const wchar_t* filePath)
{
    int ret = 0;
    NdContext ctx;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    void* pBuf = nullptr;

    WSADATA wsaData;
    int wsaRet = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaRet != 0)
    {
        printf("WSAStartup failed: %d\n", wsaRet);
        return -1;
    }

    HRESULT hr = NdStartup();
    if (FAILED(hr))
    {
        printf("NdStartup failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
        ret = -1;
        goto done;
    }

    // 用一个块包裹所有可能使用局部变量初始化的逻辑，避免 goto 跳过初始化。
    do
    {
        struct sockaddr_in remoteAddr = {};
        if (ParseIpv4(remoteIp, DEFAULT_PORT, &remoteAddr) != 0)
        {
            ret = -1;
            break;
        }

        // 复用自 D:\rdma\NetworkDirect\src\examples\ndpingpong\ndpingpong.cpp 客户端地址解析流程。
        struct sockaddr_in localAddr = {};
        SIZE_T addrLen = sizeof(localAddr);
        hr = NdResolveAddress(
            reinterpret_cast<const struct sockaddr*>(&remoteAddr),
            sizeof(remoteAddr),
            reinterpret_cast<struct sockaddr*>(&localAddr),
            &addrLen);
        if (FAILED(hr))
        {
            printf("NdResolveAddress failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        hr = NdOpenAdapter(
            IID_IND2Adapter,
            reinterpret_cast<const struct sockaddr*>(&localAddr),
            sizeof(localAddr),
            reinterpret_cast<void**>(&ctx.pAdapter));
        if (FAILED(hr))
        {
            printf("NdOpenAdapter failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        ctx.ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (ctx.ov.hEvent == nullptr)
        {
            printf("CreateEvent failed: %lu\n", GetLastError());
            ret = -1;
            break;
        }

        hr = ctx.pAdapter->CreateOverlappedFile(&ctx.hOvFile);
        if (FAILED(hr))
        {
            printf("CreateOverlappedFile failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        // 复用自 D:\rdma\NetworkDirect\src\examples\ndtestutil\ndtestutil.cpp 的对象创建顺序。
        hr = ctx.pAdapter->CreateCompletionQueue(
            IID_IND2CompletionQueue,
            ctx.hOvFile,
            CQ_DEPTH,
            0,
            0,
            reinterpret_cast<void**>(&ctx.pCq));
        if (FAILED(hr))
        {
            printf("CreateCompletionQueue failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        hr = ctx.pAdapter->CreateConnector(
            IID_IND2Connector,
            ctx.hOvFile,
            reinterpret_cast<void**>(&ctx.pConnector));
        if (FAILED(hr))
        {
            printf("CreateConnector failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        hr = ctx.pAdapter->CreateQueuePair(
            IID_IND2QueuePair,
            ctx.pCq,
            ctx.pCq,
            nullptr,
            QP_DEPTH,
            QP_DEPTH,
            1,
            1,
            0,
            reinterpret_cast<void**>(&ctx.pQp));
        if (FAILED(hr))
        {
            printf("CreateQueuePair failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        hr = ctx.pAdapter->CreateMemoryRegion(
            IID_IND2MemoryRegion,
            ctx.hOvFile,
            reinterpret_cast<void**>(&ctx.pMr));
        if (FAILED(hr))
        {
            printf("CreateMemoryRegion failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        // 整块缓冲区一次 _aligned_malloc，满足 4096 对齐；内部按页偏移分给 meta/chunk/term。
        const size_t bufSize = static_cast<size_t>(CHUNK_SIZE) + 8192;
        pBuf = AllocAligned(bufSize);
        if (pBuf == nullptr)
        {
            ret = -1;
            break;
        }

        hr = ctx.pMr->Register(pBuf, bufSize, ND_MR_FLAG_ALLOW_LOCAL_WRITE, &ctx.ov);
        if (hr == ND_PENDING)
        {
            hr = WaitOverlapped(ctx.pMr, &ctx.ov);
        }
        if (FAILED(hr))
        {
            printf("Register failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }
        const UINT32 localToken = ctx.pMr->GetLocalToken();

        // 连接远端。
        hr = ctx.pConnector->Bind(
            reinterpret_cast<const struct sockaddr*>(&localAddr),
            sizeof(localAddr));
        if (hr == ND_PENDING)
        {
            hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        }
        if (FAILED(hr))
        {
            printf("Connector Bind failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        hr = ctx.pConnector->Connect(
            ctx.pQp,
            reinterpret_cast<const struct sockaddr*>(&remoteAddr),
            sizeof(remoteAddr),
            0,
            0,
            nullptr,
            0,
            &ctx.ov);
        if (hr == ND_PENDING)
        {
            hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        }
        if (FAILED(hr))
        {
            printf("Connect failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        hr = ctx.pConnector->CompleteConnect(&ctx.ov);
        if (hr == ND_PENDING)
        {
            hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        }
        if (FAILED(hr))
        {
            printf("CompleteConnect failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        // 打开待发送文件。
        hFile = CreateFileW(
            filePath,
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            printf("CreateFileW(open read) failed for %ls: %lu\n", filePath, GetLastError());
            ret = -1;
            break;
        }

        LARGE_INTEGER fileSizeLi;
        if (!GetFileSizeEx(hFile, &fileSizeLi))
        {
            printf("GetFileSizeEx failed: %lu\n", GetLastError());
            ret = -1;
            break;
        }
        const uint64_t fileSize = static_cast<uint64_t>(fileSizeLi.QuadPart);

        // 元数据区 / 数据块区 / 终止信号区布局。
        FileMeta* pMeta = static_cast<FileMeta*>(pBuf);
        char* pChunk = static_cast<char*>(pBuf) + ALIGNMENT;
        TerminateCmd* pTerm = reinterpret_cast<TerminateCmd*>(pChunk + CHUNK_SIZE);

        pMeta->file_size = fileSize;
        ExtractFileName(filePath, pMeta->filename, &pMeta->filename_len, sizeof(pMeta->filename));

        // 发送元数据。
        ND2_SGE sge = {};
        sge.Buffer = pMeta;
        sge.BufferLength = sizeof(FileMeta);
        sge.MemoryRegionToken = localToken;

        hr = ctx.pQp->Send(META_SEND_CTX, &sge, 1, 0);
        if (FAILED(hr))
        {
            printf("Send meta failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        ND2_RESULT result = {};
        bool metaDone = false;
        while (!metaDone)
        {
            while (ctx.pCq->GetResults(&result, 1) == 0) {}
            if (result.Status != ND_SUCCESS)
            {
                printf("Send meta completion error: 0x%08X, GetLastError=%lu\n", result.Status, GetLastError());
                ret = -1;
                break;
            }
            if (result.RequestContext == META_SEND_CTX)
            {
                metaDone = true;
            }
            else
            {
                printf("Unexpected completion context while sending meta\n");
                ret = -1;
                break;
            }
        }
        if (ret != 0)
        {
            break;
        }

        // 提前发布对端终止信号的接收。
        sge.Buffer = pTerm;
        sge.BufferLength = sizeof(TerminateCmd);
        hr = ctx.pQp->Receive(TERM_RECV_CTX, &sge, 1);
        if (FAILED(hr))
        {
            printf("Post terminate-receive failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        // 分块发送文件数据。
        uint64_t bytesSent = 0;
        bool aborted = false;
        LARGE_INTEGER freq;
        LARGE_INTEGER tStart;
        LARGE_INTEGER tNow;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&tStart);

        while (bytesSent < fileSize && !aborted)
        {
            DWORD toRead = (fileSize - bytesSent > CHUNK_SIZE)
                ? CHUNK_SIZE
                : static_cast<DWORD>(fileSize - bytesSent);

            DWORD readBytes = 0;
            if (!ReadFile(hFile, pChunk, toRead, &readBytes, nullptr) || readBytes != toRead)
            {
                printf("\nReadFile failed at offset %llu: %lu\n", bytesSent, GetLastError());
                ret = -1;
                aborted = true;
                break;
            }

            sge.Buffer = pChunk;
            sge.BufferLength = toRead;
            hr = ctx.pQp->Send(CHUNK_SEND_CTX, &sge, 1, 0);
            if (FAILED(hr))
            {
                printf("Send chunk failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
                ret = -1;
                break;
            }

            bool sendDone = false;
            while (!sendDone && !aborted)
            {
                while (ctx.pCq->GetResults(&result, 1) == 0) {}
                if (result.Status != ND_SUCCESS)
                {
                    printf("Chunk send completion error: 0x%08X, GetLastError=%lu\n",
                           result.Status, GetLastError());
                    ret = -1;
                    break;
                }

                if (result.RequestContext == CHUNK_SEND_CTX)
                {
                    sendDone = true;
                    bytesSent += toRead;
                }
                else if (result.RequestContext == TERM_RECV_CTX)
                {
                    if (pTerm->cmd == TERM_CMD_ABORT)
                    {
                        printf("\nReceiver signaled abort (disk write failure).\n");
                        aborted = true;
                        ret = -1;
                    }
                    else
                    {
                        printf("Unexpected terminate command: 0x%08X\n", pTerm->cmd);
                        ret = -1;
                        break;
                    }
                }
                else
                {
                    printf("Unexpected completion context in sender loop\n");
                    ret = -1;
                    break;
                }
            }
            if (ret != 0)
            {
                break;
            }

            QueryPerformanceCounter(&tNow);
            double elapsed = static_cast<double>(tNow.QuadPart - tStart.QuadPart) /
                             static_cast<double>(freq.QuadPart);
            PrintProgress(bytesSent, fileSize, elapsed);
        }

        QueryPerformanceCounter(&tNow);
        double elapsed = static_cast<double>(tNow.QuadPart - tStart.QuadPart) /
                         static_cast<double>(freq.QuadPart);
        PrintProgress(bytesSent, fileSize, elapsed);
        PrintFinalStats(bytesSent, elapsed, "Sender");
    } while (0);

done:
    if (hFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hFile);
    }
    FreeAligned(pBuf);
    NdCleanup();
    WSACleanup();
    return ret;
}

static int RunReceiver(const wchar_t* localIp, const wchar_t* outPath)
{
    int ret = 0;
    NdContext ctx;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    void* pBuf = nullptr;

    WSADATA wsaData;
    int wsaRet = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaRet != 0)
    {
        printf("WSAStartup failed: %d\n", wsaRet);
        return -1;
    }

    HRESULT hr = NdStartup();
    if (FAILED(hr))
    {
        printf("NdStartup failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
        ret = -1;
        goto done;
    }

    do
    {
        struct sockaddr_in localAddr = {};
        if (ParseIpv4(localIp, DEFAULT_PORT, &localAddr) != 0)
        {
            ret = -1;
            break;
        }

        hr = NdOpenAdapter(
            IID_IND2Adapter,
            reinterpret_cast<const struct sockaddr*>(&localAddr),
            sizeof(localAddr),
            reinterpret_cast<void**>(&ctx.pAdapter));
        if (FAILED(hr))
        {
            printf("NdOpenAdapter failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        ctx.ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (ctx.ov.hEvent == nullptr)
        {
            printf("CreateEvent failed: %lu\n", GetLastError());
            ret = -1;
            break;
        }

        hr = ctx.pAdapter->CreateOverlappedFile(&ctx.hOvFile);
        if (FAILED(hr))
        {
            printf("CreateOverlappedFile failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        hr = ctx.pAdapter->CreateCompletionQueue(
            IID_IND2CompletionQueue,
            ctx.hOvFile,
            CQ_DEPTH,
            0,
            0,
            reinterpret_cast<void**>(&ctx.pCq));
        if (FAILED(hr))
        {
            printf("CreateCompletionQueue failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        hr = ctx.pAdapter->CreateConnector(
            IID_IND2Connector,
            ctx.hOvFile,
            reinterpret_cast<void**>(&ctx.pConnector));
        if (FAILED(hr))
        {
            printf("CreateConnector failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        hr = ctx.pAdapter->CreateQueuePair(
            IID_IND2QueuePair,
            ctx.pCq,
            ctx.pCq,
            nullptr,
            QP_DEPTH,
            QP_DEPTH,
            1,
            1,
            0,
            reinterpret_cast<void**>(&ctx.pQp));
        if (FAILED(hr))
        {
            printf("CreateQueuePair failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        hr = ctx.pAdapter->CreateMemoryRegion(
            IID_IND2MemoryRegion,
            ctx.hOvFile,
            reinterpret_cast<void**>(&ctx.pMr));
        if (FAILED(hr))
        {
            printf("CreateMemoryRegion failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        // 4 缓冲环：Q16 QP 深度足够支撑流水线，取 4 个缓冲区轮转。
        const DWORD NUM_CHUNK_BUFS = QP_DEPTH - 1; // reserve 1 slot for meta receive
        const size_t bufSize = static_cast<size_t>(ALIGNMENT + CHUNK_SIZE * NUM_CHUNK_BUFS + 8192);
        pBuf = AllocAligned(bufSize);
        if (pBuf == nullptr)
        {
            ret = -1;
            break;
        }

        hr = ctx.pMr->Register(pBuf, bufSize, ND_MR_FLAG_ALLOW_LOCAL_WRITE, &ctx.ov);
        if (hr == ND_PENDING)
        {
            hr = WaitOverlapped(ctx.pMr, &ctx.ov);
        }
        if (FAILED(hr))
        {
            printf("Register failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }
        const UINT32 localToken = ctx.pMr->GetLocalToken();

        // 复用自 D:\rdma\NetworkDirect\src\examples\ndtestutil\ndtestutil.cpp 的服务器监听流程。
        hr = ctx.pAdapter->CreateListener(
            IID_IND2Listener,
            ctx.hOvFile,
            reinterpret_cast<void**>(&ctx.pListener));
        if (FAILED(hr))
        {
            printf("CreateListener failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        hr = ctx.pListener->Bind(
            reinterpret_cast<const struct sockaddr*>(&localAddr),
            sizeof(localAddr));
        if (FAILED(hr))
        {
            printf("Listener Bind failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        hr = ctx.pListener->Listen(1);
        if (FAILED(hr))
        {
            printf("Listen failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        printf("Receiver listening on %ls:%hu, waiting for sender...\n", localIp, DEFAULT_PORT);
        fflush(stdout);

        hr = ctx.pListener->GetConnectionRequest(ctx.pConnector, &ctx.ov);
        if (hr == ND_PENDING)
        {
            hr = WaitOverlapped(ctx.pListener, &ctx.ov);
        }
        if (FAILED(hr))
        {
            printf("GetConnectionRequest failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        hr = ctx.pConnector->Accept(ctx.pQp, 0, 0, nullptr, 0, &ctx.ov);
        if (hr == ND_PENDING)
        {
            hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        }
        if (FAILED(hr))
        {
            printf("Accept failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }

        // 缓冲区布局：
        //   [0 .. ALIGNMENT-1]                     = FileMeta
        //   [ALIGNMENT .. ALIGNMENT+CHUNK_SIZE-1]  = chunk buffer A
        //   [ALIGNMENT + CHUNK_SIZE * NUM_CHUNK_BUFS .. ]  = TerminateCmd
        FileMeta* pMeta = static_cast<FileMeta*>(pBuf);
        char* chunkBufs[NUM_CHUNK_BUFS];
        for (DWORD i = 0; i < NUM_CHUNK_BUFS; i++)
        {
            chunkBufs[i] = static_cast<char*>(pBuf) + ALIGNMENT + i * CHUNK_SIZE;
        }
        TerminateCmd* pTerm = reinterpret_cast<TerminateCmd*>(
            static_cast<char*>(pBuf) + ALIGNMENT + CHUNK_SIZE * NUM_CHUNK_BUFS);

        // 接收元数据的 SGE。
        ND2_SGE sge_meta = {};
        sge_meta.Buffer = pMeta;
        sge_meta.BufferLength = sizeof(FileMeta);
        sge_meta.MemoryRegionToken = localToken;

        ND2_SGE sge_chunk = {};
        sge_chunk.MemoryRegionToken = localToken;

        // 预投递元数据 + NUM_CHUNK_BUFS 个数据块缓冲区的 receive，
        // 保证发送端持续发送时接收队列永不枯竭。
        hr = ctx.pQp->Receive(META_RECV_CTX, &sge_meta, 1);
        if (FAILED(hr))
        {
            printf("Post meta receive failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
            ret = -1;
            break;
        }
        for (DWORD i = 0; i < NUM_CHUNK_BUFS; i++)
        {
            sge_chunk.Buffer = chunkBufs[i];
            sge_chunk.BufferLength = CHUNK_SIZE;
            hr = ctx.pQp->Receive(CHUNK_RECV_CTX, &sge_chunk, 1);
            if (FAILED(hr))
            {
                printf("Post chunk receive %lu failed: 0x%08X, GetLastError=%lu\n",
                       i, hr, GetLastError());
                ret = -1;
                break;
            }
        }
        if (ret != 0)
        {
            break;
        }

        // 等待元数据。
        ND2_RESULT result = {};
        bool metaDone = false;
        while (!metaDone)
        {
            while (ctx.pCq->GetResults(&result, 1) == 0) {}
            if (result.Status != ND_SUCCESS)
            {
                printf("Meta receive completion error: 0x%08X, GetLastError=%lu\n",
                       result.Status, GetLastError());
                ret = -1;
                break;
            }
            if (result.RequestContext == META_RECV_CTX)
            {
                metaDone = true;
            }
        }
        if (ret != 0)
        {
            break;
        }

        if (result.BytesTransferred != sizeof(FileMeta))
        {
            printf("Meta receive size mismatch: got %lu, expected %zu\n",
                   result.BytesTransferred, sizeof(FileMeta));
            ret = -1;
            break;
        }

        // 创建输出文件。
        hFile = CreateFileW(
            outPath,
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            printf("CreateFileW(create write) failed for %ls: %lu\n", outPath, GetLastError());
            SendAbortSignal(ctx, sge_chunk, pTerm);
            ret = -1;
            break;
        }

        const uint64_t fileSize = pMeta->file_size;
        printf("Receiving file '%.*s', size %llu bytes...\n",
               pMeta->filename_len, pMeta->filename, fileSize);

        // 批量处理：先收割所有已完成的 chunk，批量投递 receive 维持流水线深度，
        // 再批量写盘减少写盘和轮询的交叉等待时间。
        uint64_t bytesReceived = 0;
        DWORD ringIdx = 0;
        bool diskError = false;
        LARGE_INTEGER freq;
        LARGE_INTEGER tStart;
        LARGE_INTEGER tNow;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&tStart);

        while (bytesReceived < fileSize && !diskError)
        {
            // 收割当前可用的一条完成记录。
            ND2_RESULT result = {};
            while (ctx.pCq->GetResults(&result, 1) == 0) {}
            if (result.Status != ND_SUCCESS)
            {
                printf("Chunk receive completion error: 0x%08X, GetLastError=%lu\n",
                       result.Status, GetLastError());
                ret = -1;
                diskError = true;
                break;
            }
            if (result.RequestContext != CHUNK_RECV_CTX)
            {
                continue;
            }

            DWORD received = result.BytesTransferred;

            // 立即重新投递刚被消耗的缓冲区，延迟写盘到批量阶段。
            if (bytesReceived + received < fileSize)
            {
                DWORD nextExpect = (fileSize - (bytesReceived + received) > CHUNK_SIZE)
                    ? CHUNK_SIZE
                    : static_cast<DWORD>(fileSize - (bytesReceived + received));
                sge_chunk.Buffer = chunkBufs[ringIdx];
                sge_chunk.BufferLength = nextExpect;
                hr = ctx.pQp->Receive(CHUNK_RECV_CTX, &sge_chunk, 1);
                if (FAILED(hr))
                {
                    printf("Repost chunk receive failed: 0x%08X, GetLastError=%lu\n", hr, GetLastError());
                    ret = -1;
                    diskError = true;
                    break;
                }
            }

            // 写盘。
            DWORD written = 0;
            if (!WriteFile(hFile, chunkBufs[ringIdx], received, &written, nullptr) || written != received)
            {
                printf("\nWriteFile failed at offset %llu: %lu\n", bytesReceived, GetLastError());
                ret = -1;
                diskError = true;
                break;
            }

            bytesReceived += received;
            ringIdx = (ringIdx + 1) % NUM_CHUNK_BUFS;

            QueryPerformanceCounter(&tNow);
            double elapsed = static_cast<double>(tNow.QuadPart - tStart.QuadPart) /
                             static_cast<double>(freq.QuadPart);
            PrintProgress(bytesReceived, fileSize, elapsed);
        }

        if (diskError)
        {
            SendAbortSignal(ctx, sge_chunk, pTerm);
        }

        QueryPerformanceCounter(&tNow);
        double elapsed = static_cast<double>(tNow.QuadPart - tStart.QuadPart) /
                         static_cast<double>(freq.QuadPart);
        PrintProgress(bytesReceived, fileSize, elapsed);
        PrintFinalStats(bytesReceived, elapsed, "Receiver");
    } while (0);

done:
    if (hFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hFile);
    }
    FreeAligned(pBuf);
    NdCleanup();
    WSACleanup();
    return ret;
}

// ============================================================
// 纯内存 RDMA Write 带宽测试 (bench)
// 约定：接收端先交换远程内存信息，发送端连续 RDMA Write 测速。
// ============================================================

struct BenchPeerInfo
{
    UINT64 remoteAddress;
    UINT32 remoteToken;
};

static int RunBenchReceiver(const wchar_t* localIp)
{
    int ret = 0;
    NdContext ctx;
    void* pBuf = nullptr;
    IND2MemoryWindow* pMw = nullptr;

    WSADATA wsaData;
    int wsaRet = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaRet != 0) { printf("WSAStartup failed: %d\n", wsaRet); return -1; }

    HRESULT hr = NdStartup();
    if (FAILED(hr)) { printf("NdStartup failed: 0x%08X\n", hr); ret = -1; goto done; }

    do
    {
        struct sockaddr_in localAddr = {};
        if (ParseIpv4(localIp, DEFAULT_PORT, &localAddr) != 0) { ret = -1; break; }

        hr = NdOpenAdapter(IID_IND2Adapter,
            reinterpret_cast<const struct sockaddr*>(&localAddr), sizeof(localAddr),
            reinterpret_cast<void**>(&ctx.pAdapter));
        if (FAILED(hr)) { printf("NdOpenAdapter: 0x%08X\n", hr); ret = -1; break; }

        ctx.ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        hr = ctx.pAdapter->CreateOverlappedFile(&ctx.hOvFile);
        if (FAILED(hr)) { printf("CreateOverlappedFile: 0x%08X\n", hr); ret = -1; break; }

        hr = ctx.pAdapter->CreateCompletionQueue(IID_IND2CompletionQueue,
            ctx.hOvFile, CQ_DEPTH, 0, 0, reinterpret_cast<void**>(&ctx.pCq));
        if (FAILED(hr)) { printf("CreateCQ: 0x%08X\n", hr); ret = -1; break; }

        hr = ctx.pAdapter->CreateConnector(IID_IND2Connector,
            ctx.hOvFile, reinterpret_cast<void**>(&ctx.pConnector));
        if (FAILED(hr)) { printf("CreateConnector: 0x%08X\n", hr); ret = -1; break; }

        hr = ctx.pAdapter->CreateQueuePair(IID_IND2QueuePair,
            ctx.pCq, ctx.pCq, nullptr, QP_DEPTH, QP_DEPTH,
            1, 1, 0, reinterpret_cast<void**>(&ctx.pQp));
        if (FAILED(hr)) { printf("CreateQP: 0x%08X\n", hr); ret = -1; break; }

        hr = ctx.pAdapter->CreateMemoryRegion(IID_IND2MemoryRegion,
            ctx.hOvFile, reinterpret_cast<void**>(&ctx.pMr));
        if (FAILED(hr)) { printf("CreateMR: 0x%08X\n", hr); ret = -1; break; }

        const DWORD benchBufSize = 512 * 1024 * 1024;  // 512 MB
        pBuf = AllocAligned(benchBufSize);
        if (!pBuf) { ret = -1; break; }
        printf("[BENCH-RECV] memset 512MB...\n"); fflush(stdout);
        memset(pBuf, 0xAB, benchBufSize);
        printf("[BENCH-RECV] Register 512MB MR...\n"); fflush(stdout);

        hr = ctx.pMr->Register(pBuf, benchBufSize,
            ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE | ND_MR_FLAG_ALLOW_REMOTE_READ, &ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pMr, &ctx.ov);
        if (FAILED(hr)) { printf("Register: 0x%08X\n", hr); ret = -1; break; }
        UINT32 localToken = ctx.pMr->GetLocalToken();

        // Query adapter for InboundReadLimit (required for RDMA Read)
        ND2_ADAPTER_INFO adapterInfo = {};
        adapterInfo.InfoVersion = ND_VERSION_2;
        ULONG aiSize = sizeof(adapterInfo);
        ctx.pAdapter->Query(&adapterInfo, &aiSize);
        ULONG inboundReadLimit = adapterInfo.MaxInboundReadLimit;
        if (inboundReadLimit == 0) inboundReadLimit = 16; // safe default

        // Listen & Accept
        hr = ctx.pAdapter->CreateListener(IID_IND2Listener,
            ctx.hOvFile, reinterpret_cast<void**>(&ctx.pListener));
        if (FAILED(hr)) { printf("CreateListener: 0x%08X\n", hr); ret = -1; break; }

        hr = ctx.pListener->Bind(reinterpret_cast<const struct sockaddr*>(&localAddr), sizeof(localAddr));
        if (FAILED(hr)) { printf("Bind: 0x%08X\n", hr); ret = -1; break; }
        hr = ctx.pListener->Listen(1);
        if (FAILED(hr)) { printf("Listen: 0x%08X\n", hr); ret = -1; break; }

        printf("Bench receiver listening on %ls:%hu...\n", localIp, DEFAULT_PORT);
        fflush(stdout);

        printf("[BENCH-RECV] Waiting for connection...\n"); fflush(stdout);
        hr = ctx.pListener->GetConnectionRequest(ctx.pConnector, &ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pListener, &ctx.ov);
        if (FAILED(hr)) { printf("GetConnectionRequest: 0x%08X\n", hr); ret = -1; break; }
        printf("[BENCH-RECV] Connection request received\n"); fflush(stdout);

        hr = ctx.pConnector->Accept(ctx.pQp, inboundReadLimit, 0, nullptr, 0, &ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        if (FAILED(hr)) { printf("Accept: 0x%08X\n", hr); ret = -1; break; }
        printf("[BENCH-RECV] Accept OK\n"); fflush(stdout);

        // For RDMA Read: create Memory Window and bind (ndrping pattern)
        UINT32 remoteToken;
        if (g_isRead) {
            hr = ctx.pAdapter->CreateMemoryWindow(IID_IND2MemoryWindow,
                reinterpret_cast<void**>(&pMw));
            if (FAILED(hr)) { printf("CreateMW: 0x%08X\n", hr); ret = -1; break; }
            hr = ctx.pQp->Bind((void*)0x7001, ctx.pMr, pMw, pBuf, benchBufSize,
                ND_OP_FLAG_ALLOW_READ);
            if (FAILED(hr)) { printf("Bind MW: 0x%08X\n", hr); ret = -1; break; }
            ND2_RESULT bres = {};
            while (ctx.pCq->GetResults(&bres, 1) == 0) {
                ctx.pCq->Notify(ND_CQ_NOTIFY_ANY, &ctx.ov);
                ctx.pCq->GetOverlappedResult(&ctx.ov, TRUE);
            }
            if (bres.Status != ND_SUCCESS) {
                printf("Bind MW: 0x%08X\n", bres.Status); ret = -1; break;
            }
            remoteToken = pMw->GetRemoteToken();
        } else {
            remoteToken = ctx.pMr->GetRemoteToken();
        }

        // Send memory info to sender
        BenchPeerInfo* pInfo = static_cast<BenchPeerInfo*>(pBuf);
        pInfo->remoteAddress = reinterpret_cast<UINT64>(pBuf);
        pInfo->remoteToken = remoteToken;
        ND2_SGE sge = {};
        sge.Buffer = pInfo;
        sge.BufferLength = sizeof(BenchPeerInfo);
        sge.MemoryRegionToken = localToken;
        printf("[BENCH-RECV] Calling Send...\n"); fflush(stdout);
        hr = ctx.pQp->Send((void*)0x5001, &sge, 1, 0);
        printf("[BENCH-RECV] Send returned 0x%08X\n", hr); fflush(stdout);
        if (FAILED(hr)) { printf("Send peer info: 0x%08X\n", hr); ret = -1; break; }

        ND2_RESULT result = {};
        while (ctx.pCq->GetResults(&result, 1) == 0) {}
        if (result.Status != ND_SUCCESS) {
            printf("Send peer info completion: 0x%08X\n", result.Status);
            ret = -1; break;
        }

        printf("Remote memory exported (token=0x%08X, addr=0x%llX). Waiting for RDMA writes...\n",
               pInfo->remoteToken, pInfo->remoteAddress);
        fflush(stdout);

        // Wait for sender to finish (sender sends a zero-length terminate message)
        hr = ctx.pQp->Receive((void*)0x5002, nullptr, 0);
        if (FAILED(hr)) { printf("Post terminate recv: 0x%08X\n", hr); ret = -1; break; }

        while (ctx.pCq->GetResults(&result, 1) == 0) {}
        if (result.Status == ND_SUCCESS) {
            printf("Bench complete.\n");
        }
    } while (0);

done:
    if (pMw != nullptr) { pMw->Release(); pMw = nullptr; }
    FreeAligned(pBuf);
    CleanupNd(ctx);   // must release ND objects before NdCleanup
    NdCleanup();
    WSACleanup();
    return ret;
}

static int RunBenchSender(const wchar_t* remoteIp, bool isRead)
{
    int ret = 0;
    NdContext ctx;
    void* pBuf = nullptr;

    printf("[BENCH-SEND] Starting...\n");
    fflush(stdout);

    WSADATA wsaData;
    int wsaRet = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaRet != 0) { printf("WSAStartup failed: %d\n", wsaRet); return -1; }
    printf("[BENCH-SEND] WSAStartup OK\n"); fflush(stdout);

    HRESULT hr = NdStartup();
    if (FAILED(hr)) { printf("NdStartup failed: 0x%08X\n", hr); ret = -1; goto done; }
    printf("[BENCH-SEND] NdStartup OK\n"); fflush(stdout);

    do
    {
        struct sockaddr_in remoteAddr = {};
        if (ParseIpv4(remoteIp, DEFAULT_PORT, &remoteAddr) != 0) { ret = -1; break; }

        struct sockaddr_in localAddr = {};
        SIZE_T addrLen = sizeof(localAddr);
        hr = NdResolveAddress(
            reinterpret_cast<const struct sockaddr*>(&remoteAddr), sizeof(remoteAddr),
            reinterpret_cast<struct sockaddr*>(&localAddr), &addrLen);
        if (FAILED(hr)) { printf("NdResolveAddress: 0x%08X\n", hr); ret = -1; break; }
        printf("[BENCH-SEND] NdResolveAddress OK\n"); fflush(stdout);

        hr = NdOpenAdapter(IID_IND2Adapter,
            reinterpret_cast<const struct sockaddr*>(&localAddr), sizeof(localAddr),
            reinterpret_cast<void**>(&ctx.pAdapter));
        if (FAILED(hr)) { printf("NdOpenAdapter: 0x%08X\n", hr); ret = -1; break; }
        printf("[BENCH-SEND] NdOpenAdapter OK\n"); fflush(stdout);

        ctx.ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        printf("[BENCH-SEND] CreateEvent OK\n"); fflush(stdout);
        hr = ctx.pAdapter->CreateOverlappedFile(&ctx.hOvFile);
        if (FAILED(hr)) { printf("CreateOverlappedFile: 0x%08X\n", hr); ret = -1; break; }
        printf("[BENCH-SEND] CreateOverlappedFile OK\n"); fflush(stdout);

        hr = ctx.pAdapter->CreateCompletionQueue(IID_IND2CompletionQueue,
            ctx.hOvFile, CQ_DEPTH, 0, 0, reinterpret_cast<void**>(&ctx.pCq));
        if (FAILED(hr)) { printf("CreateCQ: 0x%08X\n", hr); ret = -1; break; }
        printf("[BENCH-SEND] CreateCQ OK\n"); fflush(stdout);

        hr = ctx.pAdapter->CreateConnector(IID_IND2Connector,
            ctx.hOvFile, reinterpret_cast<void**>(&ctx.pConnector));
        if (FAILED(hr)) { printf("CreateConnector: 0x%08X\n", hr); ret = -1; break; }
        printf("[BENCH-SEND] CreateConnector OK\n"); fflush(stdout);

        hr = ctx.pAdapter->CreateQueuePair(IID_IND2QueuePair,
            ctx.pCq, ctx.pCq, nullptr, QP_DEPTH, QP_DEPTH,
            1, 1, 0, reinterpret_cast<void**>(&ctx.pQp));
        if (FAILED(hr)) { printf("CreateQP: 0x%08X\n", hr); ret = -1; break; }
        printf("[BENCH-SEND] CreateQP OK\n"); fflush(stdout);

        hr = ctx.pAdapter->CreateMemoryRegion(IID_IND2MemoryRegion,
            ctx.hOvFile, reinterpret_cast<void**>(&ctx.pMr));
        if (FAILED(hr)) { printf("CreateMR: 0x%08X\n", hr); ret = -1; break; }
        printf("[BENCH-SEND] CreateMR OK\n"); fflush(stdout);

        const DWORD benchBufSize = 512 * 1024 * 1024;  // 512 MB
        pBuf = AllocAligned(benchBufSize);
        if (!pBuf) { ret = -1; break; }

        hr = ctx.pMr->Register(pBuf, benchBufSize,
            ND_MR_FLAG_ALLOW_LOCAL_WRITE |
            (isRead ? ND_MR_FLAG_RDMA_READ_SINK : 0), &ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pMr, &ctx.ov);
        if (FAILED(hr)) { printf("Register: 0x%08X\n", hr); ret = -1; break; }
        printf("[BENCH-SEND] Register OK\n"); fflush(stdout);
        UINT32 localToken = ctx.pMr->GetLocalToken();

        // Connect
        printf("[BENCH-SEND] Binding connector...\n"); fflush(stdout);
        hr = ctx.pConnector->Bind(
            reinterpret_cast<const struct sockaddr*>(&localAddr), sizeof(localAddr));
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        if (FAILED(hr)) { printf("Bind: 0x%08X\n", hr); ret = -1; break; }
        printf("[BENCH-SEND] Bind OK\n"); fflush(stdout);

        printf("[BENCH-SEND] Connecting to %ls...\n", remoteIp); fflush(stdout);
        hr = ctx.pConnector->Connect(ctx.pQp,
            reinterpret_cast<const struct sockaddr*>(&remoteAddr), sizeof(remoteAddr),
            0, isRead ? 16 : 0, nullptr, 0, &ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        if (FAILED(hr)) { printf("Connect: 0x%08X\n", hr); ret = -1; break; }
        printf("[BENCH-SEND] Connect OK\n"); fflush(stdout);

        // Post receive BEFORE CompleteConnect: the receiver sends peer info
        // immediately after Accept which triggers CompleteConnect completion.
        BenchPeerInfo* pPeer = static_cast<BenchPeerInfo*>(pBuf);
        ND2_SGE sge = {};
        sge.Buffer = pPeer;
        sge.BufferLength = sizeof(BenchPeerInfo);
        sge.MemoryRegionToken = localToken;
        hr = ctx.pQp->Receive((void*)0x5001, &sge, 1);
        if (FAILED(hr)) { printf("Post recv peer info: 0x%08X\n", hr); ret = -1; break; }
        printf("[BENCH-SEND] Pre-posted recv OK\n"); fflush(stdout);

        hr = ctx.pConnector->CompleteConnect(&ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        if (FAILED(hr)) { printf("CompleteConnect: 0x%08X\n", hr); ret = -1; break; }
        printf("[BENCH-SEND] CompleteConnect OK\n"); fflush(stdout);

        {
            ND2_ADAPTER_INFO info = {};
            info.InfoVersion = ND_VERSION_2;
            ULONG infoSize = sizeof(info);
            hr = ctx.pAdapter->Query(&info, &infoSize);
            if (SUCCEEDED(hr)) {
                printf("[ADAPTER] MaxXfer=%luMB Flags=0x%08X InOrder=%d MultiEng=%d Loopback=%d\n",
                    info.MaxTransferLength/(1024*1024), info.AdapterFlags,
                    !!(info.AdapterFlags & ND_ADAPTER_FLAG_IN_ORDER_DMA_SUPPORTED),
                    !!(info.AdapterFlags & ND_ADAPTER_FLAG_MULTI_ENGINE_SUPPORTED),
                    !!(info.AdapterFlags & ND_ADAPTER_FLAG_LOOPBACK_CONNECTIONS_SUPPORTED));
                printf("[ADAPTER] InitQDepth=%lu RecvQDepth=%lu CQDepth=%lu\n",
                    info.MaxInitiatorQueueDepth, info.MaxReceiveQueueDepth, info.MaxCompletionQueueDepth);
            }
        }

        printf("[BENCH-SEND] Polling CQ for peer info result...\n"); fflush(stdout);
        ND2_RESULT result = {};
        while (ctx.pCq->GetResults(&result, 1) == 0) {}
        printf("[BENCH-SEND] Got result: status=0x%08X\n", result.Status); fflush(stdout);
        if (result.Status != ND_SUCCESS) {
            printf("Recv peer info: 0x%08X\n", result.Status); ret = -1; break;
        }

        printf("Remote memory: token=0x%08X, addr=0x%llX\n",
               pPeer->remoteToken, pPeer->remoteAddress);

        // RDMA Write/Read benchmark — single giant transfer for maximum throughput.
        // Note: RDMA Read via MW not functional on ConnectX-3 Pro (mlx4).
        const DWORD TOTAL_BYTES = benchBufSize;
        DWORD xferSize = TOTAL_BYTES;

        sge.Buffer = pBuf;
        sge.BufferLength = xferSize;

        LARGE_INTEGER freq, tStart, tEnd;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&tStart);

        // One giant RDMA Write or Read
        const char* opName = isRead ? "Read" : "Write";
        if (isRead) {
            hr = ctx.pQp->Read((void*)0x6001, &sge, 1,
                pPeer->remoteAddress, pPeer->remoteToken, 0);
        } else {
            hr = ctx.pQp->Write((void*)0x6001, &sge, 1,
                pPeer->remoteAddress, pPeer->remoteToken, 0);
        }
        if (FAILED(hr)) {
            printf("Giant %s failed: 0x%08X\n", opName, hr); ret = -1;
        } else {
            // Use Notify-based wait like ndrping's blocking mode
            while (ctx.pCq->GetResults(&result, 1) == 0) {
                ctx.pCq->Notify(ND_CQ_NOTIFY_ANY, &ctx.ov);
                ctx.pCq->GetOverlappedResult(&ctx.ov, TRUE);
            }
            if (result.Status != ND_SUCCESS) {
                printf("%s completion error: 0x%08X\n", opName, result.Status);
                ret = -1;
            }
        }

        QueryPerformanceCounter(&tEnd);
        double elapsed = static_cast<double>(tEnd.QuadPart - tStart.QuadPart) /
                         static_cast<double>(freq.QuadPart);
        double totalMb = static_cast<double>(TOTAL_BYTES) / (1024.0 * 1024.0);
        double speedMBps = elapsed > 0.0 ? (totalMb / elapsed) : 0.0;
        double speedGbps = speedMBps * 8.0 / 1000.0;
        double speedGBps = speedMBps / 1000.0;

        printf("\n=== %s Benchmark (1 giant %s) ===\n",
               isRead ? "RDMA Read" : "RDMA Write",
               isRead ? "read" : "write");
        printf("  Total:       %.2f MB\n", totalMb);
        printf("  %s:       %.2f MB x 1\n",
               isRead ? "Read" : "Write",
               (double)xferSize / (1024.0 * 1024.0));
        printf("  Time:        %.2f ms\n", elapsed * 1000.0);
        printf("  Throughput:  %.2f MB/s  (%.2f Gbps, %.2f GB/s)\n",
               speedMBps, speedGbps, speedGBps);

        // Send terminate (zero-length send)
        hr = ctx.pQp->Send((void*)0x5002, nullptr, 0, 0);
        if (FAILED(hr)) { printf("Send terminate: 0x%08X\n", hr); }
        while (ctx.pCq->GetResults(&result, 1) == 0) {}
    } while (0);

done:
    FreeAligned(pBuf);
    CleanupNd(ctx);
    NdCleanup();
    WSACleanup();
    return ret;
}

int wmain(int argc, wchar_t* argv[])
{
    bool isSend = false;
    bool isRecv = false;
    bool isBench = false;
    const wchar_t* ip = nullptr;
    const wchar_t* path = nullptr;

    for (int i = 1; i < argc; i++)
    {
        if (wcscmp(argv[i], L"-send") == 0)
        {
            isSend = true;
        }
        else if (wcscmp(argv[i], L"-recv") == 0)
        {
            isRecv = true;
        }
        else if (wcscmp(argv[i], L"-bench") == 0)
        {
            isBench = true;
        }
        else if (wcscmp(argv[i], L"-read") == 0)
        {
            g_isRead = true;
        }
        else if (wcscmp(argv[i], L"-ip") == 0)
        {
            if (i + 1 >= argc) { PrintUsage(argv[0]); return -1; }
            ip = argv[++i];
        }
        else if (wcscmp(argv[i], L"-file") == 0)
        {
            if (i + 1 >= argc) { PrintUsage(argv[0]); return -1; }
            path = argv[++i];
        }
        else if (wcscmp(argv[i], L"-o") == 0)
        {
            if (i + 1 >= argc) { PrintUsage(argv[0]); return -1; }
            path = argv[++i];
        }
        else
        {
            printf("Unknown argument: %ls\n", argv[i]);
            PrintUsage(argv[0]);
            return -1;
        }
    }

    if (isBench)
    {
        if (ip == nullptr) { PrintUsage(argv[0]); return -1; }
        if (isSend) return RunBenchSender(ip, g_isRead);
        if (isRecv) return RunBenchReceiver(ip);
        PrintUsage(argv[0]);
        return -1;
    }

    if ((isSend == isRecv) || (ip == nullptr) || (path == nullptr))
    {
        PrintUsage(argv[0]);
        return -1;
    }

    if (isSend)
    {
        return RunSender(ip, path);
    }
    else
    {
        return RunReceiver(ip, path);
    }
}
