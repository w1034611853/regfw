#include "hlog.hpp"

class Bmu {
public:
    Bmu() : logger_("bmu", HLOG_LEVEL_INFO)
    {
    }

    void send_cmd(unsigned opcode)
    {
        HLOG_DEBUG(logger_, "send command opcode={:#x}", opcode);
        HLOG_INFO(logger_, "command submitted");

        const unsigned char packet[] = {0x01, 0x23, 0x45, 0x67};
        HLOG_HEXDUMP(logger_, HLOG_LEVEL_DEBUG, packet, sizeof(packet));

        if (opcode == 0xffff) {
            HLOG_ERROR(logger_, "invalid opcode={:#x}", opcode);
        }
    }

private:
    hlog::Logger logger_;
};
