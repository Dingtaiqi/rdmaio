// rdma_transfer.cpp — DLL implementation of RDMA file transfer over NDSPI
//
// Build: /D RDMA_TRANSFER_EXPORTS to export symbols

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
#include <string>

#define RDMA_TRANSFER_EXPORTS
#include "rdma_transfer.h"

#pragma comment(lib, "Ws2_32.lib")

static const DWORD CHUNK_SIZE   = 4 * 1024 * 1024;
static const DWORD CQ_DEPTH     = 256;
static const DWORD QP_DEPTH     = 128;
static const DWORD ALIGNMENT    = 4096;

#define META_SEND_CTX   ((void*)0x1001)
#define META_RECV_CTX   ((void*)0x1002)
#define CHUNK_SEND_CTX  ((void*)0x2001)
#define CHUNK_RECV_CTX  ((void*)0x2002)
#define TERM_SEND_CTX   ((void*)0x3001)
#define TERM_RECV_CTX   ((void*)0x3002)
#define PEER_SEND_CTX   ((void*)0x4001)
#define PEER_RECV_CTX   ((void*)0x4002)
#define BENCH_WRITE_CTX ((void*)0x6001)
#define TERM_CMD_ABORT  0xDEADBEEFUL

#pragma pack(push, 1)
struct FileMeta {
    uint64_t file_size;
    uint32_t filename_len;
    char     filename[256];
};
#pragma pack(pop)

struct TerminateCmd  { uint32_t cmd; };
struct BenchPeerInfo { UINT64 remoteAddress; UINT32 remoteToken; };

struct NdContext {
    IND2Adapter*         pAdapter   = nullptr;
    HANDLE               hOvFile    = INVALID_HANDLE_VALUE;
    IND2CompletionQueue* pCq        = nullptr;
    IND2MemoryRegion*    pMr        = nullptr;
    IND2QueuePair*       pQp        = nullptr;
    IND2Connector*       pConnector = nullptr;
    IND2Listener*        pListener  = nullptr;
    OVERLAPPED           ov         = {};
};

// ---- Globals ------------------------------------------------------------
static rdma_progress_cb  g_progress_cb  = nullptr;
static void*             g_progress_ctx = nullptr;
static rdma_metadata_cb  g_metadata_cb  = nullptr;
static void*             g_metadata_ctx = nullptr;
static volatile LONG     g_cancel_flag  = 0;
static LONG              g_init_count   = 0;
static char              g_last_error[256] = "";

#define SET_ERROR(msg) strcpy_s(g_last_error, sizeof(g_last_error), msg)

// ---- Helpers ------------------------------------------------------------
static void* AllocAligned(size_t sz) {
    void* p = _aligned_malloc(sz, ALIGNMENT);
    if (p) memset(p, 0, sz);
    return p;
}
static void FreeAligned(void* p) { if (p) _aligned_free(p); }

static HRESULT WaitOverlapped(IND2Overlapped* pObj, OVERLAPPED* pOv) {
    return pObj->GetOverlappedResult(pOv, TRUE);
}

static void CleanupNd(NdContext& ctx) {
    HRESULT hr;
    if (ctx.pMr)       { hr = ctx.pMr->Deregister(&ctx.ov);
                         if (hr == ND_PENDING) ctx.pMr->GetOverlappedResult(&ctx.ov, TRUE); }
    if (ctx.pConnector){ hr = ctx.pConnector->Disconnect(&ctx.ov);
                         if (hr == ND_PENDING) ctx.pConnector->GetOverlappedResult(&ctx.ov, TRUE); }
    if (ctx.pQp)       { ctx.pQp->Release();       ctx.pQp       = nullptr; }
    if (ctx.pConnector){ ctx.pConnector->Release();ctx.pConnector= nullptr; }
    if (ctx.pMr)       { ctx.pMr->Release();       ctx.pMr       = nullptr; }
    // Flush providers to ensure the kernel ND driver has fully released
    // hardware QP resources before a new connection is made.
    NdFlushProviders();
    if (ctx.pCq)       { ctx.pCq->Release();       ctx.pCq       = nullptr; }
    if (ctx.pListener) { ctx.pListener->Release(); ctx.pListener  = nullptr; }
    if (ctx.hOvFile != INVALID_HANDLE_VALUE)       { CloseHandle(ctx.hOvFile); ctx.hOvFile = INVALID_HANDLE_VALUE; }
    if (ctx.pAdapter)  { ctx.pAdapter->Release();  ctx.pAdapter  = nullptr; }
    if (ctx.ov.hEvent) { CloseHandle(ctx.ov.hEvent); ctx.ov.hEvent= nullptr; }
}

static int ParseIpv4(const char* ipStr, USHORT port, struct sockaddr_in* pAddr) {
    memset(pAddr, 0, sizeof(*pAddr));
    pAddr->sin_family = AF_INET;
    pAddr->sin_port   = htons(port);
    if (InetPtonA(AF_INET, ipStr, &pAddr->sin_addr) != 1) return -1;
    return 0;
}

