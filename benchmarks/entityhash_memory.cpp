// Independent experiment: memory required by one shared EntityLayout word map.
#include <unordered_map>
#include <array>
#include <memory>
#include <vector>
#include <iostream>
#include <cstdint>
#ifdef __GLIBC__
#include <malloc.h>
#endif
struct Location {
    std::uint32_t reg{}, byte{};
    std::uint8_t rank{2};
    std::array<std::uint32_t, 8> coordinates{};
};
using Layout = std::unordered_map<std::uint64_t, Location>;
int main(int argc, char** argv) {
    const std::size_t elements = argc > 1 ? std::stoul(argv[1]) : 10000;
    const std::size_t bytes = argc > 2 ? std::stoul(argv[2]) : 4;
    const std::size_t aliases = argc > 3 ? std::stoul(argv[3]) : 8;
    if (!bytes || bytes % 4)
        return 2;
#ifdef __GLIBC__
    auto before = mallinfo2().uordblks;
#endif
    auto layout = std::make_shared<Layout>();
    layout->reserve(elements * (bytes / 4));
    for (std::uint32_t e = 0; e < elements; ++e)
        for (std::uint32_t byte = 0; byte < bytes; byte += 4) {
            Location loc{};
            loc.reg = e / 100;
            loc.byte = byte;
            loc.coordinates[0] = e / 1000;
            loc.coordinates[1] = e % 1000;
            layout->emplace(std::uint64_t(e) * 512 + byte, loc);
        }
    std::vector<std::shared_ptr<const Layout>> views(aliases, layout);
#ifdef __GLIBC__
    auto after = mallinfo2().uordblks;
#endif
    std::uint64_t checksum = 0;
    for (auto& v : views)
        for (std::size_t e = 0; e < elements; e += 17) {
            const auto it = v->find(std::uint64_t(e) * 512);
            if (it == v->end())
                return 3;
            checksum += it->second.coordinates[0] + it->second.coordinates[1];
        }
    std::cout << "elements=" << elements << " width=" << bytes << " aliases=" << aliases
              << " entries=" << layout->size() << " buckets=" << layout->bucket_count()
              << " checksum=" << checksum << "\n";
#ifdef __GLIBC__
    std::cout << "malloc_inuse_delta_bytes=" << (after - before) << "\n";
#endif
}
