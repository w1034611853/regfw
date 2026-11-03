# RegEntity 与 RegView

[返回 README](../README.md) · 下一步：[View 布局](AddressViews.md)

先声明存储，再声明入口。`REG` 创建框架持有的 `RegEntity`，寄存器成员本身是框架自动提供的 `RegView`，类似指向这份实体的带类型 span。设备不用创建 view 数组，也不用手动 bind。

| 概念 | 含义 |
|---|---|
| `RegEntity` | 真实值、数组形状、behavior、回调、事务与 reset 状态；由 bank 持有 |
| `RegView<T,...>` | 非拥有的带类型句柄；复制句柄不会复制值 |
| `RegEntityBundle` | 声明寄存器存储的组 |
| `REG_ENTITY` / `REG_REPEAT_ENTITY` | 创建一组 / N 组独立实体；无 aperture 参数时没有总线入口 |
| `reg::view(name,size).map(target)` | 把当前地址区间连接到明确的单个目标；框架安装解码项 |

下面的实体定义来自 [完整示例](../examples/pf_vf_layout.hpp)。`reg::at(...)` 是寄存器在组内的相对偏移；总线地址由后面的 View 决定。

<!-- GENERATED ENTITIES BEGIN -->
```cpp
struct ModuleRegs : reg::RegEntityBundle {
    REG_BUNDLE(ModuleRegs);
    REG(DEVICE_ID, std::uint32_t, reg::at(0x00).reset(0x6188).access(reg::ro));
    REG(STATUS, std::uint32_t, reg::at(0x08));
    REG(TIMESTAMP,
        std::uint64_t,
        reg::at(0x20).access(reg::ro).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
    REG(STATS, std::uint32_t, reg::at(0x80).array(reg::dims(4), reg::strides(4)));
};
struct PfRegs : reg::RegEntityBundle {
    REG_BUNDLE(PfRegs);
    REG(CONTROL, std::uint32_t, reg::at(0));
    REG(CPU_SHARED, std::uint32_t, reg::at(0x40));
    REG(GROUP_SHARED, std::uint32_t, reg::at(0x80));
    REG(DESC_BASE, std::uint64_t, reg::at(0xc0));
};
struct VfRegs : reg::RegEntityBundle {
    REG_BUNDLE(VfRegs);
    REG(CONTROL, std::uint32_t, reg::at(0));
    REG(STATUS, std::uint32_t, reg::at(8).access(reg::ro));
    REG(QUEUE, std::uint32_t, reg::at(0x100).array(reg::dims(2, 4), reg::strides(0x20, 4)));
    REG_ACCESS_GROUP(POOLS,
                     .members = {QUEUE},
                     .coordinates = {{LogicalPool, 1}},
                     .derive = {reg::map_index(LogicalPool, PhysicalPool)
                                    .when(reg::identity_domain(VF_DOMAIN))
                                    .using_resolver(PhysicalPool)
                                    .otherwise(reg::passthrough(LogicalPool))});
};
struct CommandRegs : reg::RegEntityBundle {
    REG_BUNDLE(CommandRegs);
    REG(DOORBELL, std::uint32_t, reg::at(0x10).access(reg::write_only));
};
```
<!-- GENERATED ENTITIES END -->

例中模块存储一份，PF 业务存储一份，VF 业务存储八份。所有 VF 都可以引用同一个 `pf.CPU_SHARED`，但 `.map(vf[i])` 明确对应各自的 VF 实体。

## 内部值访问与总线访问

`bank->vf[3].CONTROL.ref()` 返回真实值引用；`bank->vf[3].QUEUE.at(1, 2).ref()` 定位数组元素。原生 union 寄存器还可直接使用 `.u32`、`.u64`、`.bits` 等值成员。

这些是设备内部访问，不经过 MMIO 权限、地址路由或读写行为。外部总线请求走 `map.mmio_*`；例如只读寄存器的内部值仍允许由设备模型更新。

同一实体被多个窗口映射时，存储、回调、reset 和快照都只有一份。`address()` 返回第一份映射提供的规范地址；它不是“当前这次请求的地址”。回调中需要原始/最终地址时使用 `info.address` / `info.resolved_address`。

## span 式范围操作

一维数组句柄支持 `subspan(offset, count)`；切片只改变所见范围，不生成实体。二维及更高维数组使用 `.at(...)` 定位元素。

| 接口 | 含义 |
|---|---|
| `size()` / `empty()` | 当前句柄的元素范围；元素句柄的 size 为 1 |
| `data()` / `size_bytes()` | 连续的 C++ 值存储及其字节数；空 view 的 data 为 nullptr |
| `subspan(offset, count)` | 一维切片；允许空范围，越界报错 |
| `address()` | 当前范围首元素的规范总线地址；未映射/空范围报 Access 错误 |
| `address_span_bytes()` | 规范地址跨度，包含数组 stride 形成的空洞 |
| `entity()` | 同一实体的身份、逻辑元素数量、存储大小等 |

例如三个 uint32 值占 12 字节，地址 stride 为 8 时跨度是 20 字节。`.size_bytes()` 与 `.address_span_bytes()` 不能混用。

回调和 reset 属于整个实体，切片不产生独立回调状态；access group 的成员必须覆盖整个实体。bank 移动后句柄仍有效；`clear()` 或销毁后，受检的 view 访问会报错。已取得的原生指针/引用遵循 C++ 生命周期规则。

旧名称 `TypedReg` 保留为兼容名称；新代码使用 `RegView` 即可。