static int ParseIpv4W(const wchar_t* ipStr, USHORT port, struct sockaddr_in* pAddr) {
    memset(pAddr, 0, sizeof(*pAddr));
    pAddr->sin_family = AF_INET;
    pAddr->sin_port   = htons(port);
    if (InetPtonW(AF_INET, ipStr, &pAddr->sin_addr) != 1) return -1;
    return 0;
}

static void FireProgress(uint64_t bytes, uint64_t total, double elapsed) {
    if (!g_progress_cb) return;
    double pct  = total ? (100.0 * (double)bytes / (double)total) : 0.0;
    double mb   = (double)bytes / (1024.0 * 1024.0);
    double spd  = elapsed > 0.0 ? (mb / elapsed) : 0.0;
    g_progress_cb(pct, spd, g_progress_ctx);
}

static void FireMetadata(const char* filename, uint64_t file_size) {
    if (!g_metadata_cb) return;
    g_metadata_cb(filename, file_size, g_metadata_ctx);
}

static int IsCancelled(void) {
    return InterlockedCompareExchange(&g_cancel_flag, 0, 0);
}
static void ResetCancel(void) {
    InterlockedExchange(&g_cancel_flag, 0);
}

static void SendAbortSignal(NdContext& ctx, ND2_SGE& sge, TerminateCmd* pTerm) {
    pTerm->cmd = TERM_CMD_ABORT;
    sge.Buffer = pTerm;
    sge.BufferLength = sizeof(TerminateCmd);
    HRESULT hr = ctx.pQp->Send(TERM_SEND_CTX, &sge, 1, 0);
    if (FAILED(hr)) return;
    ND2_RESULT r = {};
    while (ctx.pCq->GetResults(&r, 1) == 0 && !IsCancelled()) {}
}

static void ExtractFileName(const wchar_t* path, char* out, uint32_t* pLen, size_t maxLen) {
    const wchar_t* name = wcsrchr(path, L'\\');
    if (!name) name = wcsrchr(path, L'/');
    if (!name) name = path; else name++;
    int n = WideCharToMultiByte(CP_ACP, 0, name, -1, out, (int)maxLen, nullptr, nullptr);
    *pLen = n > 0 ? (uint32_t)(n - 1) : 0;
}

