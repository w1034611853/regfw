#include <regfw/regfw.hpp>
#include <time.h>
#include <thread>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#define NOINLINE __attribute__((noinline))
#ifndef REGFW_BENCH_STRIDE
#define REGFW_BENCH_STRIDE 4
#endif
#ifndef REGFW_BENCH_MIXED
#define REGFW_BENCH_MIXED 1
#endif
constexpr unsigned COUNT = 1000, DIRECT = 700, GROUPS = 30, INDICES = 10, VFS = 8;
constexpr std::uint64_t A = 0xff71f0000000ULL, B = 0xc100000000ULL;
constexpr std::uint64_t VF_BASE = 0x100000, VF_STRIDE = 0x10000, SPACE_SIZE = 0x180000;
unsigned build_direct = COUNT;
bool build_mixed = false;
constexpr std::array<unsigned, VFS> routes = {3, 7, 0, 6, 1, 5, 2, 4};
struct Tables {
    std::array<unsigned, VFS> vf = routes;
    std::array<std::array<unsigned, INDICES>, VFS> index{};
    Tables() {
        for (unsigned v = 0; v < VFS; ++v)
            for (unsigned i = 0; i < INDICES; ++i)
                index[v][i] = (i * 3 + v) % INDICES;
    }
};
struct Effects {
    std::uint64_t reads = 0, writes = 0, hash = 0;
};
NOINLINE std::uint32_t read_effect(Effects& e, std::uint32_t value) {
    ++e.reads;
    e.hash += value;
    return value ^ 0xa5a50000u;
}
NOINLINE void write_effect(Effects& e, std::uint32_t before, std::uint32_t after) {
    ++e.writes;
    e.hash += std::uint64_t(before) + after;
}
std::uint32_t initial(unsigned v, unsigned slot) {
    return 0x12345678u + v * 0x10001u + slot * 37u;
}

