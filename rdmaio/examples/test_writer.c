#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "rdma_transfer.h"
#pragma comment(lib, "rdma_transfer.lib")

int main(int argc, char** argv) {
    printf("init...\n"); fflush(stdout);
    if (rdma_transfer_init() != 0) { printf("init failed\n"); return 1; }

    printf("start writer...\n"); fflush(stdout);
    if (rdma_mem_start(1, "192.168.100.2", 54325, 65536) != 0) {
        printf("start failed: %s\n", rdma_transfer_last_error()); fflush(stdout);
        rdma_transfer_cleanup();
        return 1;
    }

    printf("write...\n"); fflush(stdout);
    char buf[1024];
    memset(buf, 'X', sizeof(buf));
    int r = rdma_mem_write(0, buf, sizeof(buf));
    printf("write returned %d\n", r); fflush(stdout);

    printf("stop...\n"); fflush(stdout);
    rdma_mem_stop();
    printf("stop done\n"); fflush(stdout);

    rdma_transfer_cleanup();
    printf("cleanup done\n"); fflush(stdout);
    return 0;
}