// ===================================================================
// Internal transfer functions
// ===================================================================
static int InternalSend(const char* remoteIp, USHORT port, const wchar_t* filePath) {
    int ret = 0; NdContext ctx; HANDLE hFile = INVALID_HANDLE_VALUE; void* pBuf = nullptr;
    ResetCancel();

    struct sockaddr_in remoteAddr = {};
    if (ParseIpv4(remoteIp, port, &remoteAddr) != 0) return -1;

    struct sockaddr_in localAddr = {};
    SIZE_T addrLen = sizeof(localAddr);
    HRESULT hr = NdResolveAddress((const sockaddr*)&remoteAddr, sizeof(remoteAddr),
                                   (sockaddr*)&localAddr, &addrLen);
    if (FAILED(hr)) return -1;

    hr = NdOpenAdapter(IID_IND2Adapter, (const sockaddr*)&localAddr, sizeof(localAddr),
                       (void**)&ctx.pAdapter);
    if (FAILED(hr)) return -1;

    ctx.ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!ctx.ov.hEvent) return -1;
    hr = ctx.pAdapter->CreateOverlappedFile(&ctx.hOvFile);
    if (FAILED(hr)) { CleanupNd(ctx); return -1; }

    hr = ctx.pAdapter->CreateCompletionQueue(IID_IND2CompletionQueue, ctx.hOvFile,
        CQ_DEPTH, 0, 0, (void**)&ctx.pCq);
    if (FAILED(hr)) { CleanupNd(ctx); return -1; }

    hr = ctx.pAdapter->CreateConnector(IID_IND2Connector, ctx.hOvFile, (void**)&ctx.pConnector);
    if (FAILED(hr)) { CleanupNd(ctx); return -1; }

    hr = ctx.pAdapter->CreateQueuePair(IID_IND2QueuePair, ctx.pCq, ctx.pCq, nullptr,
        QP_DEPTH, QP_DEPTH, 1, 1, 0, (void**)&ctx.pQp);
    if (FAILED(hr)) { CleanupNd(ctx); return -1; }

    hr = ctx.pAdapter->CreateMemoryRegion(IID_IND2MemoryRegion, ctx.hOvFile, (void**)&ctx.pMr);
    if (FAILED(hr)) { CleanupNd(ctx); return -1; }

    const size_t bufSize = (size_t)CHUNK_SIZE + 8192;
    pBuf = AllocAligned(bufSize);
    if (!pBuf) { CleanupNd(ctx); return -1; }

    hr = ctx.pMr->Register(pBuf, bufSize, ND_MR_FLAG_ALLOW_LOCAL_WRITE, &ctx.ov);
    if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pMr, &ctx.ov);
    if (FAILED(hr)) { SET_ERROR("Register buffer failed"); CleanupNd(ctx); return -1; }

    do {
        UINT32 localToken = ctx.pMr->GetLocalToken();

        hr = ctx.pConnector->Bind((const sockaddr*)&localAddr, sizeof(localAddr));
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        if (FAILED(hr)) { SET_ERROR("Bind failed"); ret = -1; break; }

        hr = ctx.pConnector->Connect(ctx.pQp, (const sockaddr*)&remoteAddr, sizeof(remoteAddr),
                                      0, 0, nullptr, 0, &ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        if (FAILED(hr)) { SET_ERROR("Connect failed"); ret = -1; break; }

        hr = ctx.pConnector->CompleteConnect(&ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        if (FAILED(hr)) { SET_ERROR("CompleteConnect failed"); ret = -1; break; }

        hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) { ret = -1; break; }

        LARGE_INTEGER fsLi;
        if (!GetFileSizeEx(hFile, &fsLi)) { SET_ERROR("GetFileSizeEx failed"); ret = -1; break; }
        const uint64_t fileSize = (uint64_t)fsLi.QuadPart;

        FileMeta*      pMeta  = (FileMeta*)pBuf;
        char*          pChunk = (char*)pBuf + ALIGNMENT;
        TerminateCmd*  pTerm  = (TerminateCmd*)(pChunk + CHUNK_SIZE);

        pMeta->file_size = fileSize;
        ExtractFileName(filePath, pMeta->filename, &pMeta->filename_len, sizeof(pMeta->filename));

        FireMetadata(pMeta->filename, fileSize);

        ND2_SGE sge = {}; sge.Buffer = pMeta; sge.BufferLength = sizeof(FileMeta);
        sge.MemoryRegionToken = localToken;
        hr = ctx.pQp->Send(META_SEND_CTX, &sge, 1, 0);
        if (FAILED(hr)) { SET_ERROR("Send metadata failed"); ret = -1; break; }

        ND2_RESULT result = {}; bool metaDone = false;
        while (!metaDone) {
            while (ctx.pCq->GetResults(&result, 1) == 0 && !IsCancelled()) {}
            if (IsCancelled()) { ret = -1; break; }
            if (result.Status != ND_SUCCESS) { ret = -1; break; }
            if (result.RequestContext == META_SEND_CTX) metaDone = true;
        }
        if (ret) break;

        sge.Buffer = pTerm; sge.BufferLength = sizeof(TerminateCmd);
        hr = ctx.pQp->Receive(TERM_RECV_CTX, &sge, 1);
        if (FAILED(hr)) { ret = -1; break; }

        uint64_t bytesSent = 0; bool aborted = false;
        LARGE_INTEGER freq, t0; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);

        while (bytesSent < fileSize && !aborted) {
            if (IsCancelled()) { ret = -1; aborted = true; break; }
            DWORD toRead = (fileSize - bytesSent > CHUNK_SIZE) ? CHUNK_SIZE : (DWORD)(fileSize - bytesSent);
            DWORD rb = 0;
            if (!ReadFile(hFile, pChunk, toRead, &rb, nullptr) || rb != toRead) { ret = -1; aborted = true; break; }

            sge.Buffer = pChunk; sge.BufferLength = toRead;
            hr = ctx.pQp->Send(CHUNK_SEND_CTX, &sge, 1, 0);
            if (FAILED(hr)) { SET_ERROR("Send chunk failed"); ret = -1; break; }

            bool sendDone = false;
            while (!sendDone && !aborted) {
                while (ctx.pCq->GetResults(&result, 1) == 0 && !IsCancelled()) {}
                if (IsCancelled()) { SET_ERROR("Cancelled by user"); ret = -1; break; }
                if (result.Status != ND_SUCCESS) { SET_ERROR("Chunk send CQ error"); ret = -1; break; }
                if (result.RequestContext == CHUNK_SEND_CTX)      { sendDone = true; bytesSent += toRead; }
                else if (result.RequestContext == TERM_RECV_CTX)  { if (pTerm->cmd == TERM_CMD_ABORT) { SET_ERROR("Receiver disk error"); aborted = true; ret = -1; } }
                else { SET_ERROR("Unexpected CQ context"); ret = -1; break; }
            }
            if (ret) break;

            LARGE_INTEGER tn; QueryPerformanceCounter(&tn);
            FireProgress(bytesSent, fileSize, (double)(tn.QuadPart - t0.QuadPart) / (double)freq.QuadPart);
        }

        LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
        FireProgress(bytesSent, fileSize, (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart);
    } while (0);

    if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    FreeAligned(pBuf);
    ResetCancel();
    CleanupNd(ctx);
    return ret;
}

