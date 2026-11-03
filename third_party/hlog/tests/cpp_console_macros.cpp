// Compile/link against real fmt and hlog after installation.
#include "hlog.hpp"

int main()
{
    hlog::Logger logger("cpp-console", HLOG_LEVEL_OFF, "0");
    if (!logger) { return 1; }
    HLOG_CONSOLE_TRACE(logger, "trace");
    HLOG_CONSOLE_DEBUG(logger, "vf={}", 3);
    HLOG_CONSOLE_INFO(logger, "{:<8} {:>8}", "L_POOL", "VL_POOL");
    HLOG_CONSOLE_WARN(logger, "address=0x{:x}", 0x1234);
    HLOG_CONSOLE_ERROR(logger, "error");
    HLOG_CONSOLE_CRITICAL(logger, "critical");
    const unsigned char packet[] = {0x12, 0x34, 0x00, 0xff};
    HLOG_CONSOLE_HEXDUMP(logger, HLOG_LEVEL_DEBUG, packet, sizeof(packet));
    return 0;
}
