// examples/list_adapters.c — rdma_transfer.dll usage example
//
// Build: see build_examples.bat
// Run:   list_adapters.exe
//
// Lists all RDMA-capable network adapters on the system.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "rdma_transfer.h"
#pragma comment(lib, "rdma_transfer.lib")

int main(void)
{
    printf("=== RDMA-Capable Adapters ===\n\n");

    if (rdma_transfer_init() != 0) {
        printf("ERROR: rdma_transfer_init failed. Is RDMA driver installed?\n");
        return 1;
    }

    int count = rdma_list_adapters(NULL, 0);
    if (count <= 0) {
        printf("No RDMA adapters found.\n");
        rdma_transfer_cleanup();
        return count < 0 ? 1 : 0;
    }

    printf("Found %d adapter(s):\n\n", count);

    rdma_adapter_info* list = (rdma_adapter_info*)calloc(count, sizeof(rdma_adapter_info));
    if (!list) {
        printf("ERROR: out of memory\n");
        rdma_transfer_cleanup();
        return 1;
    }

    rdma_list_adapters(list, count);

    for (int i = 0; i < count; i++) {
        rdma_adapter_info* a = &list[i];
        printf("  [%d] %s\n", i, a->ip_address);
        printf("      AdapterId:      0x%016llX\n", a->adapter_id);
        printf("      Vendor/Device:  0x%04X / 0x%04X\n", a->vendor_id, a->device_id);
        printf("      MaxTransfer:    %u MB\n", a->max_transfer_mb);
        printf("      MaxInline:      %u B\n", a->max_inline_data);
        printf("      MaxCQDepth:     %u\n", a->max_cq_depth);
        printf("      MaxInitQDepth:  %u\n", a->max_initiator_depth);
        printf("      Flags:          0x%08X\n", a->flags);
        printf("      InOrder DMA:    %s\n", a->has_in_order_dma ? "yes" : "no");
        printf("      Multi Engine:   %s\n", a->has_multi_engine ? "yes" : "no");
        printf("      Loopback:       %s\n\n", a->has_loopback ? "yes" : "no");
    }

    free(list);
    rdma_transfer_cleanup();
    return 0;
}
