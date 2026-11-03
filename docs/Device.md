# 绑定设备逻辑

[返回 README](../README.md) · 前置：[Window 布局](AddressViews.md)

初始化顺序是：创建 bank（或 `init`）→ 绑定 resolver/回调 → `finish()` → 接收 MMIO。下面代码来自 [pf_vf.cpp](../examples/pf_vf.cpp) 的 `Device`，布局类型见 [pf_vf_layout.hpp](../examples/pf_vf_layout.hpp)。

## 路由、模型读值与命令

<!-- GENERATED CALLBACKS BEGIN -->
```cpp
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
```
<!-- GENERATED CALLBACKS END -->

`VfRoute` 是身份路由槽：输入请求实例，返回有效实例。`PhysicalPool` 是数组下标路由槽：根据上下文转换逻辑下标，两者职责不同。

实体定义中的 `REG_ACCESS_GROUP(POOLS)` 指定 `QUEUE` 的第 1 维参与转换；第 0 维保持不变。只有命中 `identity_domain(VF_DOMAIN)` 时才调用 resolver，否则使用声明的 passthrough。`info.index_as(LogicalPool)` 取得请求下标。添加其他数组成员时，同组成员的维度语义必须一致。

resolver 必须在 `finish()` 前绑定；确需晚绑定可先 `defer_identity_resolver(key)` 或 `defer_access_resolver(key)`，未绑定期间相关访问返回 DeviceError。可以在 finish 后替换/清除绑定；window 保存的是稳定函数槽。`has_identity_resolver(key)` / `clear_identity_resolver(key)` 可查询与清除对应槽。

命令与门铃均为 REG 实体，通过 bank 成员绑定普通回调；未声明地址返回 DecodeError，无回调时沿用寄存器 behavior。

## 实际访问与映射结果

<!-- GENERATED ACCESS BEGIN -->
```cpp
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
```
<!-- GENERATED ACCESS END -->

这些检查在 Release 下也执行，CTest 会运行它们。正常输出为 `PASS: PF/VF shared storage, identity/index routing, RO, callbacks, commands, reset and snapshots`。

## 读写回调语义

寄存器 `on_read` 可以返回 `T`、`std::optional<T>` 或 void。返回值用于本次读响应，不写回实体；`nullopt` / void 走 behavior 计算得到的候选值。可接收当前候选值与 info、仅 info 或无参数。详细的 overload 由寄存器类型和维度推导；同一个 on_read 槽只允许绑定一次。

`on_write` 是写行为之后的副作用，可接收 info 或使用无参回调。`reg::ro` 的外部写返回 Ignored，不触发 on_write。内部 `.ref()` / 原生 union 成员赋值不触发这些回调。

`on_read_complete` 用于事务完成后的副作用。transaction 快照的首拍只取一次模型值，之后各拍读取同一快照；Independent 模式逐拍取值。宽寄存器的首 beat 规则见 [诊断与访问语义](Diagnostics.md)。

行为由 `REG` 的 `.access(...)` 声明，例如 `reg::ro`、`reg::w1c`、`reg::rc`；默认是普通读写。`.mask(...)` 设置可写位。不要通过直接赋值模拟外部访问行为。

## reset 与快照

<!-- GENERATED STATE BEGIN -->
```cpp
const auto saved = map.dump_state_versioned();
map.reset_matching({reg::ResetRequest::VfFlr, 6});
require(bank->vf[6].CONTROL.ref() == 0 && bank->vf[3].CONTROL.ref() == 0xa3,
        "VF FLR isolation");
require(bank->pf.CPU_SHARED.ref() == 0x77, "shared PF entity survives VF FLR");
map.restore_state_versioned(saved);
require(bank->vf[6].CONTROL.ref() == 0x66, "snapshot restore");
```
<!-- GENERATED STATE END -->

reset 根据实体上的 scope 执行，不根据当前访问窗口决定。共享 CPU/GROUP 属于 PF，所以 VF FLR 不会清掉它们；`reg::entity().reset_per_instance(reg::ResetScope::Vf)` 给各 VF 配置独立作用域。

版本化快照保存实体数据与 schema，恢复时检查兼容性。多入口不会重复保存同一个实体，命令 REG 也保存实体值与事务状态。不要把设备的外部路由表 `route` 当成寄存器快照的一部分，它仍由设备管理。

## 完整快照（0.21.0）

map.dump_state_versioned/restore_state_versioned 已包含每元素进行中的分拍状态。推荐 bank.save()/load(snapshot)，或沿用 vmstate_fields 的 bank.save(file)/load(file)、VMSTATE_FIELDS(bank,...)。原始 dump_state(span)/restore_state(span) 仍仅处理值数组。参见 [Snapshot](Snapshot.md)。
