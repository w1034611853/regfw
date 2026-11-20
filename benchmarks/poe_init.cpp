#define POE_INCLUDE_ONLY 1
#include "poe_layout.cpp"
#include <time.h>
#include <sys/resource.h>
#include <malloc.h>
#include <iostream>
#include <stdexcept>
std::uint64_t now_ns() {
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t))
        std::abort();
    return std::uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}
std::size_t heap_bytes() {
    auto m = mallinfo2();
    return m.uordblks + m.hblkhd;
}
int main(int argc, char** argv) try {
    const unsigned repeats = argc > 1 ? std::stoul(argv[1]) : 3;
    for (unsigned k = 0; k < repeats; ++k) {
        const auto h0 = heap_bytes(), t0 = now_ns();
        reg::RegisterBank<PoeLayout> bank(reg::space().name("POE_BENCH"));
        const auto t1 = now_ns();
        bank.map().set_identity_resolver(VfRoute, [](const auto& r) {
            return r.request_id;
        });
        bank.map().set_pre_access(CheckGetGroupInit, [](const auto&) {});
        bank.map().set_pre_access(CheckAccInit, [](const auto&) {});
        const auto t2 = now_ns();
        bank.finish();
        const auto t3 = now_ns(), h1 = heap_bytes();
        const auto s = bank.map().describe_layout(reg::LayoutDetail::Summary).size(), t4 = now_ns();
        const auto f = bank.map().describe_layout(reg::LayoutDetail::Full).size(), t5 = now_ns();
        if (!bank.map().mmio_write(POE_CPU_REG_BASE + 8, 0x12345678).success() ||
            bank.map().mmio_read(POE_CPU_REG_BASE + 8) != (0x12345678u & 0x00ff00ffu))
            throw std::runtime_error("POE scratch mismatch");
        rusage u{};
        getrusage(RUSAGE_SELF, &u);
        std::cout << "POE " << k << ' ' << t1 - t0 << ' ' << t3 - t2 << ' ' << t4 - t3 << ' '
                  << t5 - t4 << ' ' << h1 - h0 << ' ' << u.ru_maxrss << ' ' << s << ' ' << f << ' '
                  << bank.map().entity_count() << ' ' << bank.map().total_node_data_count() << '\n';
    }
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
