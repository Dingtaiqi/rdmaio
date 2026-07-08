#include <stdint.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "rdma_transfer.h"
#pragma comment(lib, "rdma_transfer.lib")

static void progress(double pct, double speed, void* ctx) {
    printf("\r%.1f%% %.1fMB/s", pct, speed); fflush(stdout);
}

int main(int argc, char* argv[]) {
    if (argc < 4) { printf("Usage: %s <send|recv> <ip> <path> [port]\n", argv[0]); return 1; }
    int isSend = (argv[1][0] == 's');
    int port = (argc >= 5) ? atoi(argv[4]) : 54399;

    int len = MultiByteToWideChar(CP_ACP, 0, argv[3], -1, NULL, 0);
    wchar_t* path = malloc(len * sizeof(wchar_t));
    MultiByteToWideChar(CP_ACP, 0, argv[3], -1, path, len);

    if (rdma_transfer_init() != 0) { printf("INIT FAILED\n"); free(path); return 1; }
    rdma_set_progress_callback(progress, NULL);

    int ret = isSend ? rdma_send_file(argv[2], (unsigned short)port, path)
                     : rdma_recv_file(argv[2], (unsigned short)port, path);
    printf("\n%s code=%d err=%s\n", ret==0?"OK":"FAIL", ret, rdma_transfer_last_error());
    rdma_transfer_cleanup();
    free(path);
    return ret;
}
