#include "hlog.hpp"
#include <array>
#include <vector>
#if __cplusplus >= 202002L
#include <span>
#endif

int main()
{
    hlog::Logger logger("buffer", HLOG_LEVEL_DEBUG, "demo");
    if (!logger) { return 1; }
    (void)hlog_set_default_level(HLOG_LEVEL_DEBUG);
    (void)hlog_set_console_enabled(true);

    std::array<uint8_t, 35> packet{};
    for (size_t i = 0; i < packet.size(); ++i) { packet[i] = static_cast<uint8_t>(i); }
    HLOG_HEXDUMP_DEBUG(logger, hlog::u8, packet);
    HLOG_HEXDUMP_DEBUG(logger, hlog::u16, packet);
    HLOG_HEXDUMP_DEBUG(logger, hlog::u32, packet);
    HLOG_HEXDUMP_DEBUG(logger, hlog::u64, packet);

    const uint32_t words[] = {0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00};
    HLOG_HEXDUMP_DEBUG(logger, hlog::u32, words); // Four elements, 16 bytes.
    HLOG_HEXDUMP_DEBUG(hlog::u32, words);        // Process-default logger.
    const std::vector<uint64_t> values{0x1122334455667788ULL, 0xaabbccddeeff0011ULL};
    HLOG_HEXDUMP_DEBUG(logger, hlog::u64, values);

#if __cplusplus >= 202002L
    HLOG_HEXDUMP_DEBUG(logger, hlog::u32, std::span(packet).subspan(1, 19));
#endif
    HLOG_HEXDUMP_DEBUG(logger, hlog::u16, packet.data() + 1, 19); // Pointer + byte count.
    HLOG_CONSOLE_HEXDUMP_INFO(logger, hlog::u32, words);         // Console only.
    logger.flush();
}
