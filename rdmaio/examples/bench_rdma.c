// bench_rdma.c — simple RDMA Write benchmark tool using rdma_transfer.dll
// Receiver: bench_rdma.exe -recv <ip> <port> <size_mb>
// Sender:   bench_rdma.exe -send <ip> <port> <size_mb>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdma_transfer.h"
#pragma comment(lib, "rdma_transfer.lib")

static void on_progress(double pct, double speed, void* ctx) {
    (void)ctx;
    printf("\r  %.1f%%  (%.0f MB/s)", pct, speed);
    fflush(stdout);
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        printf("Usage:\n");
        printf("  %s -recv <ip> <port> <size_mb>   -- receiver\n", argv[0]);
        printf("  %s -send <ip> <port> <size_mb>   -- sender\n", argv[0]);
        return 1;
    }

    int is_send = (strcmp(argv[1], "-send") == 0);
    int is_recv = (strcmp(argv[1], "-recv") == 0);
    if (!is_send && !is_recv) { printf("ERROR: specify -send or -recv\n"); return 1; }

    const char* ip = argv[2];
    int port = atoi(argv[3]);
    int size_mb = atoi(argv[4]);

    if (rdma_transfer_init() != 0) { printf("ERROR: init failed\n"); return 1; }
    rdma_set_progress_callback(on_progress, NULL);

    printf("RDMA Write benchmark: %d MB\n", size_mb);
    printf("  Side: %s\n", is_send ? "SENDER" : "RECEIVER");
    printf("  IP:   %s:%d\n", ip, port);
    printf("\n");

    int ret = rdma_bench(is_send ? 1 : 0, ip, (unsigned short)port, size_mb);

    rdma_transfer_cleanup();
    printf("\nBenchmark %s (exit code %d)\n", ret == 0 ? "OK" : "FAILED", ret);
    return ret;
}
