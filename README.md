# regfw 0.38.0

`sync` 是面向单线程同步设备的分支，包含 MMIO、句柄、钩子与紧凑快照优化。同步版 0.38.0 的发布标签是 `v0.38.0-sync`。设备升级时应配套重编译库和设备，并使用同一版本的头文件。

`REG_ENTITY` 创建存储，`reg::view(name, size)` 组合局部布局，`reg::address_space(name)`
将 View 挂到一个或多个 64 位地址入口。平坦 `REG` 和显式布局共用 `FormulaDecoder`。

设备先构建 bank，再注册 resolver、pre_access 和寄存器回调，最后调用 `bank.finish()`。
访问期间保持 bank 存活，对同一 map 串行调用。升级步骤见[迁移说明](docs/Migration.md)。

带符号工厂的 `REG_LAYOUT([](auto layout) {...})` 首次使用时生成共享布局描述，
几何错误在初始化时报告。复杂声明可用 `REG_LAYOUT_DECL` / `REG_LAYOUT_IMPL` 拆分翻译单元。
见[固定布局](docs/StaticLayout.md)和[源码架构](docs/Architecture.zh-CN.md)。

## 实体与 View 组合

复杂布局优先阅读 [AddressViews.md](docs/AddressViews.md)，包含完整可运行程序和 POE 改写方式：

```cpp
// 布局片段：vf、pf_shared 是已声明的实体；共享组使用业务区内偏移。
auto shared = reg::view("PF_SHARED", BUSINESS_SIZE).map(pf_shared);
auto business = [&](auto i) {
    return reg::view("BUSINESS", BUSINESS_SIZE)
        .map(vf[i])
        .map(shared, 0, 10); // 第三个参数是局部优先级。
};
return reg::address_space("POE_CPU")
    .map_repeat("ENTRY_A", 0xff71f0000000ULL,
                reg::repeat(VF_COUNT).stride(VF_STRIDE), business)
    .map_repeat("ENTRY_B", 0xc100000000ULL,
                reg::repeat(VF_COUNT).stride(VF_STRIDE), business);
```

新 View 的空洞透明；只在真实实体被遮挡时记录 `REGFW_ENTITY_SHADOWED` warning，最高优先级存在歧义则报错。优先级在构建时解析，分发记录直接指向实体；权限/回调失败不会回落到低优先级实体。旧 Window DSL 已删除，布局统一使用 AddressView / AddressSpace。

初始化用 `reg::space().name("POE_CPU")` 选择默认 64 位范围，无需按两个 base 之间的跨度分配目录。EntityLayout Hash 按实际有效地址建立键，不随空洞范围分配内存；高地址不会被截断成低 32 位。所有地址常量使用 `ULL`，头文件和编译库必须一起升级。分发性能的实际对比和限制见 [Benchmarks](docs/Performance.md)。

## 先组合大 View，再注册入口

在一个 View 中用 `map_repeat` 组合 PF/VF 窗口，再挂载整个 View。

```cpp
auto poe_cpu = reg::view("POE_CPU", 32 * MiB)
    .map(pf_view, 0)
    .map_repeat("PF_VF", POE_PF_VF_BASE,
        reg::repeat(POE_VF_COUNT).stride(POE_PF_VF_STRIDE), pf_vf_slot)
    .map_repeat("VF", POE_VF_WINDOW_BASE,
        reg::repeat(POE_VF_COUNT).stride(POE_VF_WINDOW_STRIDE), vf_window);

return reg::address_space("DEVICE")
    .map(poe_cpu, 0xff71f0000000ULL)
    .map(poe_cpu, 0xc100000000ULL);
```

View 中的 offset 都是局部偏移；外层 base 只在挂载时加一次。每个入口映射完整的 32 MiB 布局，两个入口共享实体和事务。初始化使用 reg::space()，避免把完整物理地址限制到 32 MiB。

## 统一实体与事务

所有命令/doorbell/FIFO 端口均声明为 REG 实体；独立 command 区域和 Split64Policy 已删除。所有位宽统一 TransactionPolicy：≤64 bit 默认 INDEPENDENT，>64 bit 默认 LOW_TO_HIGH。常见旧写法迁移见 [Migration](docs/Migration.md)。

### on_read 注意事项

