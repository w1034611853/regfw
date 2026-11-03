# 固定布局的编译期计划（0.27.0）

此入口适用于设备几何、数组数量和地址入口固定的模型。原 Entity / View、值存储、回调、事务、reset 和 snapshot 继续使用。已有动态布局不会自动变成静态布局。

## 完整示例

```cpp
#include <regfw/regfw.hpp>
#include <cstdint>

REG_ACCESS_FIELD(VfRoute, std::uint32_t);
REG_ACCESS_FIELD(LogicalQueue, std::uint32_t);
REG_ACCESS_FIELD(PhysicalQueue, std::uint32_t);

struct VfRegs : reg::RegEntityBundle {
    REG_BUNDLE(VfRegs);
    REG(QUEUE, std::uint32_t,
        reg::at(0).array(reg::dims(16), reg::strides(4)));
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
    REG_REPEAT_ENTITY(vf, VfRegs, 8,
        reg::entity().reset_per_instance(reg::ResetScope::Vf));
    REG_ENTITY(shared, PfShared,
        reg::entity().reset_scope({reg::ResetScope::Pf}));

    REG_STATIC_LAYOUT([] {
        auto vf_window = [](auto i) {
            return reg::static_view("VF_WINDOW", 0x1000)
                .identity(1).using_resolver(VfRoute)
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
    bank.map().set_identity_resolver(VfRoute,
        [](const auto& request) { return request.request_id; });
    bank.map().set_access_resolver(PhysicalQueue,
        [](const auto& info) { return info.index_as(LogicalQueue); });
    bank.finish();
    bank.map().mmio_write(0xff71f0000000ULL, 42);
    return bank.map().mmio_read(0xc100000000ULL) == 42 ? 0 : 1;
}
```

`REG_SCHEMA` 只列成员，不再写一次地址、宽度或数组信息。组内若含子实体，也列对应子实体成员；嵌套目标可写 `target<&Root::group, &Group::child>()`。每遇到路径中的重复实体用一次 `.at(i)` 指定实例；实体内部数组不通过此处选择元素。重复的 schema 成员在编译期拒绝，遗漏成员在实例绑定时拒绝，不能少映射一部分寄存器。

重复窗口工厂接收常量求值中的整数，不访问真实 VF 对象。多个窗口指向同一个 `target<&Root::shared>()` 时只绑定同一份存储；只有 REG_REPEAT_ENTITY 创建不同存储。

## 哪些工作移到了编译期

- 展开实际声明的寄存器与入口；不按两个高地址之间的空洞分配表。
- 验证 View 范围、重复族几何、目标实例选择、数组形状/地址算术。
- 选择优先级赢家、合并路径权限，保留真实覆盖的 warning 描述；同优先级实际寄存器冲突直接导致编译失败。
- 确定直接/identity 解析流程、身份窗口及可预绑定的 VF 目标索引。
- 生成只读地址索引；初始化不再排序或构造这些索引。
- REG_STATIC_INDEX 的维度、范围与线性系数；仅支持单 uint32 坐标、无条件、命名 resolver。

machine init 仍要分配实体和事务状态、绑定真实存储地址、命名槽与捕获对象、构造实例诊断名称，以及验证 runtime space 和 schema 与实际对象相符。不能把这部分描述为零初始化成本。普通 `.on_read` / `.on_write` 和命名函数的绑定方式继续保留，捕获对象仍属于该 bank 实例。

resolver 的结果没有缓存：每次 MMIO 都调用当前绑定，路由表变化、resolver 换绑/清除、动态 logger/pre_access 继续生效。只有静态目标关系和流程被预先确定。

## 与原动态入口的关系

| 需求 | 推荐入口 |
|---|---|
| 固定 Bundle、数组、重复 VF、多入口、共享 PF、优先级 | REG_STATIC_LAYOUT |
| 命名 identity / View pre_access | 静态入口支持 `.identity(...).using_resolver(...)` / `.using_pre_access(...)` |
| 只读别名、reserve、方向标记 | 静态入口对应方法同动态 View |
| 初始化时读取配置决定布局/数量 | 原 REG_LAYOUT |
| REG_DYNAMIC、单 REG / 切片映射、内联 View 捕获钩子 | 原 REG_LAYOUT |
| 条件、多字段、metadata_only、自定义 index 转换 | 原 REG_ACCESS_GROUP；可挂在静态布局的实体上 |

多个 bank 共享静态表，各 bank 的寄存器值和函数捕获状态独立。

同一个 map 不能同时安装两套根布局。静态计划的工厂必须无捕获并可常量求值；不要把 `this` 或真实 RegView 放进它。完整按寄存器和事务的能力不因静态布局而删减。

## 编译、内存与调试

编译器需要 C++20 常量求值中的临时 vector / sort 支持；验证使用 GCC 13.3。静态编译确实会增加编译时间。大型布局若超过 GCC 默认运算限额，可对设备目标配置：

```cmake
target_compile_options(poe_device PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-fconstexpr-ops-limit=268435456>)
```

计划含每个实际映射 word 的静态记录、地址 hash 索引和有限的 VF 目标索引；内存随实际映射数量增长。大量宽数组、多别名仍可能生成较大的只读段，不能把它称为始终只保存一个公式。各 bank 共享常量表，绑定记录/实体值仍独立。

动态路径保留原来的页目录；静态路径直接使用编译好的只读 word 索引。DEBUG/TRACE 布局、explain_address 和覆盖 warning 仍可用，不执行业务 resolver。计划和函数指针不进入快照，恢复前创建同样布局并绑定业务状态，快照格式仍 v2。

性能数据、编译成本和验证范围见 [MmioPerformance](Performance.md)。


常量索引按布局密度选择：平均每个已用 4 KiB 页至少 256 个 word 时，生成只读页目录和页内 word ID；否则生成只读 word hash。前者每个 word 的页内索引成本不高于 16 B，也避免密集寄存器逐 word hash；后者避免孤立寄存器各占一整页。索引由同一编译期工厂生成，不在启动时重新分析密度。
