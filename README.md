# regfw 0.37.0

**Production backend: runtime-built shared EntityLayout Hash.** Existing REG,
REG_LAYOUT, REG_LAYOUT_IMPL, View/Repeat, resolver, callback, transaction and
snapshot APIs are unchanged.

A static layout factory runs as ordinary native C++ **once per layout type**
(on first use), creating compact binding/scope/array descriptions. A bank
binds its Entity instances and builds shared local-address → PreparedAccessInfo
hashes, so individual Views reuse the same immutable geometry. Normal MMIO
performs View range/priority selection, one local Hash lookup and then uses the
existing access engine. No full-layout `consteval`, per-word static plan,
Zone/Run/PageDirectory dispatcher or hot-address cache is compiled into the
production backend. The user does not need any backend-selection CMake option.

Only small **per-REG type** constants (dimension products, width, strides) are
`constexpr`; non-monotone record tables are constructed by native C++ once
per declaration type rather than constexpr-sorted. Layout validation is done
at installation and reports errors at runtime; capture-free factory/type
requirements still fail at compile time. The initializer can still allocate
hash entries proportional to actually accessible 4-byte lanes (wide REGs cost
more), so measure startup and RSS in the real target.

See [0.37.0 重构报告](RELEASE-REPORT-0.37.0.md), [模块架构说明](docs/Architecture.zh-CN.md), [backend design](docs/FormulaBackend.md),
[layout compilation](docs/LayoutCompilation.md), and
[0.37.0 validation](VALIDATION.md).

**0.37.0 只调整源码组织和格式，不调整公开 DSL、寄存器访问语义或快照格式。**

C++20 寄存器框架。`REG_ENTITY` 创建存储，`reg::view(name,size)` 组合可复用的局部布局，`reg::address_space(name)` 将同一 View 挂载到多个 64 位地址入口。框架管理解码、回调、复位、事务与完整快照。

## 固定布局：单遍运行时生成共享描述（0.36.0 起）

固定布局依然使用 `REG_LAYOUT([](auto layout){...})` 或 `REG_LAYOUT_IMPL`，业务源代码不必改写。编译器只进行 REG 声明的基本类型/宽度检查；首次使用布局时，普通 C++ 执行一次工厂并生成共享公式描述，安装时绑定 Entity 实例并填充 EntityLayout Hash。几何冲突和 reserve 等检查由初始化阶段报告，不再耗费大型 `consteval` 计算。

实体成员仍由声明宏自动收集，`layout.regs` 仍选择对应实例。0.27 的显式 REG_SCHEMA/target 接口继续兼容；偏移、宽度和数组形状只在 REG 中声明。完整示例见 [StaticLayout.md](docs/StaticLayout.md)，当前实现细节以 [FormulaBackend.md](docs/FormulaBackend.md) 为准。

REG 定义、编译、回调和事务行为保持不变；启动时仍需分配存储及构建有效地址键值，真实大布局的初始化时间和 RSS 应单独测量。`REGFW_FAST_COMPILE_INIT` 仍是可选编译优化设置，并不是切换布局后端的开关。

## 实体与 View 组合（0.23.0）

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

0.23.2 起，AddressView 同样支持 map_repeat。下面为已有工厂的组合片段：

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
| 描述地址、递归子窗口、重复区域、共享映射 | [Window 布局](docs/AddressViews.md) |
| 绑定路由与读写回调、处理命令、reset 和快照 | [设备逻辑](docs/Device.md) |
| 检查布局表、解释单个地址、理解访问错误 | [诊断与访问语义](docs/Diagnostics.md) |
| 嵌入工程、安装、hlog 与 QEMU C 接口 | [工程接入](docs/Integration.md) |
| 从旧 aperture/alias 代码迁移 | [迁移说明](docs/Migration.md) |
| 查看本版范围与发布验证方法 | [CHANGELOG](CHANGELOG.md) · [发布流程](docs/Release.md) |

## 构建开关

| 开关 | 默认值 | 效果 |
|---|---|---|
| `REGFW_BUILD_EXAMPLES` | `OFF` | 构建两个公开示例 |
| `REGFW_BUILD_TESTS` | 顶层 `ON`，被 `add_subdirectory` 引入时 `OFF` | 构建测试和所需示例，并注册 CTest；文档检查需要 Python ≥ 3.9 |

`REGFW_BUILD_TESTS=OFF` 且 `REGFW_BUILD_EXAMPLES=ON` 时，只构建库与这两个示例。源码安装导出的目标为 `regfw::regfw`。框架本身不要求 fmt；使用 hlog C++ 格式化接口的目标需另外链接 fmt。

紧凑寄存器地址表按每行代表的最小实际可见入口地址进行 64 位数值升序排序；数组仍只列一行。部分覆盖时不使用可能已隐藏的数组首地址作为排序依据。

CMake 的 CMP0144 在支持该策略的版本上设置为 NEW；HLog_ROOT / HLOG_ROOT 均可作为 hlog 安装前缀。源码 add_subdirectory 与安装包 find_package 两条入口均处理此策略。

布局表中的地址映射行统一显示 `VIEW`；映射实体时显示 `VIEW → entity: <原始实体组或 REG 名称>`，`reset` 表示目标存储的复位归属。PF、PF_VF、VF 的多个入口会显示同一个共享实体目标名，不再把每个入口标成新 ENTITY。折叠重复族的目标名/复位信息取 i=j=k=0 代表实例；实体范围列是映射边界，不表示整段地址都有实体寄存器。

## 按 AddressView 绑定 pre_access（0.24.0）

优先使用命名键，布局只声明挂载范围，在 bank 初始化后绑定设备函数：

