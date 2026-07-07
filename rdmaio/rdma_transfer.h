// rdma_transfer.h — Public C API for RDMA file transfer over NetworkDirect (NDSPI)
//
// Usage from any C/C++ project:
//   1. #include "rdma_transfer.h"
//   2. Link against rdma_transfer.lib
//   3. Copy rdma_transfer.dll next to your .exe
//
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RDMA_TRANSFER_API
#ifdef RDMA_TRANSFER_EXPORTS
#define RDMA_TRANSFER_API __declspec(dllexport)
#else
#define RDMA_TRANSFER_API __declspec(dllimport)
#endif
#endif

// ---- Lifecycle -----------------------------------------------------------
// Call once per process before any transfer.
// Returns 0 on success, -1 on failure.
RDMA_TRANSFER_API int rdma_transfer_init(void);

// Call once per process when done.
RDMA_TRANSFER_API void rdma_transfer_cleanup(void);

// ---- File Transfer -------------------------------------------------------
// Send a file to a remote peer.  Blocks until transfer completes or fails.
//   remote_ip  — IPv4 string, e.g. "192.168.100.2"
//   port       — TCP/UDP port number for ND connection (e.g. 54321)
//   file_path  — absolute path to the input file
// Returns 0 on success, -1 on error.
RDMA_TRANSFER_API int rdma_send_file(
    const char*  remote_ip,
    unsigned short port,
    const wchar_t* file_path);

// Receive a file from a remote peer.  Blocks until connection and transfer.
//   local_ip   — IPv4 string of the local RDMA interface to listen on
//   port       — same port as the sender
//   output_path— where to write the received file
// Returns 0 on success, -1 on error.
RDMA_TRANSFER_API int rdma_recv_file(
    const char*  local_ip,
    unsigned short port,
    const wchar_t* output_path);

// ---- Pure-RDMA Benchmark -------------------------------------------------
// Run a memory-to-memory RDMA Write throughput test (no disk I/O).
//   side       — 0 = receiver, 1 = sender
//   ip         — local IP (recv) or remote IP (send)
//   port       — connection port
//   size_mb    — total transfer size in MiB (receiver pre-allocates this)
// Returns 0 on success, -1 on error.
// On sender, prints throughput stats to stdout.
RDMA_TRANSFER_API int rdma_bench(
    int           side,        // 0 = recv, 1 = send
    const char*   ip,
    unsigned short port,
    int           size_mb);

// ---- Progress Callback ---------------------------------------------------
// Signature: void callback(double percent, double speed_mbps, void* user_data)
typedef void (*rdma_progress_cb)(double percent, double speed_mbps, void* user_data);

// Set a progress callback.  Called after each chunk completes.
// Pass NULL to disable.
RDMA_TRANSFER_API void rdma_set_progress_callback(rdma_progress_cb cb, void* user_data);

// ---- Metadata Callback ---------------------------------------------------
// Called on the receiver side when the sender's metadata arrives.
//   filename  — original file name (ANSI, null-terminated)
//   file_size — total file size in bytes
//   user_data — opaque pointer passed to rdma_set_metadata_callback
typedef void (*rdma_metadata_cb)(const char* filename, uint64_t file_size, void* user_data);

RDMA_TRANSFER_API void rdma_set_metadata_callback(rdma_metadata_cb cb, void* user_data);

// ---- Error Diagnostics ---------------------------------------------------
// After any transfer function returns -1, call this to get a human-readable
// error message describing where the failure occurred.
// Result points to a static buffer valid until the next rdma_* call.
RDMA_TRANSFER_API const char* rdma_transfer_last_error(void);

// ---- Cancel --------------------------------------------------------------
// Cancel any in-progress rdma_send_file / rdma_recv_file / rdma_bench call
// from another thread.  The blocked call will return -1.
// Idempotent; safe to call when nothing is running.
RDMA_TRANSFER_API void rdma_transfer_cancel(void);

// ---- Metadata Callback ---------------------------------------------------
// Called when file metadata is sent or received.
//   filename  — original file name (null-terminated ANSI string)
//   file_size — total file size in bytes
// This allows the receiver to know the sender's file name before data arrives.
typedef void (*rdma_metadata_cb)(const char* filename, uint64_t file_size, void* user_data);

// Set a metadata callback.  Pass NULL to disable.
RDMA_TRANSFER_API void rdma_set_metadata_callback(rdma_metadata_cb cb, void* user_data);

// ---- Adapter Enumeration -------------------------------------------------
// Info for one RDMA-capable adapter / address.
typedef struct rdma_adapter_info {
    char     ip_address[64];     // IPv4 string, e.g. "192.168.100.2"
    UINT64   adapter_id;         // ND2_ADAPTER_INFO.AdapterId
    UINT16   vendor_id;          // ND2_ADAPTER_INFO.VendorId
    UINT16   device_id;          // ND2_ADAPTER_INFO.DeviceId
    UINT32   max_transfer_mb;    // ND2_ADAPTER_INFO.MaxTransferLength in MiB
    UINT32   max_inline_data;    // ND2_ADAPTER_INFO.MaxInlineDataSize
    UINT32   max_cq_depth;       // ND2_ADAPTER_INFO.MaxCompletionQueueDepth
    UINT32   max_initiator_depth;// ND2_ADAPTER_INFO.MaxInitiatorQueueDepth
    UINT32   flags;              // ND2_ADAPTER_INFO.AdapterFlags
    int      has_in_order_dma;   // ND_ADAPTER_FLAG_IN_ORDER_DMA_SUPPORTED
    int      has_multi_engine;   // ND_ADAPTER_FLAG_MULTI_ENGINE_SUPPORTED
    int      has_loopback;       // ND_ADAPTER_FLAG_LOOPBACK_CONNECTIONS_SUPPORTED
} rdma_adapter_info;

// Enumerate all RDMA-capable adapters on the system.
//   info      — caller-allocated array, or NULL to just get count
//   max_count — size of the info array (ignored if info is NULL)
// Returns: number of adapters found, or -1 on error.
// Call twice: once with info=NULL to get count, then with a buffer.
RDMA_TRANSFER_API int rdma_list_adapters(
    rdma_adapter_info* info,
    int                max_count);

#ifdef __cplusplus
}
#endif
