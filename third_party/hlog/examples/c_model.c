#include "hlog.h"

#include <stdio.h>
void example_c_model(void)
{
    HLogLogger *logger = hlog_create("legacy_c", HLOG_LEVEL_INFO);
    if (logger == NULL) {
        fprintf(stderr, "logger registration failed: %s\n", hlog_last_error());
        return;
    }

    unsigned opcode = 0x17;
    uint8_t packet[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    HLOG_INFO(logger, "legacy C model initialized, opcode=%#x", opcode);
    HLOG_HEXDUMP(logger, HLOG_LEVEL_INFO, packet, sizeof(packet));
}
