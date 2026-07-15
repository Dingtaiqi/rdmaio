#include <stdio.h>
#include <windows.h>
#include "rdma_transfer.h"
#pragma comment(lib, "rdma_transfer.lib")
#pragma comment(lib, "Ws2_32.lib")

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage:\n  test_read.exe bench <ip> <port> <mb>         -- RDMA Read bench (recv first, then send)\n");
        printf("  test_read.exe bench_recv <ip> <port> <mb>\n");
        printf("  test_read.exe bench_send <ip> <port> <mb>\n");
        printf("  test_read.exe mem <ip> <port> <size>\n");
        return 1;
    }

    if (rdma_transfer_init() != 0) { printf("Init failed\n"); return 1; }

    const char* ip = argv[2];
    int port = atoi(argv[3]);

    if (strcmp(argv[1], "recv") == 0) {
        int mb = atoi(argv[4]);
        printf("RDMA Read bench recv %s:%d %dMB\n", ip, port, mb);
        int r = rdma_bench_ex(0, ip, (unsigned short)port, mb, 1);
        printf("Result: %d\n", r);
    }
    else if (strcmp(argv[1], "send") == 0) {
        int mb = atoi(argv[4]);
        printf("RDMA Read bench send %s:%d %dMB\n", ip, port, mb);
        int r = rdma_bench_ex(1, ip, (unsigned short)port, mb, 1);
        printf("Result: %d\n", r);
    }

    rdma_transfer_cleanup();
    return 0;
}
