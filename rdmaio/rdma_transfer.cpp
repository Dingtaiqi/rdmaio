// rdma_transfer.cpp — RDMA Write file transfer over NDSPI with credit-based flow control
//
// Architecture:
//   Data:  Sender RDMA Writes chunks into receiver's ring buffer.
//          Receiver CPU is NEVER touched during data arrival.
//   Ctrl:  Doorbell (Send)  — sender → receiver: "slot X has Y bytes"
//          Credit   (Send)  — receiver → sender: "slot X is free, reuse it"
//   Flow:  Credits provide natural backpressure — sender never overwhelms receiver.
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

// ---- Constants ---------------------------------------------------------
static const DWORD CHUNK_SIZE   = 4 * 1024 * 1024;   // 4 MiB chunks
static const DWORD CQ_DEPTH     = 1024;
static const DWORD QP_DEPTH     = 512;                 // covers send + recv depth
static const DWORD ALIGNMENT    = 4096;
static const DWORD NUM_SLOTS    = 256;                 // ring buffer depth (power of 2, 256×4MB=1GB)
static const uint32_t SLOT_MASK = NUM_SLOTS - 1;

// CQ completion context IDs — all < 0x1000 (avoids benchmark IDs 0x4001-0x6001)
#define META_SEND_CTX        ((void*)0x0001)
#define META_RECV_CTX        ((void*)0x0002)
#define SLOTINFO_SEND_CTX    ((void*)0x0101)
#define SLOTINFO_RECV_CTX    ((void*)0x0102)
#define CHUNK_WRITE_CTX      ((void*)0x0201)
#define DOORBELL_SEND_CTX    ((void*)0x0301)
#define CREDIT_SEND_CTX      ((void*)0x0501)
#define DONE_SEND_CTX        ((void*)0x0801)
#define DONE_RECV_CTX        ((void*)0x0802)
#define ABORT_SLOT_MARKER    UINT32_MAX

// Benchmark IDs (keep unchanged, must not overlap with above)
#define PEER_SEND_CTX   ((void*)0x4001)
#define PEER_RECV_CTX   ((void*)0x4002)
#define BENCH_WRITE_CTX ((void*)0x6001)

#pragma pack(push, 1)
struct FileMeta {
    uint64_t file_size;
    uint32_t filename_len;
    char     filename[256];
};

struct SlotInfo {
    UINT64   base_addr;
    UINT32   remote_token;
    uint32_t num_slots;
};

struct DoorbellMsg {
    uint32_t slot_index;
    uint32_t data_size;         // actual data size (may be < CHUNK_SIZE for last chunk)
};

struct CreditMsg {
    uint32_t slot_index;        // slot now free; ABORT_SLOT_MARKER = disk error
};
#pragma pack(pop)

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
static int               g_is_read      = 0;   // global flag for RDMA Read mode

#define SET_ERROR(msg) do { \
    strcpy_s(g_last_error, sizeof(g_last_error), msg); \
    DbgLog(msg); \
} while(0)