`on_read` 返回的值只用于本次响应或读快照，不会自动覆盖 entity。返回 `std::nullopt`、空 optional 或 void 时，沿用的是 **进入回调前已经计算好的候选读取值**，不会重新读取 entity。因此，普通 RW 实体原值为 `5`，回调将实体改成 `42` 后返回 `std::nullopt`，本次 QEMU 仍读到 `5`，实体则变成 `42`。希望本次就返回新值，需要显式返回该值。RC 等 behavior 的存储副作用仍照常执行；分拍读快照建立后的续读使用该快照。完整示例见 [读取返回值](docs/AgentGuide.md#72-读取返回值)。

## Agent 与完整快照入口

Agent 首先阅读 [AGENT.md](AGENT.md)。面向 Agent 的默认值、准确签名、使用约束和四个完整程序，见 [AgentGuide.md](docs/AgentGuide.md)。新设备优先查该文档，不必每次先阅读源码。

完整快照接口：

```cpp
auto state = reg_bank.save();
reg_bank.load(state);
// 包含 <regfw/vmstate.hpp> 后，沿用已有 module_var_save/load 流：
reg_bank.save(file);
reg_bank.load(file);
// 父对象也可直接 VMSTATE_FIELDS(reg_bank, fifo, vf_route)。
```

包含值、所有位宽统一的 transaction 写暂存/读快照；恢复后接着完成命令，不重放回调。用原有 vmstate_fields，不要求 VMStateDescription。准确的流接入、外部状态范围和格式兼容要求见 [Snapshot.md](docs/Snapshot.md)。

## 更易读的访问日志

Debug 一行摘要；Trace 单行显示结果、地址路径和身份/下标变化。路由显示名称，省略未变化的重复字段，ERROR 保留完整路径和原因链。框架日志不再附带完整 C++ 函数签名。例如：

```text
WRITE POE_GRP_VCPU_MASK[1][0]: 0x00000000 -> 0x00000030 | addr: 0x01225020 -> 0x00325020 | size=4B | write=0x00000030 | mask=0xffffffff | route=VF_TO_PF | vf=1 | domain=1 | group=VF_VCPU_TO_LCPU
```

仅发生转换时打印 `vf=1->6` 或 `index=[1][0]->[1][1]`；不再打印难以辨识的数字路由或重复的展平下标。完整约定见 AgentGuide §18.1。

## 构建并运行

需要 CMake ≥ 3.20、C++20 编译器。Linux x86_64 发布包自带 hlog 3.2.1 的头文件和 runtime，可直接构建。使用源码目录时，另外传入 `-DHLog_ROOT=/absolute/path/to/hlog`；该目录应包含 `include/hlog.h` 和 `lib/libhlog.so`。

GitHub 克隆版将预编译的 hlog 共享库以 `.xz` 保存。首次构建前执行 `bash scripts/restore_hlog.sh` 恢复原始 `.so` 文件；此操作不修改框架源码。

在本 README 所在目录执行：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DREGFW_BUILD_EXAMPLES=ON
cmake --build build --parallel 2
./build/regfw_quickstart
./build/regfw_pf_vf
./build/regfw_pf_vf --layout
./build/regfw_pf_vf --explain
ctest --test-dir build --output-on-failure
```

只有两个公开示例，按下表顺序阅读即可：

| 示例 | 阅读内容 |
|---|---|
| [quickstart.cpp](examples/quickstart.cpp) | 一个实体组、一个 View、回调和实际 MMIO；完整代码见下文 |
| [pf_vf_layout.hpp](examples/pf_vf_layout.hpp) + [pf_vf.cpp](examples/pf_vf.cpp) | 32 MiB、PF + 8 VF、共享寄存器、身份/下标路由、命令、reset 和快照 |

`regfw_pf_vf --layout` 只显示 View/Entity 大布局；`--layout-full` 增加紧凑寄存器地址表。Debug 自动输出前者，Trace 输出后者；多维数组按 dims/strides/地址公式折叠，不逐地址展开。过时的 aperture/alias 示例及其旧教程已删除。

## 最小完整用法

<!-- GENERATED QUICKSTART BEGIN -->
```cpp
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
```
<!-- GENERATED QUICKSTART END -->

## 继续阅读

| 你要做什么 | 文档 |
|---|---|
| 复杂布局、优先级叠加、64 位多入口 | [实体与 View](docs/AddressViews.md) |
| 理解实体与 span 式句柄，内部访问寄存器值 | [RegEntity / RegView](docs/RegEntity-RegView.md) |
| 绑定路由与读写回调、处理命令、reset 和快照 | [设备逻辑](docs/Device.md) |
| 检查布局表、解释单个地址、理解访问错误 | [诊断与访问语义](docs/Diagnostics.md) |
| 嵌入工程、安装、hlog 与 QEMU C 接口 | [工程接入](docs/Integration.md) |
| 从旧 aperture/alias 代码迁移 | [迁移说明](docs/Migration.md) |
| 复测 MMIO、句柄及初始化 | [性能报告](docs/Performance.md) |
| 查看本版范围与发布验证方法 | [CHANGELOG](CHANGELOG.md) · [发布流程](docs/Release.md) |

## 构建开关

| 开关 | 默认值 | 效果 |
|---|---|---|
| `REGFW_BUILD_EXAMPLES` | `OFF` | 构建两个公开示例 |
| `REGFW_BUILD_TESTS` | 顶层 `ON`，被 `add_subdirectory` 引入时 `OFF` | 构建测试和所需示例，并注册 CTest；文档检查需要 Python ≥ 3.9 |

`REGFW_BUILD_TESTS=OFF` 且 `REGFW_BUILD_EXAMPLES=ON` 时，只构建库与这两个示例。源码安装导出的目标为 `regfw::regfw`。框架本身不要求 fmt；使用 hlog C++ 格式化接口的目标需另外链接 fmt。
