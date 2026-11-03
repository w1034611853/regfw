#include <regfw/regfw.hpp>
#include <cstdint>

REG_ACCESS_FIELD(VfRoute, std::uint32_t);
REG_ACCESS_FIELD(LogicalQueue, std::uint32_t);
REG_ACCESS_FIELD(PhysicalQueue, std::uint32_t);

struct VfRegs : reg::RegEntityBundle {
    REG_BUNDLE(VfRegs);
    REG(QUEUE, std::uint32_t, reg::at(0).array(reg::dims(16), reg::strides(4)));
    REG_STATIC_INDEX(QueueMap, QUEUE, LogicalQueue, PhysicalQueue, 0);
    REG_SCHEMA(&VfRegs::QUEUE);
};
struct PfShared : reg::RegEntityBundle {
    REG_BUNDLE(PfShared);
    REG(STATUS, std::uint32_t, reg::at(0x80));
    REG_SCHEMA(&PfShared::STATUS);
};
struct DeviceRegs : reg::Bundle {
    REG_BUNDLE(DeviceRegs);
    REG_REPEAT_ENTITY(vf, VfRegs, 8, reg::entity().reset_per_instance(reg::ResetScope::Vf));
    REG_ENTITY(shared, PfShared, reg::entity().reset_scope({reg::ResetScope::Pf}));

    REG_STATIC_LAYOUT([] {
        auto vf_window = [](auto i) {
            return reg::static_view("VF_WINDOW", 0x1000)
                .identity(1)
                .using_resolver(VfRoute)
                .map(reg::target<&DeviceRegs::vf>().at(i))
                .map(reg::target<&DeviceRegs::shared>());
        };
        auto device = reg::static_view("DEVICE", 0x8000)
                          .map_repeat("VF", 0, reg::repeat(8).stride(0x1000), vf_window);
        return reg::static_address_space("POE")
            .map(device, 0xff71f0000000ULL)
            .map(device, 0xc100000000ULL);
    });
};
int main() {
    reg::RegisterBank<DeviceRegs> bank(reg::space().name("POE"));
    bank.map().set_identity_resolver(VfRoute, [](const auto& request) {
        return request.request_id;
    });
    bank.map().set_access_resolver(PhysicalQueue, [](const auto& info) {
        return info.index_as(LogicalQueue);
    });
    bank.finish();
    bank.map().mmio_write(0xff71f0000000ULL, 42);
    return bank.map().mmio_read(0xc100000000ULL) == 42 ? 0 : 1;
}