```cpp
REG_PRE_ACCESS(CheckBusiness); // 放在命名空间作用域

// REG_LAYOUT 中的片段，vf[i] 为此前声明的实体实例：
auto business = reg::view("BUSINESS", POE_BUSINESS_SIZE)
    .map(vf[i])
    .using_pre_access(CheckBusiness);

// bank 初始化后、finish 前：
reg_bank.map().set_pre_access(CheckBusiness,
    [this](const reg::RegAccessInfo& info) {
        // 此处做设备检查；info.address 是原请求地址。
        // info.view_path 是 identity 转换后实际选中目标的 View 路径。
    });
```

独立的小函数也可直接 `.pre_access(fn)`，fn 签名为 `void(const RegAccessInfo&)`。builder 返回新对象，必须使用返回值；同一 View 只能声明一个钩子。不要在 REG_LAYOUT 中捕获局部变量的引用，或捕获可能随 Bundle 构造返回而移动的 this；设备函数推荐使用命名键，等 bank 构建完成后绑定。重复工厂里的 i 如需直接捕获必须按值。

钩子执行顺序为 **全局 map.set_pre_access(fn) → 最终选中路径的父 View → 子 View**，发生在 identity、权限/宽度和 access-group 下标解析后、behavior/事务更新前。动态 VF i→j 时执行 j 的实际目标路径，保留来源 info.address/request_id，info.index/resolved_id 已是转换结果。只执行获胜分支；透明空洞落到另一分支时不会执行空洞 View 的钩子；共享实体在不同入口可使用不同检查。

每次有效 MMIO beat 都调用，包括宽 REG 的暂存写和快照续读。RO 忽略写、入口权限拒绝、未映射/保留地址及 resolver 失败不会运行钩子。内部 ref()/data 更新、reset、save/load、describe_layout、explain_address 不运行钩子。

命名键未绑定时 finish 报 Configuration；有意延迟绑定可 map.defer_pre_access(key)。之后可 set/clear/has_pre_access(key)；clear 只清该键，不影响全局钩子或其他键。已声明但未绑定的 View 钩子在访问时按钩子失败处理，不会静默放行；需要禁用时显式绑定空函数。重绑/clear 与 MMIO 由设备同步，不新增线程安全。

异常沿用原有 pre_access 行为：打印并停止后续钩子、behavior 和寄存器回调；写返回 Ignored，读返回 0（状态可能为 Ok），不回退其他映射。不要用此异常机制要求明确的总线 AccessError；此类需求仍在解析/验证阶段处理。钩子是 void，不使用返回 false 表达拒绝。

钩子列表在构建时预编译，无钩子的记录为空指针；MMIO 不按 View 数量扫描。函数、捕获对象和钩子链不保存到 bank 快照；恢复前按设备初始化流程重建布局并绑定，外部业务状态由父对象保存。

View pre_access 的测量方法、完整数据和适用边界见 [性能验证](docs/Performance.md)。完整可运行命名钩子示例见 [view_pre_access.cpp](docs/snippets/view_pre_access.cpp)。

0.24.1 优化了钩子调用：普通 MMIO 调用不复制函数的 shared_ptr；仅当回调在 pre_access 链执行期间重绑/清除时，保留旧函数至本次 pre_access 链结束。回调内自修改仍受支持，后续钩子读取最新绑定；旧捕获对象的析构时点可能延后到本次 pre_access 链结束。框架仍不支持未同步的并发重绑。

## 历史实现记录：0.26.0 MMIO 分发表（0.36.0 已删除）

> 此节描述早期分发表的演进背景，不是当前 0.36.0 的地址解码实现。0.36.0 使用共享 EntityLayout Hash；当前行为与约束见 [FormulaBackend.md](docs/FormulaBackend.md)。

公开 Entity/View、resolver、pre_access、回调和 transaction 写法保持。finish 把稀疏页目录冻结为连续探测表，并为已映射地址生成 32 字节分发记录；原生 32-bit RW 的数据地址、掩码和静态允许方向已确定，不再逐拍通过冷元数据计算存储地址。大地址间的空洞仍不分配页。

原生 32-bit 无 identity/index 派生或 pre_access、方向 Independent 且没有访问日志时，回调使用轻量调用帧。完整 info 在与用户函数一起编译的适配器中准备，让编译器删除该函数未使用的字段准备；需要完整字段、按值复制 info 或 typed index 的回调仍得到全部语义。读回调的两条适配路径共享同一个可变函数对象，不能各自复制一份计数或 FIFO 状态。异常和回调内启用日志时仍能构造完整诊断信息。

常见的单 uint32 坐标、无条件命名 index 映射减少字段查找；仍逐拍调用当前 resolver，并执行原有维度范围、映射冲突和容量检查。其他条件、多个字段、自定义字段转换、部分访问、RC/W1C、自定义 codec/behavior、宽寄存器和事务保留完整路径。

晚绑定回调、resolver 换绑/清除、全局/View pre_access 和动态日志等级仍生效。on_read 返回值不写回实体，nullopt 使用回调前候选值；读完成顺序、异常和快照 v2 不变。回调 info 只在同步调用期间借用，值复制保留有效字段；名称/View 路径仍借用，禁止 memcpy 整个 info。每 Map 仍需外部同步，不允许同 Map MMIO 重入。

本版以额外的紧凑分发记录和冻结页索引换取访问速度；读源保存一份函数状态和两条调用适配器，初始化开销增加。分发表、函数和缓存指针不进入快照。头文件和库必须一起升级并重新编译。数据、额外内存说明和测试范围见 [MMIO 性能报告](docs/Performance.md)。