REG_ACCESS_FIELD(VfRoute, std::uint32_t);
REG_ACCESS_FIELD(LogicalIndex, std::uint32_t);
REG_ACCESS_FIELD(PhysicalIndex, std::uint32_t);
struct Cell : reg::RegEntityBundle {
    REG_BUNDLE(Cell);
    REG(VALUE, std::uint32_t, reg::at(0));
};
template <unsigned Stride>
struct Mapped : reg::RegEntityBundle {
    REG_BUNDLE(Mapped);
    REG(TABLE,
        std::uint32_t,
        reg::at(0).array(reg::dims(GROUPS, INDICES), reg::strides(INDICES* Stride, Stride)));
#ifdef REGFW_STATIC_BENCH
    REG_STATIC_INDEX(INDEX_MAP, TABLE, LogicalIndex, PhysicalIndex, 1);
#else
    REG_ACCESS_GROUP(
        INDEX_MAP,
        .members = {TABLE},
        .coordinates = {{LogicalIndex, 1}},
        .derive = {reg::map_index(LogicalIndex, PhysicalIndex).using_resolver(PhysicalIndex)});
#endif
};
template <unsigned Stride>
struct Layout : reg::Bundle {
    REG_BUNDLE(Layout);
    REG_REPEAT_ENTITY(direct, Cell, COUNT);
    REG_REPEAT_ENTITY(vf, Mapped<Stride>, VFS);
#ifdef REGFW_STATIC_BENCH
    REG_LAYOUT([](auto layout) {
        auto base = layout.view("REGS", SPACE_SIZE)
                        .map_repeat("DIRECT",
                                    0,
                                    reg::repeat(REGFW_BENCH_MIXED ? DIRECT : COUNT).stride(Stride),
                                    [&](auto i) {
                                        return layout.view("CELL", Stride).map(layout.direct[i]);
                                    });
        if constexpr (REGFW_BENCH_MIXED) {
            auto root =
                base.map_repeat("VF", VF_BASE, reg::repeat(VFS).stride(VF_STRIDE), [&](auto i) {
                    return layout.view("VF_REGS", VF_STRIDE)
                        .identity(1)
                        .using_resolver(VfRoute)
                        .map(layout.vf[i]);
                });
            return layout.address_space("DEVICE").map(root, A).map(root, B);
        } else
            return layout.address_space("DEVICE").map(base, A).map(base, B);
    });
#else
    REG_LAYOUT([&] {
        auto root =
            reg::view("REGS", SPACE_SIZE)
                .map_repeat("DIRECT", 0, reg::repeat(build_direct).stride(Stride), [&](auto i) {
                    return reg::view("CELL", Stride).map(direct[i]);
                });
        if (build_mixed)
            root = root.map_repeat("VF", VF_BASE, reg::repeat(VFS).stride(VF_STRIDE), [&](auto i) {
                return reg::view("VF_REGS", VF_STRIDE)
                    .identity(1)
                    .using_resolver(VfRoute)
                    .map(vf[i]);
            });
        return reg::address_space("DEVICE").map(root, A).map(root, B);
    });
#endif
};
std::chrono::nanoseconds thread_now();
template <unsigned Stride>
struct Framework {
    double init_ns{}, bind_ns{}, finish_ns{};
    reg::RegisterBank<Layout<Stride>> bank;
    Tables tables;
    Effects effects;
    bool callbacks;
    static void require_stride(unsigned stride) {
        if (stride != Stride)
            throw std::runtime_error("stride mismatch");
    }
    Framework(unsigned stride, bool mixed, bool cb) : callbacks(cb) {
        const auto t0 = thread_now();
        require_stride(stride);
        build_mixed = mixed;
        build_direct = mixed ? DIRECT : COUNT;
        bank.init(reg::space().name("BENCH"));
        if (std::getenv("REGFW_BENCH_LOGGER")) {
            static auto* logger =
                hlog_register_logger("bench032", "regfw-bench032", HLOG_LEVEL_INFO);
            bank.map().set_logger(logger);
        }
        const auto t1 = thread_now();
        init_ns = (t1 - t0).count();
        bank.map().set_identity_resolver(VfRoute, [this](const auto& r) {
            if (r.request_id >= VFS)
                throw std::runtime_error("VF range");
            auto p = tables.vf[r.request_id];
            if (p >= VFS)
                throw std::runtime_error("physical VF range");
            return p;
        });
        bank.map().set_access_resolver(PhysicalIndex, [this](const reg::RegAccessInfo& i) {
            auto v = i.resolved_id(), q = i.index_as(LogicalIndex);
            if (v >= VFS || q >= INDICES)
                throw std::runtime_error("index range");
            auto p = tables.index[v][q];
            if (p >= INDICES)
                throw std::runtime_error("physical index range");
            return p;
        });
        if (cb) {
            for (unsigned i = 0; i < build_direct; ++i) {
                auto& r = bank->direct[i].VALUE;
                r.on_read([this](std::uint32_t value, const auto&) {
                    return read_effect(effects, value);
                });
                r.on_write([this](const reg::RegAccessInfo& info) {
                    write_effect(effects,
                                 std::uint32_t(info.stored_before),
                                 std::uint32_t(info.stored_after));
                });
            }
            if (mixed)
                for (unsigned v = 0; v < VFS; ++v) {
                    auto& r = bank->vf[v].TABLE;
                    r.on_read([this](std::uint32_t value, const auto&) {
                        return read_effect(effects, value);
                    });
                    r.on_write([this](const reg::RegAccessInfo& info) {
                        write_effect(effects,
                                     std::uint32_t(info.stored_before),
                                     std::uint32_t(info.stored_after));
                    });
                }
        }
        const auto t2 = thread_now();
        bind_ns = (t2 - t1).count();
        bank.finish();
        finish_ns = (thread_now() - t2).count();
        reset();
    }
    void reset() {
        for (unsigned i = 0; i < COUNT; ++i)
            bank->direct[i].VALUE.ref() = initial(0, i);
        for (unsigned v = 0; v < VFS; ++v)
            for (unsigned g = 0; g < GROUPS; ++g)
                for (unsigned q = 0; q < INDICES; ++q)
                    bank->vf[v].TABLE.at(g, q).ref() = initial(v, DIRECT + g * INDICES + q);
        effects = {};
    }
};
template <unsigned Stride>
NOINLINE std::uint32_t fw_read(void* p, std::uint64_t a, unsigned size) {
    return static_cast<Framework<Stride>*>(p)->bank.map().mmio_read_sized(a, size);
}
template <unsigned Stride>
NOINLINE unsigned fw_write(void* p, std::uint64_t a, std::uint32_t x, unsigned size) {
    return !static_cast<Framework<Stride>*>(p)->bank.map().mmio_write_sized(a, x, size).success();
}

