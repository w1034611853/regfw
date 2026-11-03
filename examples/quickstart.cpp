// 最小示例：创建存储 -> 描述地址窗口 -> 绑定回调 -> finish -> MMIO。
// DOC-BEGIN quickstart
#include <regfw/regfw.hpp>
#include <iostream>
#include <stdexcept>

struct ControlRegs : reg::RegEntityBundle {
    REG_BUNDLE(ControlRegs);
    REG(CONTROL, std::uint32_t, reg::at(0x00).reset(0));
    REG(STATUS, std::uint32_t, reg::at(0x04).access(reg::ro).reset(7));
};
struct DeviceRegs : reg::Bundle {
    REG_BUNDLE(DeviceRegs);
    REG_ENTITY(core, ControlRegs); // 只有一份存储；这里不分配总线地址。
    REG_LAYOUT([&] {
        return reg::address_space("DEVICE").map(reg::view("CORE", 0x100).map(core), 0x100);
    });
};

int main() {
    reg::RegisterBank<DeviceRegs> bank(reg::space(0x1000).name("DEVICE"));
    unsigned writes = 0;
    bank->core.CONTROL.on_write([&] {
        ++writes;
    });
    bank.finish();

    auto& map = bank.map();
    const auto result = map.mmio_write(0x100, 42); // 默认 4 字节访问。
    if (!result.success() || bank->core.CONTROL.ref() != 42 || writes != 1)
        throw std::runtime_error("CONTROL write failed");
    if (map.mmio_read(0x104) != 7 ||
        map.mmio_write(0x104, 99).result != reg::RegMemTxResult::Ignored)
        throw std::runtime_error("STATUS must remain read-only");
    std::cout << "CONTROL=42 STATUS=7 RO-write=Ignored\n";
}
// DOC-END quickstart
