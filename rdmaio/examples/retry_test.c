#include <stdint.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdma_transfer.h"
#pragma comment(lib, "rdma_transfer.lib")

int main(int argc, char* argv[]) {
    if (argc != 5) {
        printf("Usage: %s <ip> <send_file> <recv_output> <rounds>\n", argv[0]);
        return 1;
    }
    const char* ip = argv[1];
    int rounds = atoi(argv[4]);
    if (rounds < 1) rounds = 1;

    // Make wide paths
    int len = MultiByteToWideChar(CP_ACP, 0, argv[2], -1, NULL, 0);
    wchar_t* send_path = malloc(len * sizeof(wchar_t));
    MultiByteToWideChar(CP_ACP, 0, argv[2], -1, send_path, len);
    len = MultiByteToWideChar(CP_ACP, 0, argv[3], -1, NULL, 0);
    wchar_t* recv_path = malloc(len * sizeof(wchar_t));
    MultiByteToWideChar(CP_ACP, 0, argv[3], -1, recv_path, len);

    rdma_transfer_init();

    for (int r = 0; r < rounds; r++) {
        printf("=== Round %d ===\n", r+1);
        // start a sender — but we also need a receiver running
        // this test is sender-only; user must run receiver separately
        int ret = rdma_send_file(ip, 54321, send_path);
        printf("Round %d: %s (err: %s)\n", r+1,
               ret == 0 ? "OK" : "FAIL", rdma_transfer_last_error());
        if (ret != 0) {
            printf("FAILED on round %d\n", r+1);
            rdma_transfer_cleanup();
            free(send_path);
            free(recv_path);
            return 1;
        }
        Sleep(500); // let receiver finish
    }

    printf("ALL %d ROUNDS PASSED\n", rounds);
    rdma_transfer_cleanup();
    free(send_path);
    free(recv_path);
    return 0;
}