struct Switch {
    std::array<std::uint32_t, COUNT> direct{};
    std::array<std::array<std::uint32_t, GROUPS * INDICES>, VFS> mapped{};
    Tables tables;
    Effects effects;
    bool callbacks;
    explicit Switch(bool cb) : callbacks(cb) {
        reset();
    }
    void reset() {
        for (unsigned i = 0; i < COUNT; ++i)
            direct[i] = initial(0, i);
        for (unsigned v = 0; v < VFS; ++v)
            for (unsigned i = 0; i < GROUPS * INDICES; ++i)
                mapped[v][i] = initial(v, DIRECT + i);
        effects = {};
    }
};
bool valid_size(std::uint64_t a, unsigned size) {
    return (size == 1 || size == 2 || size == 4) && a % size == 0 && (a % 4) + size <= 4;
}
bool offset_of(std::uint64_t a, std::uint64_t& off) {
    if (a >= A && a - A < SPACE_SIZE) {
        off = a - A;
        return true;
    }
    if (a >= B && a - B < SPACE_SIZE) {
        off = a - B;
        return true;
    }
    return false;
}
std::uint32_t mask_of(unsigned size) {
    return size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1;
}
inline std::uint32_t load_direct(Switch& s, unsigned n, unsigned lane, unsigned size) {
    auto x = s.direct[n] & (mask_of(size) << (lane * 8));
    if (s.callbacks)
        x = read_effect(s.effects, x);
    return (x >> (lane * 8)) & mask_of(size);
}
inline unsigned
store_direct(Switch& s, unsigned n, unsigned lane, unsigned size, std::uint32_t value) {
    auto& x = s.direct[n];
    auto before = x;
    auto mask = mask_of(size) << (lane * 8);
    x = (x & ~mask) | ((value << (lane * 8)) & mask);
    if (s.callbacks)
        write_effect(s.effects, before, x);
    return 0;
}
unsigned physical_slot(Switch& s, unsigned request, unsigned slot, unsigned& vf) {
    if (request >= VFS)
        throw std::runtime_error("VF range");
    vf = s.tables.vf[request];
    if (vf >= VFS)
        throw std::runtime_error("physical VF range");
    auto q = slot % INDICES;
    auto p = s.tables.index[vf][q];
    if (p >= INDICES)
        throw std::runtime_error("physical index range");
    return (slot / INDICES) * INDICES + p;
}
inline std::uint32_t
load_mapped(Switch& s, unsigned request, unsigned slot, unsigned lane, unsigned size) {
    unsigned vf;
    auto n = physical_slot(s, request, slot, vf);
    auto x = s.mapped[vf][n] & (mask_of(size) << (lane * 8));
    if (s.callbacks)
        x = read_effect(s.effects, x);
    return (x >> (lane * 8)) & mask_of(size);
}
inline unsigned store_mapped(
    Switch& s, unsigned request, unsigned slot, unsigned lane, unsigned size, std::uint32_t value) {
    unsigned vf;
    auto n = physical_slot(s, request, slot, vf);
    auto& x = s.mapped[vf][n];
    auto before = x;
    auto mask = mask_of(size) << (lane * 8);
    x = (x & ~mask) | ((value << (lane * 8)) & mask);
    if (s.callbacks)
        write_effect(s.effects, before, x);
    return 0;
}