static bool IsDirectory(const wchar_t* path) {
    DWORD attr = GetFileAttributesW(path);
    return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring BuildOutputPath(const wchar_t* outPath, const char* filename) {
    std::wstring result = outPath;
    if (!result.empty() && result.back() != L'\\' && result.back() != L'/') {
        result += L'\\';
    }

    int fnLen = MultiByteToWideChar(CP_ACP, 0, filename, -1, nullptr, 0);
    if (fnLen > 0) {
        std::wstring fn(fnLen - 1, L'\0');
        MultiByteToWideChar(CP_ACP, 0, filename, -1, fn.data(), fnLen);
        result += fn;
    }
    return result;
}

static int InternalRecv(const char* localIp, USHORT port, const wchar_t* outPath) {
    int ret = 0; NdContext ctx; HANDLE hFile = INVALID_HANDLE_VALUE; void* pBuf = nullptr;
    ResetCancel();
    const DWORD NUM_CHUNK_BUFS = QP_DEPTH - 1;

    struct sockaddr_in localAddr = {};
    if (ParseIpv4(localIp, port, &localAddr) != 0) return -1;

    HRESULT hr = NdOpenAdapter(IID_IND2Adapter, (const sockaddr*)&localAddr, sizeof(localAddr),
                                (void**)&ctx.pAdapter);
    if (FAILED(hr)) return -1;

    ctx.ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!ctx.ov.hEvent) return -1;
    hr = ctx.pAdapter->CreateOverlappedFile(&ctx.hOvFile);
    if (FAILED(hr)) { CleanupNd(ctx); return -1; }

    hr = ctx.pAdapter->CreateCompletionQueue(IID_IND2CompletionQueue, ctx.hOvFile,
        CQ_DEPTH, 0, 0, (void**)&ctx.pCq);
    if (FAILED(hr)) { CleanupNd(ctx); return -1; }

    hr = ctx.pAdapter->CreateConnector(IID_IND2Connector, ctx.hOvFile, (void**)&ctx.pConnector);
    if (FAILED(hr)) { CleanupNd(ctx); return -1; }

    hr = ctx.pAdapter->CreateQueuePair(IID_IND2QueuePair, ctx.pCq, ctx.pCq, nullptr,
        QP_DEPTH, QP_DEPTH, 1, 1, 0, (void**)&ctx.pQp);
    if (FAILED(hr)) { CleanupNd(ctx); return -1; }

    hr = ctx.pAdapter->CreateMemoryRegion(IID_IND2MemoryRegion, ctx.hOvFile, (void**)&ctx.pMr);
    if (FAILED(hr)) { CleanupNd(ctx); return -1; }

    const size_t bufSize = ALIGNMENT + (size_t)CHUNK_SIZE * NUM_CHUNK_BUFS + 8192;
    pBuf = AllocAligned(bufSize);
    if (!pBuf) { CleanupNd(ctx); return -1; }

    hr = ctx.pMr->Register(pBuf, bufSize, ND_MR_FLAG_ALLOW_LOCAL_WRITE, &ctx.ov);
    if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pMr, &ctx.ov);
    if (FAILED(hr)) { CleanupNd(ctx); return -1; }

    do {
        UINT32 localToken = ctx.pMr->GetLocalToken();

        hr = ctx.pAdapter->CreateListener(IID_IND2Listener, ctx.hOvFile, (void**)&ctx.pListener);
        if (FAILED(hr)) { ret = -1; break; }

        hr = ctx.pListener->Bind((const sockaddr*)&localAddr, sizeof(localAddr));
        if (FAILED(hr)) { ret = -1; break; }
        hr = ctx.pListener->Listen(1);
        if (FAILED(hr)) { ret = -1; break; }

        hr = ctx.pListener->GetConnectionRequest(ctx.pConnector, &ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pListener, &ctx.ov);
        if (FAILED(hr)) { ret = -1; break; }

        FileMeta* pMeta = (FileMeta*)pBuf;
        char* chunkBufs[128];
        for (DWORD i = 0; i < NUM_CHUNK_BUFS; i++)
            chunkBufs[i] = (char*)pBuf + ALIGNMENT + i * CHUNK_SIZE;
        TerminateCmd* pTerm = (TerminateCmd*)((char*)pBuf + ALIGNMENT + CHUNK_SIZE * NUM_CHUNK_BUFS);

        ND2_SGE sge_meta = {}; sge_meta.Buffer = pMeta;
        sge_meta.BufferLength = sizeof(FileMeta); sge_meta.MemoryRegionToken = localToken;

        ND2_SGE sge_chunk = {}; sge_chunk.MemoryRegionToken = localToken;

        hr = ctx.pQp->Receive(META_RECV_CTX, &sge_meta, 1);
        if (FAILED(hr)) { ret = -1; break; }

        for (DWORD i = 0; i < NUM_CHUNK_BUFS; i++) {
            sge_chunk.Buffer = chunkBufs[i]; sge_chunk.BufferLength = CHUNK_SIZE;
            hr = ctx.pQp->Receive(CHUNK_RECV_CTX, &sge_chunk, 1);
            if (FAILED(hr)) { ret = -1; break; }
        }
        if (ret) break;

        hr = ctx.pConnector->Accept(ctx.pQp, 0, 0, nullptr, 0, &ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        if (FAILED(hr)) { ret = -1; break; }

        ND2_RESULT result = {}; bool metaDone = false;
        while (!metaDone) {
            while (ctx.pCq->GetResults(&result, 1) == 0 && !IsCancelled()) {}
            if (IsCancelled()) { ret = -1; break; }
            if (result.Status != ND_SUCCESS) { ret = -1; break; }
            if (result.RequestContext == META_RECV_CTX) metaDone = true;
        }
        if (ret || result.BytesTransferred != sizeof(FileMeta)) { if (!ret) ret = -1; break; }

        FireMetadata(pMeta->filename, pMeta->file_size);

        std::wstring finalPath = IsDirectory(outPath)
            ? BuildOutputPath(outPath, pMeta->filename)
            : std::wstring(outPath);

        hFile = CreateFileW(finalPath.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            sge_chunk.Buffer = pTerm; sge_chunk.BufferLength = sizeof(TerminateCmd);
            SendAbortSignal(ctx, sge_chunk, pTerm);
            ret = -1; break;
        }

        const uint64_t fileSize = pMeta->file_size;
        uint64_t bytesReceived = 0; DWORD ringIdx = 0; bool diskError = false;
        LARGE_INTEGER freq, t0; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);

        while (bytesReceived < fileSize && !diskError) {
            if (IsCancelled()) { ret = -1; diskError = true; break; }
            while (ctx.pCq->GetResults(&result, 1) == 0 && !IsCancelled()) {}
            if (IsCancelled()) { ret = -1; diskError = true; break; }
            if (result.Status != ND_SUCCESS || result.RequestContext != CHUNK_RECV_CTX) {
                ret = -1; diskError = true; break;
            }
            DWORD received = result.BytesTransferred;

            if (bytesReceived + received < fileSize) {
                DWORD nextExpect = (fileSize - (bytesReceived + received) > CHUNK_SIZE)
                    ? CHUNK_SIZE : (DWORD)(fileSize - (bytesReceived + received));
                sge_chunk.Buffer = chunkBufs[ringIdx];
                sge_chunk.BufferLength = nextExpect;
                hr = ctx.pQp->Receive(CHUNK_RECV_CTX, &sge_chunk, 1);
                if (FAILED(hr)) { ret = -1; diskError = true; break; }
            }

            DWORD wb = 0;
            if (!WriteFile(hFile, chunkBufs[ringIdx], received, &wb, nullptr) || wb != received) {
                ret = -1; diskError = true; break;
            }
            bytesReceived += received;
            ringIdx = (ringIdx + 1) % NUM_CHUNK_BUFS;

            LARGE_INTEGER tn; QueryPerformanceCounter(&tn);
            FireProgress(bytesReceived, fileSize, (double)(tn.QuadPart - t0.QuadPart) / (double)freq.QuadPart);
        }

        if (diskError) {
            sge_chunk.Buffer = pTerm; sge_chunk.BufferLength = sizeof(TerminateCmd);
            SendAbortSignal(ctx, sge_chunk, pTerm);
        }

        LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
        FireProgress(bytesReceived, fileSize, (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart);
    } while (0);

    if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    FreeAligned(pBuf);
    ResetCancel();
    CleanupNd(ctx);
    return ret;
}

