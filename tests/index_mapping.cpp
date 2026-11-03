#include <regfw/regfw.hpp>
#include <iostream>
#include <stdexcept>

#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!(__VA_ARGS__))                                                                        \
            throw std::runtime_error(#__VA_ARGS__);                                                \
    } while (false)
namespace {
constexpr std::uint32_t VF_DOMAIN = 1;
REG_ACCESS_FIELD(VlPool, std::uint32_t);
REG_ACCESS_FIELD(LPool, std::uint32_t);
REG_ACCESS_FIELD(VfRoute, std::uint32_t);
union Word {
    std::uint32_t u32;
    struct {
        std::uint32_t value : 16, reserved : 16;
    } bits;
};

struct BusinessRegs : reg::RegEntityBundle {
    REG_BUNDLE(BusinessRegs);
    REG(QUEUE, Word, reg::at(0).array(reg::dims(2, 4), reg::strides(0x20, 4)));
    REG(POOL_CONFIG, Word, reg::at(0x80).array(reg::dims(2, 4), reg::strides(0x20, 4)));
    REG(UNAFFECTED, Word, reg::at(0x100).array(reg::dims(2, 4), reg::strides(0x20, 4)));
    REG(COUNTER, Word, reg::at(0x180).array(reg::dims(4), reg::strides(4)));

    REG_ACCESS_GROUP(VF_POOL_2D,
                     .members = {QUEUE, POOL_CONFIG},
                     .coordinates = {{VlPool, 1}},
                     .derive = {
                         reg::map_index(VlPool, LPool)
                             .when(reg::identity_domain(VF_DOMAIN))
                             .using_resolver(LPool)
                             .otherwise(reg::passthrough(VlPool)),
                     });
    REG_ACCESS_GROUP(VF_POOL_1D,
                     .members = {COUNTER},
                     .coordinates = {{VlPool, 0}},
                     .derive = {
                         reg::map_index(VlPool, LPool)
                             .when(reg::identity_domain(VF_DOMAIN))
                             .using_resolver(LPool)
                             .otherwise(reg::passthrough(VlPool)),
                     });
};
struct DeviceLayout : reg::Bundle {
    REG_BUNDLE(DeviceLayout);
    REG_ENTITY(pf, BusinessRegs);
    REG_REPEAT_ENTITY(vf, BusinessRegs, 8);
    REG_LAYOUT([&] {
        return reg::address_space("DEVICE")
            .map(reg::view("PF", 0x200).map(pf), 0)
            .map_repeat("VF", 0x1000, reg::repeat(vf.size()).stride(0x200), [&](auto i) {
                return reg::view("INSTANCE", 0x200)
                    .map(vf[i])
                    .identity(VF_DOMAIN)
                    .using_resolver(VfRoute);
            });
    });
};
struct Device {
    reg::RegisterBank<DeviceLayout> bank;
    unsigned calls{}, last_vf = 99;
    Device() {
        bank.init(reg::space(0x3000).name("INDEX_MAPPING"));
        bank.map().set_identity_resolver(VfRoute, [](const auto& r) {
            return r.request_id;
        });
        bank.map().set_access_resolver(LPool,
                                       [this](const reg::RegAccessInfo& info) -> std::uint32_t {
                                           ++calls;
                                           last_vf = info.resolved_id();
                                           return (info.index_as(VlPool) + 1) % 4;
                                       });
        bank.finish();
    }
};
} // namespace
int main() {
    Device device;
    auto& bank = device.bank;
    auto& map = bank.map();
    for (unsigned vf = 0; vf < 8; ++vf) {
        const auto base = 0x1000 + vf * 0x200;
        CHECK(map.mmio_write(base + 0x28, 10 + vf).success()); // QUEUE[1,2] -> [1,3]
        CHECK(bank->vf[vf].QUEUE.at(1, 3).u32 == 10 + vf);
        CHECK(bank->vf[vf].QUEUE.at(1, 2).u32 == 0);
        CHECK(map.mmio_write(base + 0xA8, 20 + vf).success());
        CHECK(bank->vf[vf].POOL_CONFIG.at(1, 3).u32 == 20 + vf);
        CHECK(map.mmio_write(base + 0x128, 30 + vf).success());
        CHECK(bank->vf[vf].UNAFFECTED.at(1, 2).u32 == 30 + vf);
        CHECK(bank->vf[vf].UNAFFECTED.at(1, 3).u32 == 0);
        CHECK(map.mmio_write(base + 0x188, 40 + vf).success());
        CHECK(bank->vf[vf].COUNTER.at(3).u32 == 40 + vf);
    }
    CHECK(device.calls == 24);
    CHECK(map.mmio_write(0x28, 77).success()); // PF bypasses the resolver.
    CHECK(bank->pf.QUEUE.at(1, 2).u32 == 77 && device.calls == 24);
    map.set_identity_resolver(VfRoute, [](const reg::IdentityResolveRequest& r) {
        return r.request_id == 2 ? 6u : r.request_id;
    });
    bool called = false;
    bank->vf[6].QUEUE.on_write([&](const auto& info) {
        CHECK(info.request_id() == 2 && info.resolved_id() == 6);
        CHECK(info.index_as(VlPool) == 2 && info.mapped_as(LPool) == 3);
        const auto [row, pool] = info.index.get();
        CHECK(row == 1 && pool == 3);
        called = true;
    });
    CHECK(map.mmio_write(0x1000 + 2 * 0x200 + 0x28, 99).success());
    CHECK(called && device.last_vf == 6 && bank->vf[6].QUEUE.at(1, 3).u32 == 99);
    std::cout << "PASS: declarative selective 1D/2D index rules, 8 VFs, PF passthrough and VF "
                 "remap ordering\n";
}
