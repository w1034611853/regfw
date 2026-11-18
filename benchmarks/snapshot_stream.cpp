// Compile twice with matching SDK headers/library. Uses the real bank archive path.
#include <regfw/vmstate.hpp>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>

struct File {
    std::vector<std::uint8_t> bytes;
    std::size_t position{}, writes{}, reads{};
};
extern "C" void module_var_save(void* opaque, const std::uint8_t* data, std::uint32_t size) {
    auto& file = *static_cast<File*>(opaque);
    ++file.writes;
    file.bytes.insert(file.bytes.end(), data, data + size);
}
extern "C" void module_var_load(void* opaque, std::uint8_t* data, std::uint32_t size) {
    auto& file = *static_cast<File*>(opaque);
    ++file.reads;
    if (size > file.bytes.size() - file.position)
        throw std::runtime_error("truncated stream");
    std::memcpy(data, file.bytes.data() + file.position, size);
    file.position += size;
}
struct Empty : reg::Bundle {
    REG_BUNDLE(Empty);
};
using Clock = std::chrono::steady_clock;
int main(int argc, char** argv) try {
    const bool arrays = argc > 1 && std::string_view(argv[1]) == "array";
    const unsigned count = arrays ? 65536 : 2048;
    reg::RegisterBank<Empty> bank(reg::space(9ull * count * 4).name("SNAPSHOT"));
    auto& map = bank.map();
    for (unsigned vf = 0; vf < 9; ++vf) {
        const auto prefix = "POE.PF_VF[" + std::to_string(vf) + "].REGISTER_BANK.";
        if (arrays) {
            auto values =
                map.create_reg<std::uint32_t>(std::uint64_t(vf) * count * 4,
                                              prefix + "TABLE",
                                              reg::at(0).array(reg::dims(count), reg::strides(4)));
            for (unsigned i = 0; i < count; ++i)
                values.at(i).ref() = (vf + 1) * 65537 + i;
        } else {
            for (unsigned i = 0; i < count; ++i) {
                auto value = map.create_reg<std::uint32_t>(
                    (std::uint64_t(vf) * count + i) * 4,
                    prefix + "SCHEDULER.ORDER_ENGINE.STATE_" + std::to_string(i));
                value.ref() = (vf + 1) * 65537 + i;
            }
        }
    }
    bank.finish();
    const auto before = map.dump_state_versioned().data;
    File file;
    const auto begin = Clock::now();
    bank.save(static_cast<void*>(&file));
    const auto saved = Clock::now();
    bank.reset();
    const auto load_begin = Clock::now();
    bank.load(static_cast<void*>(&file));
    const auto loaded = Clock::now();
    if (file.position != file.bytes.size() || before != map.dump_state_versioned().data)
        throw std::runtime_error("restore mismatch");
    std::cout << "mode,value_bytes,stream_bytes,write_calls,read_calls,save_ms,load_ms\n"
              << (arrays ? "array" : "scalar") << ',' << 9ull * count * 4 << ','
              << file.bytes.size() << ',' << file.writes << ',' << file.reads << ','
              << std::chrono::duration<double, std::milli>(saved - begin).count() << ','
              << std::chrono::duration<double, std::milli>(loaded - load_begin).count() << '\n';
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