static void DbgLog(const char* msg) {
    char buf[512];
    SYSTEMTIME st; GetLocalTime(&st);
    sprintf_s(buf, "%02d:%02d:%02d.%03d [%lu] %s\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        GetCurrentThreadId(), msg);
    OutputDebugStringA(buf);
    // Also write to rdma_dll.log next to the DLL
    static char logPath[MAX_PATH] = "";
    if (logPath[0] == 0) {
        GetModuleFileNameA(NULL, logPath, MAX_PATH);
        char* p = strrchr(logPath, '\\');
        if (p) { p[1] = 0; }
        strcat_s(logPath, "rdma_dll.log");
    }
    OutputDebugStringA(buf);
    FILE* f = NULL;
    fopen_s(&f, logPath, "a");
    if (f) { fputs(buf, f); fflush(f); fclose(f); }
    else {
        // Fallback: write to stderr so it appears in the redirected console
        fprintf(stderr, "%s", buf);
        fflush(stderr);
    }
}

// ---- Helpers ------------------------------------------------------------
static void* AllocAligned(size_t sz) {
    void* p = _aligned_malloc(sz, ALIGNMENT);
    if (p) memset(p, 0, sz);
    return p;
}
static void FreeAligned(void* p) { if (p) _aligned_free(p); }
static void* AllocLarge(size_t sz) {
    // VirtualAlloc is better for large DMA buffers than _aligned_malloc
    void* p = VirtualAlloc(NULL, sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p) memset(p, 0, sz);
    return p;
}
static void FreeLarge(void* p) { if (p) VirtualFree(p, 0, MEM_RELEASE); }

static HRESULT WaitOverlapped(IND2Overlapped* pObj, OVERLAPPED* pOv) {
    return pObj->GetOverlappedResult(pOv, TRUE);
}

// Blocking CQ wait using Notify-based event (Microsoft ndtestutil pattern).
// Returns when a completion is available or cancelled.
// Avoids busy-wait spinning.
static bool WaitForCompletion(IND2CompletionQueue* pCq, OVERLAPPED* pOv, ND2_RESULT& result) {
    if (!pCq) return false;
    for (;;) {
        if (InterlockedCompareExchange(&g_cancel_flag, 0, 0)) return false;
        if (pCq->GetResults(&result, 1) == 1) return true;
        HRESULT hr = pCq->Notify(ND_CQ_NOTIFY_ANY, pOv);
        if (hr == ND_PENDING) {
            pCq->GetOverlappedResult(pOv, TRUE);
        }
    }
}

static void CleanupNd(NdContext& ctx) {
    HRESULT hr;

    // Drain the CQ before Disconnect to satisfy the NetworkDirect requirement
    // that "QP 上的所有 DMA 活动都已完成" before calling NdDisconnect.
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

static void ExtractFileName(const wchar_t* path, char* out, uint32_t* pLen, size_t maxLen) {
    const wchar_t* name = wcsrchr(path, L'\\');
    if (!name) name = wcsrchr(path, L'/');
    if (!name) name = path; else name++;
    int n = WideCharToMultiByte(CP_ACP, 0, name, -1, out, (int)maxLen, nullptr, nullptr);
    *pLen = n > 0 ? (uint32_t)(n - 1) : 0;
}

// ===================================================================
// RDMA Write file transfer — sender
// ===================================================================
static int InternalSend(const char* remoteIp, USHORT port, const wchar_t* filePath) {
    int ret = 0; NdContext ctx; HANDLE hFile = INVALID_HANDLE_VALUE;
    void* pBuf = nullptr;
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

    // --- Sender memory layout (one MR) ---
    //  [0 .. sizeof(FileMeta)]         : FileMeta to send
    //  [ALIGNMENT .. ALIGNMENT+CHUNK_SIZE] : local chunk buffer (ReadFile target + Write source)
    //  [ALIGNMENT+CHUNK_SIZE .. +sizeof(DoorbellMsg)] : doorbell send buffer
    //  [+sizeof(DoorbellMsg) .. +sizeof(SlotInfo)] : slot info recv buffer
    //  [+sizeof(SlotInfo) .. +NUM_SLOTS*sizeof(CreditMsg)] : credit recv ring
    //  [+NUM_SLOTS*sizeof(CreditMsg) .. +sizeof(uint32_t)] : done recv buffer
    size_t sendBufSize = ALIGNMENT + CHUNK_SIZE + sizeof(DoorbellMsg) + sizeof(SlotInfo)
                       + NUM_SLOTS * sizeof(CreditMsg) + sizeof(uint32_t);
    pBuf = AllocAligned(sendBufSize);
    if (!pBuf) { CleanupNd(ctx); return -1; }

    hr = ctx.pMr->Register(pBuf, sendBufSize, ND_MR_FLAG_ALLOW_LOCAL_WRITE, &ctx.ov);
    if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pMr, &ctx.ov);
    if (FAILED(hr)) { SET_ERROR("Register send buffer failed"); CleanupNd(ctx); return -1; }
    UINT32 localToken = ctx.pMr->GetLocalToken();

    // Pointers into sender buffer
    FileMeta*    pMeta     = (FileMeta*)pBuf;
    char*        pChunk    = (char*)pBuf + ALIGNMENT;
    DoorbellMsg* pDoorbell = (DoorbellMsg*)(pChunk + CHUNK_SIZE);
    SlotInfo*    pSlotInfo = (SlotInfo*)(pDoorbell + 1);
    CreditMsg*   creditRecvs = (CreditMsg*)(pSlotInfo + 1);
    uint32_t*    pDoneBuf  = (uint32_t*)(creditRecvs + NUM_SLOTS);

    do {
        // ---- Connect to receiver ------------------------------------------------
        hr = ctx.pConnector->Bind((const sockaddr*)&localAddr, sizeof(localAddr));
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        if (FAILED(hr)) { SET_ERROR("Bind failed"); ret = -1; break; }

        hr = ctx.pConnector->Connect(ctx.pQp, (const sockaddr*)&remoteAddr, sizeof(remoteAddr),
                                      0, 0, nullptr, 0, &ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        if (FAILED(hr)) { char b[64]; sprintf_s(b, "Connect failed: hr=0x%08X", hr); SET_ERROR(b); ret = -1; break; }

        // Post Recv for SlotInfo (receiver's buffer details) BEFORE CompleteConnect
        ND2_SGE slotInfoSge = {}; slotInfoSge.Buffer = pSlotInfo;
        slotInfoSge.BufferLength = sizeof(SlotInfo); slotInfoSge.MemoryRegionToken = localToken;
        hr = ctx.pQp->Receive(SLOTINFO_RECV_CTX, &slotInfoSge, 1);
        if (FAILED(hr)) { ret = -1; break; }

        hr = ctx.pConnector->CompleteConnect(&ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        if (FAILED(hr)) { SET_ERROR("CompleteConnect failed"); ret = -1; break; }

        // ---- Open file ---------------------------------------------------------
        hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) { ret = -1; break; }

        LARGE_INTEGER fsLi;
        if (!GetFileSizeEx(hFile, &fsLi)) { SET_ERROR("GetFileSizeEx failed"); ret = -1; break; }
        uint64_t fileSize = (uint64_t)fsLi.QuadPart;

        // ---- Metadata exchange via Send/Recv -----------------------------------
        // 1. Send FileMeta to receiver
        pMeta->file_size = fileSize;
        ExtractFileName(filePath, pMeta->filename, &pMeta->filename_len, sizeof(pMeta->filename));
        FireMetadata(pMeta->filename, fileSize);
        char logBuf[128];
        sprintf_s(logBuf, "SEND: sending %s (%llu bytes, %lu chunks)",
            pMeta->filename, fileSize,
            (ULONG)((fileSize + CHUNK_SIZE - 1) / CHUNK_SIZE));
        DbgLog(logBuf);

        ND2_SGE metaSge = {}; metaSge.Buffer = pMeta;
        metaSge.BufferLength = sizeof(FileMeta); metaSge.MemoryRegionToken = localToken;
        hr = ctx.pQp->Send(META_SEND_CTX, &metaSge, 1, 0);
        if (FAILED(hr)) { SET_ERROR("Send metadata failed"); ret = -1; break; }
        ND2_RESULT cqResult = {};
        if (!WaitForCompletion(ctx.pCq, &ctx.ov, cqResult)) { ret = -1; break; }
        if (cqResult.Status != ND_SUCCESS) { ret = -1; break; }

        // 2. Wait for SlotInfo from receiver (remote buffer layout)
        if (!WaitForCompletion(ctx.pCq, &ctx.ov, cqResult)) { ret = -1; break; }
        if (IsCancelled() || cqResult.Status != ND_SUCCESS ||
            cqResult.RequestContext != SLOTINFO_RECV_CTX) { ret = -1; break; }
        UINT64 remoteBaseAddr = pSlotInfo->base_addr;
        UINT32 remoteToken    = pSlotInfo->remote_token;
        uint32_t remoteSlots  = pSlotInfo->num_slots;
        if (remoteSlots < 1) { SET_ERROR("Receiver has no slots"); ret = -1; break; }
        uint32_t usedSlots = min(remoteSlots, NUM_SLOTS);
        sprintf_s(logBuf, "SEND: got slot info: base=0x%llX token=0x%X slots=%lu",
            remoteBaseAddr, remoteToken, usedSlots);
        DbgLog(logBuf);

        // ---- Pre-post credit Recvs --------------------------------------------
        // Must come BEFORE DONE Recv so that credit Sends from the receiver
        // match a credit Recv, not the DONE Recv, in the Recv queue.
        ND2_SGE creditSge = {}; creditSge.MemoryRegionToken = localToken;
        for (uint32_t i = 0; i < usedSlots; i++) {
            creditSge.Buffer = &creditRecvs[i];
            creditSge.BufferLength = sizeof(CreditMsg);
            hr = ctx.pQp->Receive((void*)(uintptr_t)(0x2000 + i), &creditSge, 1);
            if (FAILED(hr)) { SET_ERROR("Post credit Recv failed"); ret = -1; break; }
        }
        if (ret) break;
        DbgLog("SEND: posted credit Recvs");

        // Post Recv for DONE (transfer-complete signal from receiver).
        // Must come AFTER the credit Recvs.
        ND2_SGE doneSge = {}; doneSge.Buffer = pDoneBuf;
        doneSge.BufferLength = sizeof(uint32_t); doneSge.MemoryRegionToken = localToken;
        hr = ctx.pQp->Receive(DONE_RECV_CTX, &doneSge, 1);
        if (FAILED(hr)) { SET_ERROR("Post DONE Recv failed"); ret = -1; break; }

        // ---- Initialize free-slot tracking -------------------------------------
        // At any point: freeCount = number of slots the sender may write to.
        // Initially all slots are free. Each RDMA Write uses one; each credit returns one.
        uint32_t freeSlots[NUM_SLOTS];
        uint32_t freeHead = 0;
        uint32_t freeCount = usedSlots;
        for (uint32_t i = 0; i < usedSlots; i++) freeSlots[i] = i;
        uint32_t bytesSent = 0;

        // ---- RDMA Write data transfer loop -------------------------------------
        LARGE_INTEGER freq, t0;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);

        while (bytesSent < fileSize && ret == 0) {
            // 1. Wait for a free slot (credit)
            if (freeCount == 0) {
                DbgLog("SEND: waiting for credit (all slots full)");
                while (freeCount == 0 && ret == 0 && !IsCancelled()) {
                    while (ctx.pCq->GetResults(&cqResult, 1) == 0 && !IsCancelled()) {}
                    if (IsCancelled()) { ret = -1; break; }
                    if (cqResult.Status == ND_SUCCESS &&
                        (uintptr_t)cqResult.RequestContext >= 0x2000 &&
                        (uintptr_t)cqResult.RequestContext < 0x2000 + usedSlots) {
                        uint32_t bufIdx = (uint32_t)((uintptr_t)cqResult.RequestContext - 0x2000);
                        uint32_t slotIdx = creditRecvs[bufIdx].slot_index;
                        if (slotIdx == ABORT_SLOT_MARKER) {
                            SET_ERROR("Receiver disk error");
                            DbgLog("SEND: receiver reported disk error");
                            ret = -1; break;
                        }
                        freeSlots[(freeHead + freeCount) & SLOT_MASK] = slotIdx;
                        freeCount++;
                        // Re-post credit Recv
                        creditSge.Buffer = &creditRecvs[bufIdx];
                        hr = ctx.pQp->Receive((void*)(uintptr_t)(0x2000 + bufIdx), &creditSge, 1);
                        if (FAILED(hr)) { ret = -1; break; }
                    } else if (cqResult.Status == ND_SUCCESS &&
                               cqResult.RequestContext == DONE_RECV_CTX) {
                        // Receiver sent DONE early — no more data to write
                        DbgLog("SEND: got DONE while waiting for credit");
                        ret = -1;
                        break;
                    } else if (cqResult.Status != ND_SUCCESS) {
                        char buf[64];
                        sprintf_s(buf, "SEND: credit wait CQ=0x%08X", cqResult.Status);
                        SET_ERROR(buf);
                        ret = -1; break;
                    }
                }
                if (ret) break;
            }
            if (ret) break;

            // 2. Pop free slot
            uint32_t slot = freeSlots[freeHead];
            freeHead = (freeHead + 1) & SLOT_MASK;
            freeCount--;

            // 3. Read chunk from disk
            DWORD toRead = (fileSize - bytesSent > CHUNK_SIZE) ? CHUNK_SIZE
                         : (DWORD)(fileSize - bytesSent);
            DWORD rb = 0;
            if (!ReadFile(hFile, pChunk, toRead, &rb, nullptr) || rb != toRead) {
                SET_ERROR("ReadFile failed"); ret = -1; break;
            }

            // 4. RDMA Write chunk into receiver's ring buffer slot
            UINT64 remoteAddr = remoteBaseAddr + (UINT64)slot * CHUNK_SIZE;
            ND2_SGE writeSge = {}; writeSge.Buffer = pChunk;
            writeSge.BufferLength = toRead; writeSge.MemoryRegionToken = localToken;
            hr = ctx.pQp->Write(CHUNK_WRITE_CTX, &writeSge, 1,
                remoteAddr, remoteToken, ND_OP_FLAG_SILENT_SUCCESS);
            if (FAILED(hr)) { SET_ERROR("RDMA Write failed"); ret = -1; break; }
            // Write is fire-and-forget (SILENT_SUCCESS). QP ordering guarantees
            // the data arrives at the remote buffer before the doorbell below.

            // 5. Send doorbell: tell receiver "slot X has Y bytes of data"
            pDoorbell->slot_index = slot;
            pDoorbell->data_size  = toRead;
            ND2_SGE doorbellSge = {}; doorbellSge.Buffer = pDoorbell;
            doorbellSge.BufferLength = sizeof(DoorbellMsg);
            doorbellSge.MemoryRegionToken = localToken;
            hr = ctx.pQp->Send(DOORBELL_SEND_CTX, &doorbellSge, 1, 0);
            if (FAILED(hr)) { SET_ERROR("Send doorbell failed"); ret = -1; break; }

            // 6. Wait for doorbell Send CQ
            if (!WaitForCompletion(ctx.pCq, &ctx.ov, cqResult)) { ret = -1; break; }
            if (cqResult.Status != ND_SUCCESS) {
                sprintf_s(logBuf, "SEND: doorbell CQ=0x%08X (Write may have failed)", cqResult.Status);
                SET_ERROR(logBuf); ret = -1; break;
            }

            bytesSent += toRead;

            // 7. Non-blocking check for any pending credits
            while (true) {
                ND2_RESULT cr;
                ULONG got = ctx.pCq->GetResults(&cr, 1);
                if (got == 0) break;
                if (cr.Status == ND_SUCCESS &&
                    (uintptr_t)cr.RequestContext >= 0x2000 &&
                    (uintptr_t)cr.RequestContext < 0x2000 + usedSlots) {
                    uint32_t bufIdx = (uint32_t)((uintptr_t)cr.RequestContext - 0x2000);
                    uint32_t slotIdx = creditRecvs[bufIdx].slot_index;
                    if (slotIdx == ABORT_SLOT_MARKER) {
                        SET_ERROR("Receiver disk error (async)");
                        ret = -1; break;
                    }
                    freeSlots[(freeHead + freeCount) & SLOT_MASK] = slotIdx;
                    freeCount++;
                    creditSge.Buffer = &creditRecvs[bufIdx];
                    hr = ctx.pQp->Receive((void*)(uintptr_t)(0x2000 + bufIdx), &creditSge, 1);
                    if (FAILED(hr)) { ret = -1; break; }
                } else if (cr.Status == ND_SUCCESS && cr.RequestContext == DONE_RECV_CTX) {
                    // Receiver sent DONE — all data already processed
                    DbgLog("SEND: receiver sent DONE early — transfer complete");
                    // We can exit the outer loop too, but let's check bytesSent first
                    if (bytesSent >= fileSize) {
                        // We already sent everything, receiver confirmed processing
                        ret = 0;
                    } else {
                        ret = -1;
                    }
                    break;
                }
            }
            if (ret) break;

            LARGE_INTEGER tn; QueryPerformanceCounter(&tn);
            FireProgress(bytesSent, fileSize,
                (double)(tn.QuadPart - t0.QuadPart) / (double)freq.QuadPart);
        }

        // ---- Drain: wait for all outstanding credits ---------------------------
        // All chunks have been sent. Wait for the receiver to finish writing
        // everything to disk and return all credits.
        // If the receiver disconnects after finishing, CQ will return an error —
        // that's OK as long as we already sent all data.
        DbgLog("SEND: draining credits...");
        while (freeCount < usedSlots && ret == 0 && !IsCancelled()) {
            while (ctx.pCq->GetResults(&cqResult, 1) == 0 && !IsCancelled()) {}
            if (IsCancelled()) { ret = -1; break; }
            if (cqResult.Status == ND_SUCCESS &&
                (uintptr_t)cqResult.RequestContext >= 0x2000 &&
                (uintptr_t)cqResult.RequestContext < 0x2000 + usedSlots) {
                uint32_t bufIdx = (uint32_t)((uintptr_t)cqResult.RequestContext - 0x2000);
                uint32_t slotIdx = creditRecvs[bufIdx].slot_index;
                if (slotIdx == ABORT_SLOT_MARKER) {
                    SET_ERROR("Receiver disk error (drain)");
                    ret = -1; break;
                }
                freeSlots[(freeHead + freeCount) & SLOT_MASK] = slotIdx;
                freeCount++;
                creditSge.Buffer = &creditRecvs[bufIdx];
                hr = ctx.pQp->Receive((void*)(uintptr_t)(0x2000 + bufIdx), &creditSge, 1);
                if (FAILED(hr)) { ret = -1; break; }
            } else if (cqResult.Status == ND_SUCCESS &&
                       cqResult.RequestContext == DONE_RECV_CTX) {
                DbgLog("SEND: received DONE during drain");
                freeCount = usedSlots;
                break;
            } else if (cqResult.Status != ND_SUCCESS) {
                // Receiver may have disconnected after writing all data — OK.
                DbgLog("SEND: drain CQ non-success — assuming receiver done");
                freeCount = usedSlots;
                break;
            }
        }

        if (ret == 0) {
            // ---- Send DONE to signal clean completion --------------------------
            DbgLog("SEND: all credits returned, sending DONE");
            uint32_t doneVal = 1;
            ND2_SGE doneSendSge = {}; doneSendSge.Buffer = &doneVal;
            doneSendSge.BufferLength = sizeof(doneVal);
            doneSendSge.MemoryRegionToken = localToken;
            hr = ctx.pQp->Send(DONE_SEND_CTX, &doneSendSge, 1, 0);
            if (SUCCEEDED(hr)) {
                while (ctx.pCq->GetResults(&cqResult, 1) == 0 && !IsCancelled()) {}
                if (!IsCancelled() && cqResult.Status == ND_SUCCESS) {
                    DbgLog("SEND: DONE sent successfully");
                }
            }
        }

        LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
        if (ret == 0) {
            FireProgress(fileSize, fileSize,
                (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart);
        }
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

// ===================================================================
// RDMA Write file transfer — receiver
// ===================================================================
static int InternalRecv(const char* localIp, USHORT port, const wchar_t* outPath) {
    int ret = 0; NdContext ctx; HANDLE hFile = INVALID_HANDLE_VALUE;
    void* pBuf = nullptr;
    ResetCancel();
    DbgLog("RECV: start (RDMA Write)");

    struct sockaddr_in localAddr = {};
    if (ParseIpv4(localIp, port, &localAddr) != 0) { SET_ERROR("ParseIpv4"); return -1; }

    HRESULT hr = NdOpenAdapter(IID_IND2Adapter, (const sockaddr*)&localAddr, sizeof(localAddr),
                                (void**)&ctx.pAdapter);
    if (FAILED(hr)) { SET_ERROR("NdOpenAdapter"); return -1; }

    ctx.ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!ctx.ov.hEvent) { SET_ERROR("CreateEvent"); return -1; }
    hr = ctx.pAdapter->CreateOverlappedFile(&ctx.hOvFile);
    if (FAILED(hr)) { SET_ERROR("CreateOverlappedFile"); CleanupNd(ctx); return -1; }

    hr = ctx.pAdapter->CreateCompletionQueue(IID_IND2CompletionQueue, ctx.hOvFile,
        CQ_DEPTH, 0, 0, (void**)&ctx.pCq);
    if (FAILED(hr)) { SET_ERROR("CreateCQ"); CleanupNd(ctx); return -1; }

    hr = ctx.pAdapter->CreateConnector(IID_IND2Connector, ctx.hOvFile, (void**)&ctx.pConnector);
    if (FAILED(hr)) { SET_ERROR("CreateConnector"); CleanupNd(ctx); return -1; }

    hr = ctx.pAdapter->CreateQueuePair(IID_IND2QueuePair, ctx.pCq, ctx.pCq, nullptr,
        QP_DEPTH, QP_DEPTH, 1, 1, 0, (void**)&ctx.pQp);
    if (FAILED(hr)) { SET_ERROR("CreateQP"); CleanupNd(ctx); return -1; }

    hr = ctx.pAdapter->CreateMemoryRegion(IID_IND2MemoryRegion, ctx.hOvFile, (void**)&ctx.pMr);
    if (FAILED(hr)) { SET_ERROR("CreateMR"); CleanupNd(ctx); return -1; }

    // Allocate ONE big buffer: control area + ring buffer area.
    // Single MR registration avoids multi-MR issues with remote tokens.
    // Layout: [ctrl: FileMeta|SlotInfo|DoorbellBufs|DoneBuf] [ring: CHUNK_SIZE * NUM_SLOTS]
    const size_t ctrlAreaSize = (sizeof(FileMeta) + sizeof(SlotInfo)
                               + NUM_SLOTS * sizeof(DoorbellMsg)
                               + sizeof(uint32_t) + sizeof(CreditMsg) + 4095) & ~4095;
    const size_t ringAreaSize = (size_t)CHUNK_SIZE * NUM_SLOTS;
    const size_t totalBufSize = ctrlAreaSize + ringAreaSize;
    pBuf = AllocLarge(totalBufSize);
    if (!pBuf) { SET_ERROR("AllocLarge total buf"); CleanupNd(ctx); return -1; }
    DbgLog("RECV: total buf allocated (single MR)");

    hr = ctx.pMr->Register(pBuf, totalBufSize,
        ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE, &ctx.ov);
    if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pMr, &ctx.ov);
    if (FAILED(hr)) { SET_ERROR("Register total buf"); CleanupNd(ctx); return -1; }
    UINT32 ctrlToken = ctx.pMr->GetLocalToken();
    UINT32 remoteToken = ctx.pMr->GetRemoteToken();
    DbgLog("RECV: total buf registered (single MR)");

    // Pointers in the unified buffer
    char* buf = (char*)pBuf;
    FileMeta*    pMeta      = (FileMeta*)buf;
    SlotInfo*    pSlotInfo  = (SlotInfo*)(buf + sizeof(FileMeta));
    DoorbellMsg* doorbellBufs = (DoorbellMsg*)(buf + sizeof(FileMeta) + sizeof(SlotInfo));
    uint32_t*    pDoneRecv  = (uint32_t*)(buf + sizeof(FileMeta) + sizeof(SlotInfo)
                               + NUM_SLOTS * sizeof(DoorbellMsg));
    CreditMsg*   pCreditSend = (CreditMsg*)(buf + sizeof(FileMeta) + sizeof(SlotInfo)
                               + NUM_SLOTS * sizeof(DoorbellMsg) + sizeof(uint32_t));
    char*        pRingBuf   = buf + ctrlAreaSize;

    do {
        hr = ctx.pAdapter->CreateListener(IID_IND2Listener, ctx.hOvFile, (void**)&ctx.pListener);
        if (FAILED(hr)) { SET_ERROR("CreateListener"); ret = -1; break; }

        hr = ctx.pListener->Bind((const sockaddr*)&localAddr, sizeof(localAddr));
        if (FAILED(hr)) { char b[64]; sprintf_s(b, "Bind failed: hr=0x%08X", hr); SET_ERROR(b); ret = -1; break; }
        hr = ctx.pListener->Listen(1);
        if (FAILED(hr)) { SET_ERROR("Listen"); ret = -1; break; }
        DbgLog("RECV: listening...");

        // Post Recv for FileMeta BEFORE accepting the connection
        ND2_SGE metaSge = {}; metaSge.Buffer = pMeta;
        metaSge.BufferLength = sizeof(FileMeta); metaSge.MemoryRegionToken = ctrlToken;
        hr = ctx.pQp->Receive(META_RECV_CTX, &metaSge, 1);
        if (FAILED(hr)) { ret = -1; break; }

        // Accept connection
        hr = ctx.pListener->GetConnectionRequest(ctx.pConnector, &ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pListener, &ctx.ov);
        if (FAILED(hr)) { SET_ERROR("GetConnectionRequest"); ret = -1; break; }
        DbgLog("RECV: got connection request");

        hr = ctx.pConnector->Accept(ctx.pQp, 0, 0, nullptr, 0, &ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        if (FAILED(hr)) { ret = -1; break; }
        DbgLog("RECV: accepted");

        // Wait for FileMeta
        ND2_RESULT cqResult = {};
        if (!WaitForCompletion(ctx.pCq, &ctx.ov, cqResult)) { ret = -1; break; }
        if (cqResult.Status != ND_SUCCESS ||
            cqResult.RequestContext != META_RECV_CTX) { ret = -1; break; }
        uint64_t fileSize = pMeta->file_size;
        DbgLog("RECV: got metadata");

        // Compute output path
        std::wstring finalPath = IsDirectory(outPath)
            ? BuildOutputPath(outPath, pMeta->filename)
            : std::wstring(outPath);

        // Check disk space (basic check)
        ULARGE_INTEGER freeBytesAvailable;
        if (GetDiskFreeSpaceExW(finalPath.c_str(), &freeBytesAvailable, nullptr, nullptr)) {
            if (freeBytesAvailable.QuadPart < fileSize) {
                SET_ERROR("Insufficient disk space");
                ret = -1; break;
            }
        }

        // Create output file
        hFile = CreateFileW(finalPath.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) { ret = -1; break; }
        DbgLog("RECV: created output file");

        // ring buffer is inside the unified pBuf, already registered
        UINT64 ringBase = (UINT64)pRingBuf;

        // ---- Pre-post doorbell Recvs (all slots ready to receive notifications) -
        ND2_SGE doorbellSge = {}; doorbellSge.MemoryRegionToken = ctrlToken;
        for (uint32_t i = 0; i < NUM_SLOTS; i++) {
            doorbellSge.Buffer = &doorbellBufs[i];
            doorbellSge.BufferLength = sizeof(DoorbellMsg);
            hr = ctx.pQp->Receive((void*)(uintptr_t)(0x1000 + i), &doorbellSge, 1);
            if (FAILED(hr)) {
                char buf[64]; sprintf_s(buf, "RECV: post doorbell[%lu] failed", i);
                SET_ERROR(buf); ret = -1; break;
            }
        }
        if (ret) break;
        DbgLog("RECV: posted doorbell Recvs");

        // ---- Pre-post DONE Recv (will be consumed after last doorbell) ----------
        ND2_SGE doneSge = {}; doneSge.Buffer = pDoneRecv;
        doneSge.BufferLength = sizeof(uint32_t); doneSge.MemoryRegionToken = ctrlToken;
        hr = ctx.pQp->Receive(DONE_RECV_CTX, &doneSge, 1);
        if (FAILED(hr)) { ret = -1; break; }

        // ---- Send SlotInfo to sender (ring buffer details) --------------------
        pSlotInfo->base_addr   = ringBase;
        pSlotInfo->remote_token = remoteToken;
        pSlotInfo->num_slots    = NUM_SLOTS;
        ND2_SGE slotInfoSge = {}; slotInfoSge.Buffer = pSlotInfo;
        slotInfoSge.BufferLength = sizeof(SlotInfo);
        slotInfoSge.MemoryRegionToken = ctrlToken;
        hr = ctx.pQp->Send(SLOTINFO_SEND_CTX, &slotInfoSge, 1, 0);
        if (FAILED(hr)) { ret = -1; break; }
        while (ctx.pCq->GetResults(&cqResult, 1) == 0 && !IsCancelled()) {}
        if (IsCancelled() || cqResult.Status != ND_SUCCESS) { ret = -1; break; }
        DbgLog("RECV: sent SlotInfo to sender");

        // Fire metadata callback (filename + size)
        FireMetadata(pMeta->filename, fileSize);

        // ---- Transfer data loop -----------------------------------------------
        // In this loop we process doorbells: the sender RDMA Writes data into our
        // ring buffer silently (zero CPU), then sends a doorbell to wake us up.
        // We read the doorbell, write the data to disk, and return a credit.
        uint64_t bytesReceived = 0;
        bool diskError = false;
        LARGE_INTEGER freq, t0;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        char logBuf[128];

        while (bytesReceived < fileSize && !diskError) {
            // Wait for doorbell or DONE
            if (!WaitForCompletion(ctx.pCq, &ctx.ov, cqResult)) { ret = -1; diskError = true; break; }

            if (cqResult.Status != ND_SUCCESS) {
                if (bytesReceived >= fileSize) {
                    break;
                }
                sprintf_s(logBuf, "RECV: CQ error ctx=0x%p status=0x%08X recvd=%llu",
                    cqResult.RequestContext, cqResult.Status, bytesReceived);
                SET_ERROR(logBuf);
                ret = -1; diskError = true; break;
            }

            if ((uintptr_t)cqResult.RequestContext >= 0x1000 &&
                (uintptr_t)cqResult.RequestContext < 0x1000 + NUM_SLOTS) {
                // Process doorbell
                uint32_t dbIdx = (uint32_t)((uintptr_t)cqResult.RequestContext - 0x1000);
                uint32_t slot = doorbellBufs[dbIdx].slot_index;
                uint32_t dataSize = doorbellBufs[dbIdx].data_size;

                if (slot >= NUM_SLOTS || dataSize > CHUNK_SIZE || dataSize == 0) {
                    SET_ERROR("Invalid doorbell"); ret = -1; break;
                }

                // Write data from ring buffer slot to disk
                DWORD wb = 0;
                if (!WriteFile(hFile, (char*)pRingBuf + (size_t)slot * CHUNK_SIZE,
                              dataSize, &wb, nullptr) || wb != dataSize) {
                    SET_ERROR("WriteFile failed");
                    diskError = true;
                    pCreditSend->slot_index = ABORT_SLOT_MARKER;
                    ND2_SGE abortSge = {};
                    abortSge.Buffer = pCreditSend;
                    abortSge.BufferLength = sizeof(CreditMsg);
                    abortSge.MemoryRegionToken = ctrlToken;
                    ctx.pQp->Send(CREDIT_SEND_CTX, &abortSge, 1, 0);
                    ret = -1; break;
                }
                bytesReceived += dataSize;
                if (bytesReceived % (10 * CHUNK_SIZE) == 0) {
                    sprintf_s(logBuf, "RECV: %llu / %llu bytes (%.1f%%)",
                        bytesReceived, fileSize,
                        100.0 * (double)bytesReceived / (double)fileSize);
                    DbgLog(logBuf);
                }

                // Send credit: fire-and-forget (no wait for CQ).
                // The completion will arrive in the main poll loop below.
                pCreditSend->slot_index = slot;
                ND2_SGE creditSge = {};
                creditSge.Buffer = pCreditSend;
                creditSge.BufferLength = sizeof(CreditMsg);
                creditSge.MemoryRegionToken = ctrlToken;
                ctx.pQp->Send(CREDIT_SEND_CTX, &creditSge, 1, 0);

                // Re-post this doorbell Recv immediately
                doorbellSge.Buffer = &doorbellBufs[dbIdx];
                doorbellSge.BufferLength = sizeof(DoorbellMsg);
                hr = ctx.pQp->Receive((void*)(uintptr_t)(0x1000 + dbIdx), &doorbellSge, 1);
                if (FAILED(hr)) { ret = -1; break; }

                LARGE_INTEGER tn; QueryPerformanceCounter(&tn);
                FireProgress(bytesReceived, fileSize,
                    (double)(tn.QuadPart - t0.QuadPart) / (double)freq.QuadPart);

            } else if (cqResult.RequestContext == CREDIT_SEND_CTX) {
                // Credit Send completion — fire-and-forget, no action needed
            } else if (cqResult.RequestContext == DONE_RECV_CTX) {
                // Sender signaled transfer complete
                DbgLog("RECV: received DONE from sender");
                if (bytesReceived >= fileSize) {
                    ret = 0;  // success
                } else {
                    SET_ERROR("Sender DONE but not all bytes received");
                    ret = -1;
                }
                break;
            } else {
                sprintf_s(logBuf, "RECV: unexpected context 0x%p", cqResult.RequestContext);
                SET_ERROR(logBuf);
                ret = -1; break;
            }
        }

        if (diskError) {
            // Send abort via credit mechanism
            pCreditSend->slot_index = ABORT_SLOT_MARKER;
            ND2_SGE abortSge = {};
            abortSge.Buffer = pCreditSend;
            abortSge.BufferLength = sizeof(CreditMsg);
            abortSge.MemoryRegionToken = ctrlToken;
            ctx.pQp->Send(CREDIT_SEND_CTX, &abortSge, 1, 0);
            // fire-and-forget
        }

        LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
        if (ret == 0) {
            FireProgress(fileSize, fileSize,
                (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart);
        }
    } while (0);

    if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    FreeLarge(pBuf);  // unified buffer (control + ring, allocated via AllocLarge)
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

    {
        ULONG mrFlags = ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE;
        if (g_is_read) mrFlags |= ND_MR_FLAG_ALLOW_REMOTE_READ;
        hr = ctx.pMr->Register(pBuf, benchBufSize, mrFlags, &ctx.ov);
    }
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

    hr = ctx.pMr->Register(pBuf, benchBufSize,
        ND_MR_FLAG_ALLOW_LOCAL_WRITE | (g_is_read ? ND_MR_FLAG_RDMA_READ_SINK : 0), &ctx.ov);
    if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pMr, &ctx.ov);
    if (FAILED(hr)) { CleanupNd(ctx); FreeAligned(pBuf); return -1; }

    do {
        UINT32 localToken = ctx.pMr->GetLocalToken();

        hr = ctx.pConnector->Bind((const sockaddr*)&localAddr, sizeof(localAddr));
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        if (FAILED(hr)) { ret = -1; break; }

        hr = ctx.pConnector->Connect(ctx.pQp, (const sockaddr*)&remoteAddr, sizeof(remoteAddr),
                                      0, g_is_read ? 16 : 0, nullptr, 0, &ctx.ov);
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
            if (g_is_read) {
                hr = ctx.pQp->Read(BENCH_WRITE_CTX, &sge, 1,
                    pPeer->remoteAddress + offset, pPeer->remoteToken, flags);
            } else {
                hr = ctx.pQp->Write(BENCH_WRITE_CTX, &sge, 1,
                    pPeer->remoteAddress + offset, pPeer->remoteToken, flags);
            }
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

        printf("  RDMA %s: %.0f MB in %.1f ms  |  %.1f MB/s  (%.2f Gbps)\n",
               g_is_read ? "Read" : "Write",
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

// Memory share context IDs (must not conflict with file transfer or benchmark IDs)
#define MEM_SETUP_SEND_CTX    ((void*)0xA001)
#define MEM_SETUP_RECV_CTX    ((void*)0xA002)
#define MEM_WRITE_CTX         ((void*)0xA003)
#define MEM_DOORBELL_SEND_CTX ((void*)0xA004)
#define MEM_DOORBELL_RECV_CTX ((void*)0xA005)

struct MemSetupInfo {
    UINT64   remote_addr;
    UINT32   remote_token;
    uint32_t size;
};

struct MemDoorbellMsg {
    uint32_t offset;
    uint32_t length;
};

// Global state for memory share session
static struct MemShareState {
    NdContext ctx;
    void*     pBuf         = nullptr;
    char*     pLocalBuf    = nullptr;   // the actual shared memory
    uint32_t  bufSize      = 0;
    UINT32    localToken   = 0;
    UINT32    remoteToken  = 0;
    UINT64    remoteAddr   = 0;
    int       mode         = -1;        // 0=display, 1=writer
    bool      useRead      = false;
    bool      connected    = false;
    MemDoorbellMsg lastDoorbell = {};
    MemDoorbellMsg* pDoorbellSend = nullptr;
} g_mem;

// ===================================================================
// Shared Memory Monitor — implementation
// ===================================================================
static void MemCleanup() {
    if (g_mem.pBuf) { FreeLarge(g_mem.pBuf); g_mem.pBuf = nullptr; }
    g_mem.pLocalBuf = nullptr;
    g_mem.connected = false;
    g_mem.mode = -1;
    if (g_mem.ctx.pAdapter) {
        if (g_mem.ctx.pQp || g_mem.ctx.pCq || g_mem.ctx.pMr || g_mem.ctx.pConnector) {
            CleanupNd(g_mem.ctx);
        }
    }
    memset(&g_mem.ctx, 0, sizeof(g_mem.ctx));
}

RDMA_TRANSFER_API int rdma_mem_start(int mode, const char* ip, unsigned short port, unsigned int size_bytes)
{
    return rdma_mem_start_ex(mode, ip, port, size_bytes, 0);
}

RDMA_TRANSFER_API int rdma_mem_start_ex(int mode, const char* ip, unsigned short port, unsigned int size_bytes, int use_read)
{
    if (g_mem.mode >= 0) { SET_ERROR("Memory share already active"); return -1; }
    if (mode != 0 && mode != 1) { SET_ERROR("mode must be 0 (display) or 1 (writer)"); return -1; }
    if (size_bytes < 1 || size_bytes > 64 * 1024 * 1024) { SET_ERROR("size must be 1..64MB"); return -1; }

    MemCleanup();
    g_mem.mode = mode;
    g_mem.bufSize = size_bytes;
    g_mem.useRead = (use_read != 0);
    ResetCancel();

    // Read mode reverses listener/connector roles:
    //   Write mode: display listens (owns buffer), writer connects
    //   Read mode:  writer listens (owns buffer), display connects
    bool isListener = (g_mem.useRead ? (mode == 1) : (mode == 0));

    NdContext& ctx = g_mem.ctx;
    struct sockaddr_in localAddr = {};
    struct sockaddr_in remoteAddr = {};

    HRESULT hr;
    if (isListener) {
        if (ParseIpv4(ip, port, &localAddr) != 0) { MemCleanup(); return -1; }
    } else {
        if (ParseIpv4(ip, port, &remoteAddr) != 0) { MemCleanup(); return -1; }
        SIZE_T addrLen = sizeof(localAddr);
        hr = NdResolveAddress((const sockaddr*)&remoteAddr, sizeof(remoteAddr),
                               (sockaddr*)&localAddr, &addrLen);
        if (FAILED(hr)) { MemCleanup(); return -1; }
    }

    hr = NdOpenAdapter(IID_IND2Adapter, (const sockaddr*)&localAddr, sizeof(localAddr),
                       (void**)&ctx.pAdapter);
    if (FAILED(hr)) { MemCleanup(); return -1; }

    ctx.ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!ctx.ov.hEvent) { MemCleanup(); return -1; }
    hr = ctx.pAdapter->CreateOverlappedFile(&ctx.hOvFile);
    if (FAILED(hr)) { MemCleanup(); return -1; }

    hr = ctx.pAdapter->CreateCompletionQueue(IID_IND2CompletionQueue, ctx.hOvFile,
        CQ_DEPTH, 0, 0, (void**)&ctx.pCq);
    if (FAILED(hr)) { MemCleanup(); return -1; }

    hr = ctx.pAdapter->CreateConnector(IID_IND2Connector, ctx.hOvFile, (void**)&ctx.pConnector);
    if (FAILED(hr)) { MemCleanup(); return -1; }

    hr = ctx.pAdapter->CreateQueuePair(IID_IND2QueuePair, ctx.pCq, ctx.pCq, nullptr,
        QP_DEPTH, QP_DEPTH, 1, 1, 0, (void**)&ctx.pQp);
    if (FAILED(hr)) { MemCleanup(); return -1; }

    hr = ctx.pAdapter->CreateMemoryRegion(IID_IND2MemoryRegion, ctx.hOvFile, (void**)&ctx.pMr);
    if (FAILED(hr)) { MemCleanup(); return -1; }

    // Layout: [MemSetupInfo] [MemDoorbellRecv] [MemDoorbellSend] [staging: CHUNK_SIZE] [shared_data: size_bytes]
    const size_t ctrlSize = sizeof(MemSetupInfo) + sizeof(MemDoorbellMsg) * 2;
    const size_t stagingSize = CHUNK_SIZE;
    const size_t totalSize = ctrlSize + stagingSize + size_bytes;
    g_mem.pBuf = AllocLarge(totalSize);
    if (!g_mem.pBuf) { MemCleanup(); return -1; }

    {
        ULONG mrFlags = ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE;
        if (g_mem.useRead) mrFlags |= ND_MR_FLAG_ALLOW_REMOTE_READ;
        hr = ctx.pMr->Register(g_mem.pBuf, totalSize, mrFlags, &ctx.ov);
    }
    if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pMr, &ctx.ov);
    if (FAILED(hr)) { MemCleanup(); return -1; }
    g_mem.localToken = ctx.pMr->GetLocalToken();

    // Setup pointers
    char* base = (char*)g_mem.pBuf;
    MemSetupInfo* pSetup = (MemSetupInfo*)base;
    MemDoorbellMsg* pDoorbellRecv = (MemDoorbellMsg*)(base + sizeof(MemSetupInfo));
    g_mem.pDoorbellSend = (MemDoorbellMsg*)(base + sizeof(MemSetupInfo) + sizeof(MemDoorbellMsg));
    char* staging = base + ctrlSize;
    g_mem.pLocalBuf = base + ctrlSize + stagingSize;
    memset(g_mem.pLocalBuf, 0, size_bytes);

    // Set up doorbell Recv buffer (in MR so Write won't access-violation)
    // The Recv SGE points to pDoorbellRecv which is inside the MR

    if (isListener) {
        // ====== LISTENER (Write mode display or Read mode writer) ======
        hr = ctx.pAdapter->CreateListener(IID_IND2Listener, ctx.hOvFile, (void**)&ctx.pListener);
        if (FAILED(hr)) { MemCleanup(); return -1; }

        hr = ctx.pListener->Bind((const sockaddr*)&localAddr, sizeof(localAddr));
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pListener, &ctx.ov);
        if (FAILED(hr)) { MemCleanup(); return -1; }
        hr = ctx.pListener->Listen(1);
        if (FAILED(hr)) { MemCleanup(); return -1; }

        ND2_SGE dbSge = {}; dbSge.Buffer = pDoorbellRecv;
        dbSge.BufferLength = sizeof(MemDoorbellMsg); dbSge.MemoryRegionToken = g_mem.localToken;
        hr = ctx.pQp->Receive(MEM_DOORBELL_RECV_CTX, &dbSge, 1);
        if (FAILED(hr)) { MemCleanup(); return -1; }

        hr = ctx.pListener->GetConnectionRequest(ctx.pConnector, &ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pListener, &ctx.ov);
        if (FAILED(hr)) { MemCleanup(); return -1; }

        hr = ctx.pConnector->Accept(ctx.pQp, g_mem.useRead ? 16 : 0, 0, nullptr, 0, &ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        if (FAILED(hr)) { MemCleanup(); return -1; }

        // Send our buffer info to connector
        pSetup->remote_addr  = (UINT64)g_mem.pLocalBuf;
        pSetup->remote_token = ctx.pMr->GetRemoteToken();
        pSetup->size         = size_bytes;
        ND2_SGE setupSge = {}; setupSge.Buffer = pSetup;
        setupSge.BufferLength = sizeof(MemSetupInfo); setupSge.MemoryRegionToken = g_mem.localToken;
        hr = ctx.pQp->Send(MEM_SETUP_SEND_CTX, &setupSge, 1, 0);
        if (FAILED(hr)) { MemCleanup(); return -1; }
        ND2_RESULT cqResult = {};
        while (ctx.pCq->GetResults(&cqResult, 1) == 0 && !IsCancelled()) {}
        if (IsCancelled() || cqResult.Status != ND_SUCCESS) { MemCleanup(); return -1; }

        DbgLog(g_mem.useRead ? "MEM: writer ready (Read mode)" : "MEM: display ready");
    } else {
        // ====== CONNECTOR (Write mode writer or Read mode display) ======
        hr = ctx.pConnector->Bind((const sockaddr*)&localAddr, sizeof(localAddr));
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        if (FAILED(hr)) { MemCleanup(); return -1; }

        ND2_SGE setupSge = {}; setupSge.Buffer = pSetup;
        setupSge.BufferLength = sizeof(MemSetupInfo); setupSge.MemoryRegionToken = g_mem.localToken;
        hr = ctx.pQp->Receive(MEM_SETUP_RECV_CTX, &setupSge, 1);
        if (FAILED(hr)) { MemCleanup(); return -1; }

        hr = ctx.pConnector->Connect(ctx.pQp, (const sockaddr*)&remoteAddr, sizeof(remoteAddr),
                                      g_mem.useRead ? 0 : 0, g_mem.useRead ? 16 : 0, nullptr, 0, &ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        if (FAILED(hr)) { MemCleanup(); return -1; }

        hr = ctx.pConnector->CompleteConnect(&ctx.ov);
        if (hr == ND_PENDING) hr = WaitOverlapped(ctx.pConnector, &ctx.ov);
        if (FAILED(hr)) { MemCleanup(); return -1; }

        ND2_RESULT cqResult = {};
        if (!WaitForCompletion(ctx.pCq, &ctx.ov, cqResult)) { MemCleanup(); return -1; }
        if (cqResult.Status != ND_SUCCESS) { MemCleanup(); return -1; }

        g_mem.remoteAddr  = pSetup->remote_addr;
        g_mem.remoteToken = pSetup->remote_token;
        DbgLog(g_mem.useRead ? "MEM: display connected" : "MEM: writer connected");
    }

    g_mem.connected = true;
    return 0;
}

RDMA_TRANSFER_API int rdma_mem_write(unsigned int offset, const void* data, unsigned int len)
{
    if (!g_mem.connected || g_mem.mode != 1) { SET_ERROR("Not connected as writer"); return -1; }
    if (offset + len > g_mem.bufSize) { SET_ERROR("Write exceeds buffer"); return -1; }
    if (len > CHUNK_SIZE) { SET_ERROR("Write exceeds max size (4MB)"); return -1; }

    // Read mode: just copy to local buffer, display will RDMA Read
    if (g_mem.useRead) {
        memcpy((char*)g_mem.pLocalBuf + offset, data, len);
        return 0;
    }

    NdContext& ctx = g_mem.ctx;

    // Staging buffer is at g_mem.pBuf + ctrlSize. Copy user data there.
    const size_t ctrlSize = sizeof(MemSetupInfo) + sizeof(MemDoorbellMsg) * 2;
    char* staging = (char*)g_mem.pBuf + ctrlSize;
    memcpy(staging, data, len);

    // RDMA Write from staging buffer into display's shared buffer
    UINT64 remoteAddr = g_mem.remoteAddr + offset;
    ND2_SGE writeSge = {}; writeSge.Buffer = staging;
    writeSge.BufferLength = len; writeSge.MemoryRegionToken = g_mem.localToken;
    HRESULT hr = ctx.pQp->Write(MEM_WRITE_CTX, &writeSge, 1,
        remoteAddr, g_mem.remoteToken, ND_OP_FLAG_SILENT_SUCCESS);
    if (FAILED(hr)) { SET_ERROR("MEM Write failed"); return -1; }

    // Send doorbell
    g_mem.pDoorbellSend->offset = offset;
    g_mem.pDoorbellSend->length = len;
    ND2_SGE dbSge = {}; dbSge.Buffer = g_mem.pDoorbellSend;
    dbSge.BufferLength = sizeof(MemDoorbellMsg); dbSge.MemoryRegionToken = g_mem.localToken;
    hr = ctx.pQp->Send(MEM_DOORBELL_SEND_CTX, &dbSge, 1, 0);
    if (FAILED(hr)) { SET_ERROR("MEM doorbell failed"); return -1; }

    ND2_RESULT cqResult = {};
    while (ctx.pCq->GetResults(&cqResult, 1) == 0 && !IsCancelled()) {}
    if (IsCancelled() || cqResult.Status != ND_SUCCESS) { return -1; }
    return 0;
}

RDMA_TRANSFER_API const void* rdma_mem_buffer(void)
{
    if (!g_mem.connected) return nullptr;
    return g_mem.pLocalBuf;
}

RDMA_TRANSFER_API unsigned int rdma_mem_size(void)
{
    return g_mem.bufSize;
}

RDMA_TRANSFER_API int rdma_mem_wait(int timeout_ms)
{
    if (!g_mem.connected || g_mem.mode != 0) { SET_ERROR("Not connected as display"); return -1; }

    NdContext& ctx = g_mem.ctx;

    // Read mode: RDMA Read the entire buffer from writer, blocking
    if (g_mem.useRead) {
        ND2_SGE readSge = {}; readSge.Buffer = g_mem.pLocalBuf;
        readSge.BufferLength = g_mem.bufSize; readSge.MemoryRegionToken = g_mem.localToken;
        HRESULT hr = ctx.pQp->Read(MEM_WRITE_CTX, &readSge, 1,
            g_mem.remoteAddr, g_mem.remoteToken, 0);
        if (FAILED(hr)) { SET_ERROR("MEM Read failed"); return -1; }
        ND2_RESULT r = {};
        while (ctx.pCq->GetResults(&r, 1) == 0) { if (IsCancelled()) return -1; Sleep(1); }
        return (r.Status == ND_SUCCESS) ? 1 : -1;
    }

    ND2_RESULT cqResult = {};

    int elapsed = 0;
    while (true) {
        ULONG got = ctx.pCq->GetResults(&cqResult, 1);
        if (got > 0) break;
        if (IsCancelled()) return -1;
        if (timeout_ms > 0) {
            Sleep(1);
            if (++elapsed >= timeout_ms) return 0;
        } else if (timeout_ms == 0) {
            Sleep(1); // infinite wait — yield
        } else {
            return 0; // non-blocking
        }
    }

    if (cqResult.Status != ND_SUCCESS) {
        DbgLog("MEM: doorbell error");
        return -1;
    }
    if (cqResult.RequestContext != MEM_DOORBELL_RECV_CTX) {
        DbgLog("MEM: unexpected CQ context");
        return -1;
    }

    // Save doorbell info
    MemDoorbellMsg* pDoorbellRecv = (MemDoorbellMsg*)((MemSetupInfo*)g_mem.pBuf + 1);
    g_mem.lastDoorbell = *pDoorbellRecv;

    // Re-post doorbell Recv for next update
    ND2_SGE dbSge = {}; dbSge.Buffer = pDoorbellRecv;
    dbSge.BufferLength = sizeof(MemDoorbellMsg); dbSge.MemoryRegionToken = g_mem.localToken;
    ctx.pQp->Receive(MEM_DOORBELL_RECV_CTX, &dbSge, 1);

    return 1;
}

RDMA_TRANSFER_API void rdma_mem_last_write(unsigned int* out_offset, unsigned int* out_len)
{
    if (out_offset) *out_offset = g_mem.lastDoorbell.offset;
    if (out_len)    *out_len    = g_mem.lastDoorbell.length;
}

RDMA_TRANSFER_API void rdma_mem_stop(void)
{
    if (g_mem.mode < 0) return;
    // Set cancel flag so any blocked rdma_mem_wait exits, then
    // yield briefly to let the other thread finish before we destroy resources.
    InterlockedExchange(&g_cancel_flag, 1);
    Sleep(10);
    ResetCancel();
    MemCleanup();
    DbgLog("MEM: stopped");
}

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
    g_is_read = 0;
    return InternalSend(remote_ip, port, file_path);
}

RDMA_TRANSFER_API int rdma_send_file_ex(const char* remote_ip, unsigned short port, const wchar_t* file_path, int use_read)
{
    g_is_read = use_read;
    int r = InternalSend(remote_ip, port, file_path);
    g_is_read = 0;
    return r;
}

RDMA_TRANSFER_API int rdma_recv_file(const char* local_ip, unsigned short port, const wchar_t* output_path)
{
    g_is_read = 0;
    return InternalRecv(local_ip, port, output_path);
}

RDMA_TRANSFER_API int rdma_recv_file_ex(const char* local_ip, unsigned short port, const wchar_t* output_path, int use_read)
{
    g_is_read = use_read;
    int r = InternalRecv(local_ip, port, output_path);
    g_is_read = 0;
    return r;
}

RDMA_TRANSFER_API int rdma_bench(int side, const char* ip, unsigned short port, int size_mb)
{
    return rdma_bench_ex(side, ip, port, size_mb, 0);
}

RDMA_TRANSFER_API int rdma_bench_ex(int side, const char* ip, unsigned short port, int size_mb, int use_read)
{
    g_is_read = use_read;
    int r;
    if (side == 1)
        r = InternalBenchSend(ip, port, size_mb);
    else
        r = InternalBenchRecv(ip, port, size_mb);
    g_is_read = 0;
    return r;
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
