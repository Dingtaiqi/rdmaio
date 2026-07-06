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

#ifdef __cplusplus
}
#endif
