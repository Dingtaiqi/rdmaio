// In-process loopback test: receiver thread + sender thread.
// Tests cancel → retry with the same file.
#include <stdint.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include "rdma_transfer.h"
#pragma comment(lib, "rdma_transfer.lib")

static volatile int g_recv_ready = 0;
static volatile int g_send_done  = 0;
static volatile int g_send_ret   = 0;
static const char* g_ip = NULL;
static wchar_t* g_file = NULL;
static wchar_t* g_out  = NULL;

static unsigned __stdcall recv_thread(void* arg) {
    g_recv_ready = 0;
    int ret = rdma_recv_file(g_ip, 54321, g_out);
    g_recv_ready = 0;
    return ret;
}

static unsigned __stdcall send_thread(void* arg) {
    int ret = rdma_send_file(g_ip, 54321, g_file);
    g_send_ret = ret;
    g_send_done = 1;
    return ret;
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        printf("Usage: %s <ip> <send_file> <recv_output>\n", argv[0]);
        return 1;
    }
    g_ip = argv[1];

    int len = MultiByteToWideChar(CP_ACP, 0, argv[2], -1, NULL, 0);
    g_file = malloc(len * sizeof(wchar_t));
    MultiByteToWideChar(CP_ACP, 0, argv[2], -1, g_file, len);

    len = MultiByteToWideChar(CP_ACP, 0, argv[3], -1, NULL, 0);
    g_out = malloc(len * sizeof(wchar_t));
    MultiByteToWideChar(CP_ACP, 0, argv[3], -1, g_out, len);

    rdma_transfer_init();

    for (int round = 1; round <= 2; round++) {
        printf("=== Round %d ===\n", round);
        DeleteFileW(g_out);

        // Start receiver thread
        uintptr_t rt = _beginthreadex(NULL, 0, recv_thread, NULL, 0, NULL);
        Sleep(1500); // wait for listener

        if (round == 1) {
            // Round 1: send normally, cancel after 800ms
            uintptr_t st = _beginthreadex(NULL, 0, send_thread, NULL, 0, NULL);
            Sleep(800);
            printf("  Cancelling sender...\n");
            rdma_transfer_cancel();
            WaitForSingleObject((HANDLE)st, 5000);
            CloseHandle((HANDLE)st);
            printf("  Round 1 sender returned %d\n", g_send_ret);
            // Wait for receiver to finish
            WaitForSingleObject((HANDLE)rt, 5000);
            CloseHandle((HANDLE)rt);
            printf("  Round 1 receiver finished\n");
        } else {
            // Round 2: send normally (no cancel)
            uintptr_t st = _beginthreadex(NULL, 0, send_thread, NULL, 0, NULL);
            WaitForSingleObject((HANDLE)st, 30000);
            CloseHandle((HANDLE)st);
            WaitForSingleObject((HANDLE)rt, 10000);
            CloseHandle((HANDLE)rt);

            if (g_send_ret == 0) {
                printf("  Round 2 OK\n");
            } else {
                printf("  Round 2 FAILED: err=%s\n", rdma_transfer_last_error());
                return 1;
            }
        }
        Sleep(500);
    }

    printf("PASS\n");
    free(g_file); free(g_out);
    return 0;
}