static int InternalBenchRecv(const char* localIp, USHORT port, int sizeMb) {
    int ret = 0; NdContext ctx; void* pBuf = nullptr;
    ResetCancel();
    struct sockaddr_in localAddr = {};
    if (ParseIpv4(localIp, port, &localAddr) != 0) return -1;

    HRESULT hr = NdOpenAdapter(IID_IND2Adapter, (const sockaddr*)&localAddr, sizeof(localAddr),
                                (void**)&ctx.pAdapter);
    if (FAILED(hr)) return -1;

    ctx.ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    hr = ctx.pAdapter->CreateOverlappedFile(&ctx.hOvFile);
    if (FAILED(hr)) { CleanupNd(ctx); return -1; }

    hr = ctx.pAdapter->CreateCompletionQueue(IID_IND2CompletionQueue, ctx.hOvFile,
        CQ_DEPTH, 0, 0, (void**)&ctx.pCq);
    if (FAILED(hr)) { CleanupNd(ctx); return -1; }

    hr = ctx.pAdapter->CreateConnector(IID_IND2Connector, ctx.hOvFile, (void**)&ctx.pConnector);
    if (FAILED(hr)) { CleanupNd(ctx); return -1; }

    hr = ctx.pAdapter->CreateQueuePair(IID_IND2QueuePair, ctx.pCq, ctx.pCq, nullptr,
        QP_DEPTH, QP_DEPTH, 1, 1, 0, (void**)&ctx.pQp);
    if (FAILED(hr)) { CleanupNd(ctx); return -1; }

    hr = ctx.pAdapter->CreateMemoryRegion(IID_IND2MemoryRegion, ctx.hOvFile, (void**)&ctx.pMr);
    if (FAILED(hr)) { CleanupNd(ctx); return -1; }

    DWORD benchBufSize = (DWORD)sizeMb * 1024 * 1024;
    pBuf = AllocAligned(benchBufSize);
    if (!pBuf) { CleanupNd(ctx); return -1; }
    memset(pBuf, 0xAB, benchBufSize);

    hr = ctx.pMr->Register(pBuf, benchBufSize,
        ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE, &ctx.ov);
    if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pMr, &ctx.ov);
    if (FAILED(hr)) { CleanupNd(ctx); FreeAligned(pBuf); return -1; }

    do {
        hr = ctx.pAdapter->CreateListener(IID_IND2Listener, ctx.hOvFile, (void**)&ctx.pListener);
        if (FAILED(hr)) { ret = -1; break; }
        hr = ctx.pListener->Bind((const sockaddr*)&localAddr, sizeof(localAddr));
        if (FAILED(hr)) { ret = -1; break; }
        hr = ctx.pListener->Listen(1);
        if (FAILED(hr)) { ret = -1; break; }

        hr = ctx.pListener->GetConnectionRequest(ctx.pConnector, &ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pListener, &ctx.ov);
        if (FAILED(hr)) { ret = -1; break; }

        hr = ctx.pConnector->Accept(ctx.pQp, 0, 0, nullptr, 0, &ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        if (FAILED(hr)) { ret = -1; break; }

        // Send our memory info to sender
        BenchPeerInfo* pInfo = (BenchPeerInfo*)pBuf;
        pInfo->remoteAddress = (UINT64)pBuf;
        pInfo->remoteToken   = ctx.pMr->GetRemoteToken();

        ND2_SGE sge = {}; sge.Buffer = pInfo; sge.BufferLength = sizeof(BenchPeerInfo);
        sge.MemoryRegionToken = ctx.pMr->GetLocalToken();
        hr = ctx.pQp->Send(PEER_SEND_CTX, &sge, 1, 0);
        if (FAILED(hr)) { ret = -1; break; }

        ND2_RESULT result = {};
        while (ctx.pCq->GetResults(&result, 1) == 0 && !IsCancelled()) {}
        if (result.Status != ND_SUCCESS) { ret = -1; break; }

        // Wait for sender's terminate message
        hr = ctx.pQp->Receive((void*)0x5002, nullptr, 0);
        if (FAILED(hr)) { ret = -1; break; }
        while (ctx.pCq->GetResults(&result, 1) == 0 && !IsCancelled()) {}
    } while (0);
    FreeAligned(pBuf);
    ResetCancel();
    CleanupNd(ctx);
    return ret;
}

