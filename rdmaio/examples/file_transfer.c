// examples/file_transfer.c — rdma_transfer.dll file send/receive example
//
// Build: see build_examples.bat
//
// Receiver:  file_transfer.exe -recv 192.168.100.2 54321 output.bin
// Sender:    file_transfer.exe -send 192.168.100.2 54321 input.bin

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdma_transfer.h"
#pragma comment(lib, "rdma_transfer.lib")

static void on_progress(double pct, double speed_mbps, void* ctx)
{
    (void)ctx;
    printf("\r  %.1f%%  (%.1f MB/s)", pct, speed_mbps);
    fflush(stdout);
}

int main(int argc, char* argv[])
{
    if (argc < 4) {
        printf("Usage:\n");
        printf("  %s -send <ip> <port> <file>       -- send a file\n", argv[0]);
        printf("  %s -recv <ip> <port> <output>      -- receive a file\n", argv[0]);
        return 1;
    }

    int is_send = (strcmp(argv[1], "-send") == 0);
    int is_recv = (strcmp(argv[1], "-recv") == 0);
    if (!is_send && !is_recv) {
        printf("ERROR: specify -send or -recv\n");
        return 1;
    }
    if (argc != 5) {
        printf("ERROR: need <ip> <port> <path>\n");
        return 1;
    }

    const char* ip   = argv[2];
    int         port = atoi(argv[3]);

    if (rdma_transfer_init() != 0) {
        printf("ERROR: rdma_transfer_init failed\n");
        return 1;
    }

    rdma_set_progress_callback(on_progress, NULL);

    int ret;
    if (is_send) {
        // Convert narrow path to wide
        const char* narrow = argv[4];
        int len = MultiByteToWideChar(CP_ACP, 0, narrow, -1, NULL, 0);
        wchar_t* wide = (wchar_t*)malloc(len * sizeof(wchar_t));
        MultiByteToWideChar(CP_ACP, 0, narrow, -1, wide, len);

        printf("Sending %s to %s:%d ...\n", narrow, ip, port);
        ret = rdma_send_file(ip, (unsigned short)port, wide);
        free(wide);
    } else {
        const char* narrow = argv[4];
        int len = MultiByteToWideChar(CP_ACP, 0, narrow, -1, NULL, 0);
        wchar_t* wide = (wchar_t*)malloc(len * sizeof(wchar_t));
        MultiByteToWideChar(CP_ACP, 0, narrow, -1, wide, len);

        printf("Listening on %s:%d ...\n", ip, port);
        ret = rdma_recv_file(ip, (unsigned short)port, wide);
        free(wide);
    }

    rdma_transfer_cleanup();
    printf("\nTransfer %s (exit code %d)\n", ret == 0 ? "OK" : "FAILED", ret);
    return ret;
}
