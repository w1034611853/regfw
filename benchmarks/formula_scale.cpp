// Scale benchmark (task §8): 8 identity-routed VF windows, each mapping a
// transposed 512-bit array of dims(8,256) at stride 0x100 -- 262,144 mapped
// words plus scalar/shared business registers. The formula plan must stay
// compact and its install time must not scale with the word count.
#include <regfw/regfw.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <time.h>
#include <sys/resource.h>

REG_ACCESS_FIELD(VfRoute, std::uint32_t);
constexpr unsigned VF = 8;
constexpr std::uint64_t VF_BASE = 0x2000000, VF_STRIDE = 0x200000, ARRAY_OFF = 0x10000;
constexpr std::uint64_t BUS_BASE = 0;

struct BigArray : reg::RegEntityBundle {
    REG_BUNDLE(BigArray);
    REG(SLOT, reg::u512, reg::at(0).array(reg::dims(8, 256), reg::strides(0x40000, 0x100)));
};
struct Scalars : reg::RegEntityBundle {
    REG_BUNDLE(Scalars);
    REG(VALUE, std::uint32_t, reg::at(0).array(reg::dims(64), reg::strides(4)));
};
struct Shared : reg::RegEntityBundle {
    REG_BUNDLE(Shared);
    REG(CTRL, std::uint32_t, reg::at(0));
};
struct ScaleLayout : reg::Bundle {
    REG_BUNDLE(ScaleLayout);
    REG_ENTITY(array, BigArray);
    REG_ENTITY(scalars, Scalars);
    REG_ENTITY(shared, Shared);
    REG_LAYOUT([](auto layout) {
        namespace reg = ::reg::compiled;
        return reg::address_space("SCALE")
            .map(reg::view("PF", VF_STRIDE)
                     .map(reg::view("PF_ARRAY", 0x1D0000).map(layout.array), ARRAY_OFF)
                     .map(reg::view("PF_SCALARS", 0x100).map(layout.scalars), 0x80000),
                 0)
            .map_repeat("VF", VF_BASE, reg::repeat(VF).stride(VF_STRIDE), [&](auto) {
                return reg::view("VF_WIN", VF_STRIDE)
                    .identity(1)
                    .using_resolver(VfRoute)
                    .vf_to_pf()
                    .map(reg::view("VF_ARRAY", 0x1D0000).map(layout.array), ARRAY_OFF)
                    .map(reg::view("VF_SCALARS", 0x100).map(layout.scalars), 0x80000)
                    .map(reg::view("VF_SHARED", 0x40).map(layout.shared), 0x1000);
            });
    });
};
static std::int64_t scale_cpu_ns() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return std::int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}
static double secs(std::int64_t ns) {
    return double(ns) / 1e9;
}

int main() {
    const auto w0 = scale_cpu_ns();
    reg::RegisterBank<ScaleLayout> bank(reg::space().name("SCALE"));
    const auto w1 = scale_cpu_ns();
    bank.map().set_identity_resolver(VfRoute, [](const auto& r) {
        return r.request_id;
    });
    bank.finish();
    const auto w2 = scale_cpu_ns();
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    auto& m = bank.map();
    // Scalar reachability: PF business, identity-routed VF slot, shared alias.
    if (!m.mmio_write_sized(0x80000, 7, 4).success() || m.mmio_read_sized(0x80000, 4) != 7)
        return 2;
    if (!m.mmio_write_sized(VF_BASE + VF_STRIDE + 0x80000, 9, 4).success() ||
        m.mmio_read_sized(VF_BASE + VF_STRIDE + 0x80000, 4) != 9)
        return 3;
    if (!m.mmio_write_sized(VF_BASE + 2 * VF_STRIDE + 0x1000, 42, 4).success() ||
        bank->shared.CTRL.ref() != 42)
        return 4;
    // Scalar32 TPS over PF scalars.
    constexpr unsigned N = 1u << 20;
    const auto r0 = scale_cpu_ns();
    for (unsigned i = 0; i < N; ++i)
        m.mmio_write_sized(0x80000 + (i % 64) * 4, i, 4);
    const auto r1 = scale_cpu_ns();
    std::uint64_t sink = 0;
    for (unsigned i = 0; i < N; ++i)
        sink += m.mmio_read_sized(0x80000 + (i % 64) * 4, 4);
    const auto r2 = scale_cpu_ns();
    // Identity-routed VF scalar TPS.
    const auto i0 = scale_cpu_ns();
    for (unsigned k = 0; k < N / 4; ++k)
        m.mmio_write_sized(VF_BASE + (k % VF) * VF_STRIDE + 0x80000 + (k % 64) * 4, k, 4);
    const auto i1 = scale_cpu_ns();
    std::printf("RESULT construct=%.6f finish=%.6f maxrss_kb=%ld scalar32_w_tps=%.0f "
                "scalar32_r_tps=%.0f identity_w_tps=%.0f sink=%llu\n",
                secs(w1 - w0),
                secs(w2 - w1),
                usage.ru_maxrss,
                N / secs(r1 - r0),
                N / secs(r2 - r1),
                (N / 4) / secs(i1 - i0),
                (unsigned long long)sink);
    return 0;
}
