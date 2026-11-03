// 完整示例：布局见 pf_vf_layout.hpp；此处绑定设备逻辑并验证实际访问。
#include "pf_vf_layout.hpp"
#include <array>
#include <iostream>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace example {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct Device {
    std::array<std::uint32_t, VF_COUNT> route{};
    unsigned route_calls = 0, index_calls = 0, command_calls = 0;
    unsigned last_request = 99, last_resolved = 99;
    std::uint64_t last_command_offset = 0, ticks = 0x1122334455667788ull;
    reg::RegisterBank<DeviceRegs> bank;

    Device() : bank(reg::space(SPACE_SIZE).name("POE")) {
        std::iota(route.begin(), route.end(), 0);
        auto& map = bank.map();
        // DOC-BEGIN callbacks
        map.set_identity_resolver(
            VfRoute, "Device::route_vf", [&](const reg::IdentityResolveRequest& request) {
                ++route_calls;
                return route.at(request.request_id);
            });
        map.set_access_resolver(
            PhysicalPool, "Device::route_pool", [&](const reg::RegAccessInfo& info) {
                ++index_calls;
                return (info.index_as(LogicalPool) + 1) % 4;
            });
        bank->vf[3].STATUS.on_read([]() -> std::optional<std::uint32_t> {
            return 777;
        });
        bank->module.TIMESTAMP.on_read([&] {
            return ticks++;
        });
        bank->pf_commands.DOORBELL.on_write([&](const reg::RegAccessInfo&) {
            ++command_calls;
        });
        for (unsigned i = 0; i < VF_COUNT; ++i) {
            bank->vf_commands[i].DOORBELL.on_write([&, i](const reg::RegAccessInfo& info) {
                ++command_calls;
                last_request = info.request_id();
                last_resolved = info.resolved_id();
                last_command_offset = info.resolved_address - (vf_window(i) + MiB);
            });
        }
        bank.finish(); // 绑定完设备逻辑后再校验并冻结布局。
        // DOC-END callbacks
    }

    void demonstrate() {
        auto& map = bank.map();
        require(map.layout_warnings().empty(), "unexpected layout warning");
        require(route_calls == 0 && index_calls == 0, "finish must not call resolvers");
        // DOC-BEGIN access
        // VF3 的业务入口与 PF 的 VF3 槽位指向同一份 vf[3]。
        require(map.mmio_write(vf_window(3) + HEAD_SIZE, 0xa3).success(), "VF write");
        require(bank->vf[3].CONTROL.ref() == 0xa3, "VF entity");
        require(map.mmio_read(pf_slot(3) + HEAD_SIZE) == 0xa3, "PF slot shares VF storage");

        // 所有 VF 的 +0x40 都指向 PF 的同一个 CPU_SHARED。
        require(map.mmio_write(vf_window(5) + HEAD_SIZE + 0x40, 0x77).success(), "shared write");
        require(bank->pf.CPU_SHARED.ref() == 0x77, "shared entity");
        require(map.mmio_read(pf_slot(0) + HEAD_SIZE + 0x40) == 0x77, "shared PF slot");
        require(map.mmio_read(vf_window(7) + HEAD_SIZE + 0x40) == 0x77, "shared VF window");

        // 动态身份：请求 VF2 -> vf[6]；两个入口使用同一 resolver key。
        route[2] = 6;
        const auto before = route_calls;
        require(map.mmio_write(vf_window(2) + HEAD_SIZE, 0x66).success(), "redirect write");
        require(bank->vf[6].CONTROL.ref() == 0x66 && bank->vf[2].CONTROL.ref() == 0,
                "redirect target");
        require(route_calls == before + 1, "resolve identity once");
        require(map.mmio_read(pf_slot(2) + HEAD_SIZE) == 0x66, "same redirect from PF slot");

        // QUEUE[1][2] -> vf[6].QUEUE[1][3]；身份转换先于数组下标转换。
        require(map.mmio_write(vf_window(2) + HEAD_SIZE + 0x128, 0x55).success(), "queue write");
        require(bank->vf[6].QUEUE.at(1, 3).ref() == 0x55 && index_calls == 1, "mapped queue");
        // DOC-END access

        // 只读模块、模型读值、64 bit transaction 与显式 8 字节写。
        require(map.mmio_write(vf_window(0) + 8, 99).result == reg::RegMemTxResult::Ignored,
                "RO window");
        require(bank->module.STATUS.ref() == 0, "RO window preserved storage");
        require(map.mmio_write(0, 99).result == reg::RegMemTxResult::Ignored, "RO register");
        require(map.mmio_read(vf_window(3) + HEAD_SIZE + 8) == 777, "model read");
        require(bank->vf[3].STATUS.ref() == 0, "model read preserved storage");
        require(map.mmio_read(0x20) == 0x55667788 && map.mmio_read(0x24) == 0x11223344,
                "64-bit transaction snapshot");
        require(map.mmio_write_sized(HEAD_SIZE + 0xc0, 0x123456789abcdef0ull, 8).success(),
                "64-bit write");
        require(bank->pf.DESC_BASE.ref() == 0x123456789abcdef0ull, "64-bit value");

        require(map.mmio_write(MiB + 0x10, 1).success(), "PF command");
        require(map.mmio_write(vf_window(2) + MiB + 0x10, 1).success(), "VF command");
        require(command_calls == 2 && last_request == 2 && last_resolved == 6 &&
                    last_command_offset == 0x10,
                "command identity and offset");
        require(map.try_mmio_read_sized(vf_window(0) + MiB, 4).result ==
                    reg::RegMemTxResult::DecodeError,
                "undeclared command address");
        for (auto address : {pf_slot(0), 10 * MiB, vf_window(0) + HEAD_SIZE + 0x10})
            require(map.try_mmio_read_sized(address, 4).result == reg::RegMemTxResult::DecodeError,
                    "unmapped hole");

        // DOC-BEGIN state
        const auto saved = map.dump_state_versioned();
        map.reset_matching({reg::ResetRequest::VfFlr, 6});
        require(bank->vf[6].CONTROL.ref() == 0 && bank->vf[3].CONTROL.ref() == 0xa3,
                "VF FLR isolation");
        require(bank->pf.CPU_SHARED.ref() == 0x77, "shared PF entity survives VF FLR");
        map.restore_state_versioned(saved);
        require(bank->vf[6].CONTROL.ref() == 0x66, "snapshot restore");
        // DOC-END state
        std::cout << "PASS: PF/VF shared storage, identity/index routing, RO, callbacks, commands, "
                     "reset and snapshots\n";
    }
};
} // namespace example

int main(int argc, char** argv) try {
    using namespace example;
    Device device;
    auto& map = device.bank.map();
    const std::string_view option = argc > 1 ? argv[1] : "";
    if (argc == 1)
        device.demonstrate();
    else if (argc != 2)
        throw std::runtime_error("expected a single option");
    else if (option == "--layout")
        std::cout << map.describe_layout();
    else if (option == "--layout-full")
        std::cout << map.describe_layout(reg::LayoutDetail::Full);
    else if (option == "--explain") {
        // DOC-BEGIN explain
        // VF2 的 QUEUE[1][2]；只解释规则，不调用设备路由函数。
        const auto address = vf_window(2) + HEAD_SIZE + 0x128;
        std::cout << map.explain_address(address);
        // 假设 resolver 返回 6，预览对应实体；数组映射仍以符号规则显示。
        std::cout << map.explain_address(address, 4, reg::RegTransactionKind::Read, 6);
        // DOC-END explain
    } else
        throw std::runtime_error("usage: regfw_pf_vf [--layout|--layout-full|--explain]");
    if (argc > 1)
        require(device.route_calls == 0 && device.index_calls == 0, "inspection called a resolver");
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
