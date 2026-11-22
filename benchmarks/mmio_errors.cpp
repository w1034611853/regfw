// Compare the result entry points on aligned holes and invalid requests.
#include <regfw/regfw.hpp>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <time.h>

struct SparseWords : reg::RegEntityBundle {
    REG_BUNDLE(SparseWords);
    REG(WORDS, std::uint32_t, reg::at(0).array(reg::dims(256), reg::strides(16)));
};
struct ErrorDevice : reg::Bundle {
    REG_BUNDLE(ErrorDevice);
    REG_ENTITY(regs, SparseWords);
    REG_LAYOUT([&] {
        return reg::address_space("ERRORS").map(reg::view("REGS", 0x1000).map(regs), 0);
    });
};
static std::uint64_t time_ns() {
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t))
        std::abort();
    return std::uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}
int main(int argc, char** argv) {
    if (argc != 4)
        return 2;
    const std::string_view probe = argv[1];
    const bool writing = std::atoi(argv[2]);
    const auto iterations = std::strtoull(argv[3], nullptr, 10);
    if (!iterations ||
        (probe != "hole" && probe != "outside" && probe != "unaligned" && probe != "width"))
        return 2;
    const auto offset = probe == "hole"        ? 4u
                        : probe == "outside"   ? 0x2000u
                        : probe == "unaligned" ? 1u
                                               : 0u;
    const auto size = probe == "width" ? 3u : 4u;
    const auto expected = probe == "width" || probe == "unaligned"
                              ? reg::RegMemTxResult::Unsupported
                              : reg::RegMemTxResult::DecodeError;
    const auto invalid = size == 4 ? reg::INVALID_READ_VALUE & 0xffffffffu : 0;
    reg::RegisterBank<ErrorDevice> bank(reg::space(0x2000).finalize());
    auto& map = bank.map();
    if (!map.mmio_write(16, 1).success() || map.mmio_read(16) != 1)
        std::abort();
    std::uint64_t sum = 0;
    const auto start = time_ns();
    for (std::uint64_t i = 0; i < iterations; ++i) {
        const auto address = offset + ((i * 13) & 255) * 16;
        if (writing) {
            const auto result = map.try_mmio_write_sized(address, i, size);
            if (result.result != expected)
                std::abort();
            sum += static_cast<unsigned>(result.result);
        } else {
            const auto result = map.try_mmio_read_sized(address, size);
            if (result.result != expected || result.value != invalid)
                std::abort();
            sum += result.value + static_cast<unsigned>(result.result);
        }
    }
    const auto elapsed = time_ns() - start;
    if (map.mmio_read(16) != 1)
        std::abort();
    std::cout << "ERROR " << double(elapsed) / iterations << ' ' << sum << '\n';
}