static int InternalBenchSend(const char* remoteIp, USHORT port, int sizeMb) {
    int ret = 0; NdContext ctx; void* pBuf = nullptr;
    ResetCancel();

    struct sockaddr_in remoteAddr = {};
    if (ParseIpv4(remoteIp, port, &remoteAddr) != 0) return -1;

    struct sockaddr_in localAddr = {};
    SIZE_T addrLen = sizeof(localAddr);
    HRESULT hr = NdResolveAddress((const sockaddr*)&remoteAddr, sizeof(remoteAddr),
                                   (sockaddr*)&localAddr, &addrLen);
    if (FAILED(hr)) return -1;

    hr = NdOpenAdapter(IID_IND2Adapter, (const sockaddr*)&localAddr, sizeof(localAddr),
                        (void**)&ctx.pAdapter);
    if (FAILED(hr)) return -1;

    ctx.ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    hr = ctx.pAdapter->CreateOverlappedFile(&ctx.hOvFile);
    if (FAILED(hr)) { CleanupNd(ctx); return -1; }

    hr = ctx.pAdapter->CreateCompletionQueue(IID_IND2CompletionQueue, ctx.hOvFile,
        CQ_DEPTH, 0, 0, (void**)&ctx.pCq);
    if (FAILED(hr)) { CleanupNd(ctx); return -1; }

    hr = ctx.pAdapter->CreateConnector(IID_IND2Connector, ctx.hOvFile, (void**)&ctx.pConnector);
    if (FAILED(hr)) { CleanupNd(ctx); return -1; }

    hr = ctx.pAdapter->CreateQueuePair(IID_IND2QueuePair, ctx.pCq, ctx.pCq, nullptr,
        QP_DEPTH, QP_DEPTH, 1, 1, 0, (void**)&ctx.pQp);
    if (FAILED(hr)) { CleanupNd(ctx); return -1; }

    hr = ctx.pAdapter->CreateMemoryRegion(IID_IND2MemoryRegion, ctx.hOvFile, (void**)&ctx.pMr);
    if (FAILED(hr)) { CleanupNd(ctx); return -1; }

    DWORD benchBufSize = (DWORD)sizeMb * 1024 * 1024;
    pBuf = AllocAligned(benchBufSize);
    if (!pBuf) { CleanupNd(ctx); return -1; }

    hr = ctx.pMr->Register(pBuf, benchBufSize, ND_MR_FLAG_ALLOW_LOCAL_WRITE, &ctx.ov);
    if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pMr, &ctx.ov);
    if (FAILED(hr)) { CleanupNd(ctx); FreeAligned(pBuf); return -1; }

    do {
        UINT32 localToken = ctx.pMr->GetLocalToken();

        hr = ctx.pConnector->Bind((const sockaddr*)&localAddr, sizeof(localAddr));
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        if (FAILED(hr)) { ret = -1; break; }

        hr = ctx.pConnector->Connect(ctx.pQp, (const sockaddr*)&remoteAddr, sizeof(remoteAddr),
                                      0, 0, nullptr, 0, &ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        if (FAILED(hr)) { ret = -1; break; }

        // Post receive for peer info BEFORE CompleteConnect
        BenchPeerInfo* pPeer = (BenchPeerInfo*)pBuf;
        ND2_SGE sge = {}; sge.Buffer = pPeer;
        sge.BufferLength = sizeof(BenchPeerInfo); sge.MemoryRegionToken = localToken;
        hr = ctx.pQp->Receive(PEER_RECV_CTX, &sge, 1);
        if (FAILED(hr)) { ret = -1; break; }

        hr = ctx.pConnector->CompleteConnect(&ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        if (FAILED(hr)) { ret = -1; break; }

        // Receive peer info from receiver (its memory token + address)
        ND2_RESULT result = {};
        while (ctx.pCq->GetResults(&result, 1) == 0 && !IsCancelled()) {}
        if (result.Status != ND_SUCCESS) { ret = -1; break; }
        // pPeer now holds receiver's memory info

        // RDMA Write benchmark
        const DWORD XFER_SIZE = 4 * 1024 * 1024;
        const DWORD writes = benchBufSize / XFER_SIZE;

        sge.Buffer = pBuf; sge.BufferLength = XFER_SIZE;
        sge.MemoryRegionToken = localToken;

        LARGE_INTEGER freq, t0, t1;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);

        for (DWORD i = 0; i < writes; i++) {
            if (IsCancelled()) { ret = -1; break; }
            UINT64 offset = (UINT64)i * XFER_SIZE;
            DWORD flags = (i == writes - 1) ? 0 : ND_OP_FLAG_SILENT_SUCCESS;
            hr = ctx.pQp->Write(BENCH_WRITE_CTX, &sge, 1,
                pPeer->remoteAddress + offset, pPeer->remoteToken, flags);
            if (FAILED(hr)) { ret = -1; break; }
        }

        if (ret == 0) {
            while (ctx.pCq->GetResults(&result, 1) == 0 && !IsCancelled()) {}
            if (result.Status != ND_SUCCESS) ret = -1;
        }

        QueryPerformanceCounter(&t1);
        double elapsed = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        double totalMb = (double)benchBufSize / (1024.0 * 1024.0);
        double mbps = elapsed > 0.0 ? (totalMb / elapsed) : 0.0;

        printf("  RDMA Write: %.0f MB in %.1f ms  |  %.1f MB/s  (%.2f Gbps)\n",
               totalMb, elapsed * 1000.0, mbps, mbps * 8.0 / 1000.0);

        FireProgress(benchBufSize, benchBufSize, elapsed);

        // Send terminate
        hr = ctx.pQp->Send((void*)0x5002, nullptr, 0, 0);
        if (SUCCEEDED(hr)) while (ctx.pCq->GetResults(&result, 1) == 0 && !IsCancelled()) {}
    } while (0);

    FreeAligned(pBuf);
    ResetCancel();
    CleanupNd(ctx);
    return ret;
}

