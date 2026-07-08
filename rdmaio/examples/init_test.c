#include <stdint.h>
#include <windows.h>
#include <stdio.h>
#include "rdma_transfer.h"
#pragma comment(lib, "rdma_transfer.lib")
int main() {
    printf("init=%d\n", rdma_transfer_init());
    printf("adapters=%d\n", rdma_list_adapters(NULL, 0));
    rdma_transfer_cleanup();
    return 0;
}