template <unsigned Stride, bool Mixed, bool Normalize>
bool decode(std::uint64_t a, unsigned size, std::uint64_t& key, unsigned& request, unsigned& lane) {
    std::uint64_t off;
    if (!valid_size(a, size) || !offset_of(a, off))
        return false;
    lane = off % 4;
    off -= lane;
    request = 0;
    if constexpr (Mixed) {
        if (off >= VF_BASE) {
            auto relative = off - VF_BASE;
            request = relative / VF_STRIDE;
            if (request >= VFS)
                return false;
            auto local = relative % VF_STRIDE;
            if constexpr (Normalize) {
                if (local % Stride)
                    return false;
                key = DIRECT + local / Stride;
                if (key >= COUNT)
                    return false;
            } else
                key = VF_BASE + local;
            return true;
        }
    }
    if constexpr (Normalize) {
        if (off % Stride)
            return false;
        key = off / Stride;
        if (key >= (Mixed ? DIRECT : COUNT))
            return false;
    } else
        key = off;
    return true;
}
template <unsigned Stride, bool Mixed, bool Normalize>
NOINLINE std::uint32_t sw_read(void* p, std::uint64_t a, unsigned size) {
    auto& s = *static_cast<Switch*>(p);
    std::uint64_t key;
    unsigned request, lane;
    if (!decode<Stride, Mixed, Normalize>(a, size, key, request, lane))
        return 0;
    switch (key) {
#define CASE(N) case (Normalize ? std::uint64_t(N) : ((Mixed && N>=DIRECT)?VF_BASE+std::uint64_t(N-DIRECT)*Stride:std::uint64_t(N)*Stride)): if constexpr(Mixed && N>=DIRECT)return load_mapped(s,request,N-DIRECT,lane,size); \
        else return load_direct(s, N, lane, size);
#include "cases.inc"
#undef CASE
        default:
            return 0;
    }
}
template <unsigned Stride, bool Mixed, bool Normalize>
NOINLINE unsigned sw_write(void* p, std::uint64_t a, std::uint32_t value, unsigned size) {
    auto& s = *static_cast<Switch*>(p);
    std::uint64_t key;
    unsigned request, lane;
    if (!decode<Stride, Mixed, Normalize>(a, size, key, request, lane))
        return 1;
    switch (key) {
#define CASE(N) case (Normalize ? std::uint64_t(N) : ((Mixed && N>=DIRECT)?VF_BASE+std::uint64_t(N-DIRECT)*Stride:std::uint64_t(N)*Stride)): if constexpr(Mixed && N>=DIRECT)return store_mapped(s,request,N-DIRECT,lane,size,value); \
        else return store_direct(s, N, lane, size, value);
#include "cases.inc"
#undef CASE
        default:
            return 1;
    }
}
struct Access {
    std::uint64_t address;
    std::uint32_t value;
};
std::uint64_t next_random(std::uint64_t& x) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}
std::vector<Access>
trace(unsigned stride, bool mixed, const std::string& pattern, unsigned n = 100000) {
    std::vector<Access> out;
    out.reserve(n);
    std::uint64_t seed = 0x123456789abcdefULL;
    unsigned direct = 0, mapped = 0;
    for (unsigned i = 0; i < n; ++i) {
        auto random = next_random(seed);
        bool remap = mixed && (pattern == "mapped" || i % 10 >= 7);
        unsigned slot, vf = 0;
        if (remap) {
            slot = pattern == "hot" ? 0 : pattern == "sequential" ? mapped++ % 300 : random % 300;
            vf = pattern == "hot" ? 0 : (random >> 32) % VFS;
        } else
            slot = pattern == "hot"          ? 0
                   : pattern == "sequential" ? direct++ % (mixed ? DIRECT : COUNT)
                                             : random % (mixed ? DIRECT : COUNT);
        auto off = remap ? VF_BASE + vf * VF_STRIDE + slot * stride : slot * stride;
        auto base = pattern == "hot" ? A : ((random >> 40) & 1) ? A : B;
        out.push_back({base + off, std::uint32_t(random)});
    }
    return out;
}
using Read = std::uint32_t (*)(void*, std::uint64_t, unsigned);
using Write = unsigned (*)(void*, std::uint64_t, std::uint32_t, unsigned);
struct Engine {
    const char* name;
    void* object;
    Read read;
    Write write;
};
std::uint64_t observed = 0;
std::chrono::nanoseconds thread_now() {
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t))
        throw std::runtime_error("clock_gettime");
    return std::chrono::seconds(t.tv_sec) + std::chrono::nanoseconds(t.tv_nsec);
}
NOINLINE double
timed(const Engine& e, const std::vector<Access>& t, bool writing, unsigned repeats) {
    std::uint64_t sum = 0;
    auto start = thread_now();
    if (writing) {
        for (unsigned r = 0; r < repeats; ++r)
            for (const auto& a : t)
                sum += e.write(e.object, a.address, a.value, 4);
    } else {
        for (unsigned r = 0; r < repeats; ++r)
            for (const auto& a : t)
                sum += e.read(e.object, a.address, 4);
    }
    auto end = thread_now();
    observed += sum;
    return std::chrono::duration<double, std::nano>(end - start).count() / (t.size() * repeats);
}
void require(bool ok, const char* text) {
    if (!ok)
        throw std::runtime_error(text);
}
template <unsigned Stride>
void same_storage(Framework<Stride>& f, const Switch& s, bool mixed) {
    for (unsigned i = 0; i < (mixed ? DIRECT : COUNT); ++i)
        require(f.bank->direct[i].VALUE.ref() == s.direct[i], "direct storage differs");
    if (mixed)
        for (unsigned v = 0; v < VFS; ++v)
            for (unsigned g = 0; g < GROUPS; ++g)
                for (unsigned q = 0; q < INDICES; ++q)
                    require(f.bank->vf[v].TABLE.at(g, q).ref() == s.mapped[v][g * INDICES + q],
                            "mapped storage differs");
    if (f.effects.reads != s.effects.reads || f.effects.writes != s.effects.writes ||
        f.effects.hash != s.effects.hash) {
        std::cerr << "effects FW " << f.effects.reads << ' ' << f.effects.writes << ' '
                  << f.effects.hash << " SW " << s.effects.reads << ' ' << s.effects.writes << ' '
                  << s.effects.hash << '\n';
        throw std::runtime_error("callback effects differ");
    }
}
unsigned selected_stride = 4;
bool selected_mixed = false, selected_callbacks = false, selected_writing = false;
std::string selected_pattern = "random";
template <unsigned Stride, bool Mixed>
void run(bool callbacks, unsigned repeats) {
    if (Stride != selected_stride || Mixed != selected_mixed || callbacks != selected_callbacks)
        return;
    std::cerr << "CONFIG " << Stride << ' ' << Mixed << ' ' << callbacks << '\n';
    if (repeats == 999) {
        for (unsigned k = 0; k < 11; ++k) {
            auto start = thread_now();
            Framework<Stride> f(Stride, Mixed, callbacks);
            auto all = (thread_now() - start).count();
            std::cout << "INIT " << Stride << ' ' << Mixed << ' ' << callbacks << ' ' << f.init_ns
                      << ' ' << f.bind_ns << ' ' << f.finish_ns << ' ' << all << '\n';
        }
        return;
    }
    Framework<Stride> f(Stride, Mixed, callbacks);
    Switch raw(callbacks), norm(callbacks);
    std::array<Engine, 3> engines = {
        Engine{"regfw", &f, fw_read<Stride>, fw_write<Stride>},
        Engine{"switch_raw", &raw, sw_read<Stride, Mixed, false>, sw_write<Stride, Mixed, false>},
        Engine{"switch_normalized",
               &norm,
               sw_read<Stride, Mixed, true>,
               sw_write<Stride, Mixed, true>}};
    // Untimed correctness comparison includes lane merging and callback outputs.
    auto correctness = trace(Stride, Mixed, "random", 10000);
    for (unsigned i = 0; i < correctness.size(); ++i) {
        auto a = correctness[i];
        unsigned size = 1u << (i % 3);
        a.address += (size == 1 ? i % 4 : size == 2 ? (i % 2) * 2 : 0);
        for (auto& e : engines)
            require(e.write(e.object, a.address, a.value, size) == 0, "valid write failed");
        auto expected = engines[0].read(engines[0].object, a.address, size);
        for (unsigned j = 1; j < 3; ++j)
            require(engines[j].read(engines[j].object, a.address, size) == expected,
                    "read result differs");
    }
    same_storage(f, raw, Mixed);
    same_storage(f, norm, Mixed);
    f.reset();
    raw.reset();
    norm.reset();
    if (!repeats) {
        std::cout << "VALIDATED " << Stride << ' ' << Mixed << ' ' << callbacks << '\n';
        return;
    }
    for (const auto* pattern : {"hot", "sequential", "random", "mapped"}) {
        if (selected_pattern != pattern)
            continue;
        auto t = trace(Stride, Mixed, pattern);
        for (bool writing : {false, true}) {
            if (writing != selected_writing)
                continue;
            std::array<std::vector<double>, 3> samples;
            for (unsigned round = 0; round < 8; ++round)
                for (unsigned step = 0; step < 3; ++step) {
                    unsigned j = (round + step) % 3;
                    auto x = timed(engines[j], t, writing, repeats);
                    if (round)
                        samples[j].push_back(x);
                }
            for (unsigned j = 0; j < 3; ++j) {
                auto x = samples[j];
                std::sort(x.begin(), x.end());
                std::cout << "RESULT " << Stride << ' ' << (Mixed ? "mixed30" : "plain") << ' '
                          << (callbacks ? "callbacks" : "rw") << ' ' << pattern << ' '
                          << (writing ? "write" : "read") << ' ' << engines[j].name << ' '
                          << std::fixed << std::setprecision(4) << x[3];
                for (auto v : samples[j])
                    std::cout << ' ' << v;
                std::cout << '\n' << std::flush;
            }
        }
    }
    same_storage(f, raw, Mixed);
    same_storage(f, norm, Mixed);
    observed += f.effects.hash + raw.effects.hash + norm.effects.hash;
}
int main(int argc, char** argv) {
    if (std::getenv("REGFW_BENCH_THREADS"))
        std::thread([] {}).join();
#if defined(__GLIBC__)
    std::cout << "LIBC_SINGLE_THREADED " << int(__libc_single_threaded) << "\n";
#else
    std::cout << "LIBC_SINGLE_THREADED n/a\n";
#endif
    unsigned repeats = argc > 1 ? std::stoul(argv[1]) : 5;
    if (argc != 7)
        throw std::runtime_error("usage: bench PASSES STRIDE MIXED CALLBACKS PATTERN WRITE");
    selected_stride = std::stoul(argv[2]);
    selected_mixed = std::stoul(argv[3]);
    selected_callbacks = std::stoul(argv[4]);
    selected_pattern = argv[5];
    selected_writing = std::stoul(argv[6]);
#if defined(REGFW_STATIC_BENCH)
    for (bool callbacks : {false, true})
        run<REGFW_BENCH_STRIDE, REGFW_BENCH_MIXED>(callbacks, repeats);
#else
    for (bool callbacks : {false, true}) {
        run<4, false>(callbacks, repeats);
        run<4, true>(callbacks, repeats);
        run<64, false>(callbacks, repeats);
        run<64, true>(callbacks, repeats);
    }
#endif
    std::cout << "CHECKSUM " << observed << '\n';
}