// ===================================================================
// Adapter enumeration
// ===================================================================
RDMA_TRANSFER_API int rdma_list_adapters(rdma_adapter_info* out, int max_count)
{
    int count = 0;

    // Get address list size first
    SIZE_T len = 0;
    HRESULT hr = NdQueryAddressList(ND_QUERY_EXCLUDE_EMULATOR_ADDRESSES, nullptr, &len);
    if (hr != ND_BUFFER_OVERFLOW || len == 0) return -1;

    SOCKET_ADDRESS_LIST* pList = (SOCKET_ADDRESS_LIST*)HeapAlloc(GetProcessHeap(), 0, len);
    if (!pList) return -1;

    hr = NdQueryAddressList(ND_QUERY_EXCLUDE_EMULATOR_ADDRESSES, pList, &len);
    if (FAILED(hr)) { HeapFree(GetProcessHeap(), 0, pList); return -1; }

    char seen_ips[32][64]; int seen_count = 0;
    for (int i = 0; i < pList->iAddressCount && (out == NULL || count < max_count); i++)
    {
        sockaddr_in* pAddr = (sockaddr_in*)pList->Address[i].lpSockaddr;
        if (pAddr->sin_family != AF_INET) continue;

        char ipStr[64] = {};
        InetNtopA(AF_INET, &pAddr->sin_addr, ipStr, sizeof(ipStr));

        // Deduplicate by IP
        bool dup = false;
        for (int k = 0; k < seen_count; k++) {
            if (strcmp(seen_ips[k], ipStr) == 0) { dup = true; break; }
        }
        if (dup) continue;
        if (seen_count < 32) strcpy_s(seen_ips[seen_count++], sizeof(seen_ips[0]), ipStr);

        IND2Adapter* pAdapter = nullptr;
        hr = NdOpenAdapter(IID_IND2Adapter,
            (const sockaddr*)pAddr, sizeof(*pAddr), (void**)&pAdapter);
        if (FAILED(hr)) continue;

        ND2_ADAPTER_INFO ai = {}; ai.InfoVersion = ND_VERSION_2;
        ULONG aiSize = sizeof(ai);
        hr = pAdapter->Query(&ai, &aiSize);
        pAdapter->Release();
        if (FAILED(hr)) continue;

        if (out)
        {
            rdma_adapter_info* dst = &out[count];
            memset(dst, 0, sizeof(*dst));
            InetNtopA(AF_INET, &pAddr->sin_addr, dst->ip_address, sizeof(dst->ip_address));
            dst->adapter_id          = ai.AdapterId;
            dst->vendor_id           = ai.VendorId;
            dst->device_id           = ai.DeviceId;
            dst->max_transfer_mb     = (UINT32)(ai.MaxTransferLength / (1024 * 1024));
            dst->max_inline_data     = ai.MaxInlineDataSize;
            dst->max_cq_depth        = ai.MaxCompletionQueueDepth;
            dst->max_initiator_depth = ai.MaxInitiatorQueueDepth;
            dst->flags               = ai.AdapterFlags;
            dst->has_in_order_dma    = !!(ai.AdapterFlags & ND_ADAPTER_FLAG_IN_ORDER_DMA_SUPPORTED);
            dst->has_multi_engine    = !!(ai.AdapterFlags & ND_ADAPTER_FLAG_MULTI_ENGINE_SUPPORTED);
            dst->has_loopback        = !!(ai.AdapterFlags & ND_ADAPTER_FLAG_LOOPBACK_CONNECTIONS_SUPPORTED);
        }

        count++;
    }

    HeapFree(GetProcessHeap(), 0, pList);
    return count;
}

