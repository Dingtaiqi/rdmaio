// mem_demo.c — RDMA shared memory demo
// Display: mem_demo.exe -display <ip> <port> <size>
// Writer:  mem_demo.exe -write <ip> <port> <size>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdma_transfer.h"
#pragma comment(lib, "rdma_transfer.lib")

static int running = 1;

int main(int argc, char* argv[]) {
    if (argc < 5) {
        printf("Usage:\n  %s -display <ip> <port> <size>\n  %s -write <ip> <port> <size>\n", argv[0], argv[0]);
        return 1;
    }
    int isDisplay = (strcmp(argv[1], "-display") == 0);
    int isWriter = (strcmp(argv[1], "-write") == 0);
    const char* ip = argv[2];
    int port = atoi(argv[3]);
    int size = atoi(argv[4]);

    if (rdma_transfer_init() != 0) { printf("Init failed\n"); return 1; }

    printf("Starting %s on %s:%d buffer=%d bytes\n",
           isDisplay ? "DISPLAY" : "WRITER", ip, port, size);

    if (rdma_mem_start(isWriter ? 1 : 0, ip, (unsigned short)port, size) != 0) {
        printf("rdma_mem_start failed: %s\n", rdma_transfer_last_error());
        rdma_transfer_cleanup();
        return 1;
    }

    if (isDisplay) {
        printf("Display mode: waiting for data...\n");
        unsigned char* buf = (unsigned char*)rdma_mem_buffer();
        unsigned int bufSize = rdma_mem_size();
        int iter = 0;
        while (running) {
            int ret = rdma_mem_wait(1000);  // 1s timeout
            if (ret == 1) {
                unsigned int off, len;
                rdma_mem_last_write(&off, &len);
                printf("\r[%d] write@%u +%u bytes  ", ++iter, off, len);
                // Show first 16 bytes of buffer
                printf("data: ");
                for (int i = 0; i < 16 && i < bufSize; i++)
                    printf("%02X ", buf[i]);
                printf(" | ");
                for (int i = 0; i < 16 && i < bufSize; i++)
                    printf("%c", buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.');
                fflush(stdout);
            } else if (ret == 0) {
                // timeout — just show buffer state
                if (iter > 0) {
                    printf("\r[%d] timeout, buf[0..3]=%02X%02X%02X%02X\n",
                           ++iter, buf[0], buf[1], buf[2], buf[3]);
                }
            } else {
                printf("\nError or stopped\n");
                break;
            }
            if (iter > 20) running = 0; // stop after 20 updates
        }
    } else {
        // Writer: send some data patterns
        char buf[4096];
        for (int i = 0; i < 5; i++) {
            memset(buf, 'A' + i, sizeof(buf));
            printf("Writing block %d: %d x '%c'\n", i, (int)sizeof(buf), 'A' + i);
            if (rdma_mem_write(0, buf, sizeof(buf)) != 0) {
                printf("Write failed\n");
                break;
            }
            Sleep(500);
        }
        // Write a small message at offset 0
        const char* msg = "RDMA Write Zero-Copy!";
        rdma_mem_write(0, msg, (unsigned int)strlen(msg) + 1);
        printf("Final write: \"%s\"\n", msg);
    }

    rdma_mem_stop();
    rdma_transfer_cleanup();
    printf("\nDone\n");
    return 0;
}