// ===================================================================
// Public API
// ===================================================================
RDMA_TRANSFER_API int rdma_transfer_init(void)
{
    if (InterlockedIncrement(&g_init_count) > 1) return 0; // already initialized

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { InterlockedDecrement(&g_init_count); return -1; }

    HRESULT hr = NdStartup();
    if (FAILED(hr)) { WSACleanup(); InterlockedDecrement(&g_init_count); return -1; }

    return 0;
}

RDMA_TRANSFER_API void rdma_transfer_cleanup(void)
{
    if (InterlockedDecrement(&g_init_count) > 0) return;
    if (g_init_count < 0) { g_init_count = 0; return; }
    NdCleanup();
    WSACleanup();
}

RDMA_TRANSFER_API int rdma_send_file(const char* remote_ip, unsigned short port, const wchar_t* file_path)
{
    return InternalSend(remote_ip, port, file_path);
}

RDMA_TRANSFER_API int rdma_recv_file(const char* local_ip, unsigned short port, const wchar_t* output_path)
{
    return InternalRecv(local_ip, port, output_path);
}

RDMA_TRANSFER_API int rdma_bench(int side, const char* ip, unsigned short port, int size_mb)
{
    if (side == 1)
        return InternalBenchSend(ip, port, size_mb);
    else
        return InternalBenchRecv(ip, port, size_mb);
}

RDMA_TRANSFER_API void rdma_set_progress_callback(rdma_progress_cb cb, void* user_data)
{
    g_progress_cb  = cb;
    g_progress_ctx = user_data;
}

RDMA_TRANSFER_API void rdma_set_metadata_callback(rdma_metadata_cb cb, void* user_data)
{
    g_metadata_cb  = cb;
    g_metadata_ctx = user_data;
}

RDMA_TRANSFER_API const char* rdma_transfer_last_error(void)
{
    return g_last_error;
}

RDMA_TRANSFER_API void rdma_transfer_cancel(void)
{
    InterlockedExchange(&g_cancel_flag, 1);
}

// ===================================================================
// DLL entry point
// ===================================================================
BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_DETACH) {
        while (g_init_count > 0)
            rdma_transfer_cleanup();
    }
    return TRUE;
}
