> **0.36.0 更新：** 本文早期关于 `consteval`、静态索引或编译期几何验证的描述仅供历史参考。当前保留原有 REG_LAYOUT DSL，布局由普通 C++ 在首次使用时构建共享 Formula 描述及 EntityLayout Hash，几何错误在安装阶段报告。请以 [FormulaBackend.md](FormulaBackend.md) 与 [LayoutCompilation.md](LayoutCompilation.md) 为准。

> 0.33.3：前端 API 保持；规则布局直接填最终表、声明形状跨挂载共享，见 [LayoutCompilation](LayoutCompilation.md)。当前测量见 [Performance](Performance.md)。

# 编译期布局：自动成员描述与符号对象（0.33.3）

推荐 `REG_LAYOUT([](auto layout) { ... })`。REG / REG_ENTITY / REG_REPEAT_ENTITY 自动提供成员描述，不再要求 REG_SCHEMA。`layout` 是常量求值中的符号对象，没有运行期设备指针；`.map(layout.vf[i])` 选择已有实体，不分配存储。实际成员和回调仍通过 bank->vf[i] 访问。

```cpp
REG_LAYOUT([](auto layout) {
    auto vf_window = [&](auto i) {
        return layout.view("VF_WINDOW", 0x200000)
            .identity(POE_VF_DOMAIN).using_resolver(VfRoute)
            .map(layout.vf[i])
            .map(layout.shared);
    };
    auto device = layout.view("DEVICE", 8 * 0x200000)
        .map_repeat("VF", 0, reg::repeat(8).stride(0x200000), vf_window);
    return layout.address_space("POE")
        .map(device, 0xff71f0000000ULL)
        .map(device, 0xc100000000ULL);
});
```

以上片段假定此前已声明 vf/shared 和 resolver 键，且实际寄存器不冲突。[完整 POE 示例](snippets/poe_compiled_layout.cpp) 可直接编译运行，包含 PF 自身业务、8 个 VF、PF_VF、8 组 PF 共享实体、512-bit command REG、模块只读别名及双高地址入口。

## 迁移只涉及布局入口与成员引用

- 原 `REG_LAYOUT([&] { ... })` 继续走动态布局，原返回类型、循环赋回写法及捕获行为不变。
- 固定布局改成 `REG_LAYOUT([](auto layout) { ... })`，实际实体引用改成 `layout.member` / `layout.vf[i]` / `layout.outer[i].inner[j]`。
- 构造器写 `layout.view(...)` 和 `layout.address_space(...)`。若保留 `reg::view(...)` 的拼写，在该 lambda 内添加 `namespace reg = ::reg::compiled;`。这个别名只作用于该函数块，不改项目其他代码。
- 不要求 `REG_SCHEMA`、`target<&Type::member>()`、`static_view` 或第二个根布局宏。0.27 的这些显式接口仍支持。
- REG 定义、实体创建、reset scope、命名 pre_access / resolver 和业务回调不变；on_read/on_write 可继续捕获实际设备 this。
- 同一 map 只能有一套根布局，不同时声明 REG_LAYOUT 和 REG_STATIC_LAYOUT。

这是显式选择静态入口，不会把捕获 this 的旧 lambda 悄悄称为编译期执行。若在符号工厂里误用原运行期 reg::view，编译失败；不会静默回退到启动时建表。

## 自动描述机制

每个 REG、REG_DYNAMIC、GROUP、APERTURE、ENTITY 声明产生一个静态成员描述。编译器通过类内重载收集声明顺序，未使用全局 __COUNTER__、行号、friend 注入或运行期注册；同一头文件在不同翻译单元包含顺序不同仍可使用。无需末尾标记、列表、额外生成器或构建步骤。手写构造函数不使用 REG_BUNDLE 时也保留自动描述。

显式 REG_SCHEMA 仍具有优先权，用于兼容 0.27；其遗漏检查不变。正常的新代码应删除该冗余列表。未通过声明宏、直接在构造函数里创建的实体不自动参与静态描述；初始化仍检查实体完整性，禁止静默遗漏。以框架 Bundle/RegEntityBundle 为基类并在类内声明成员；不要依赖用户自定义非空 Bundle 继承来跨类拼接自动 schema。

## 编译期与运行期边界

编译期仍执行 0.27 的布局编译器：范围/冲突验证、覆盖选择、路径权限、VF 身份族、固定流程、目标索引及只读分发表。本版不是运行期缓存，也没有给每次 MMIO 增加符号对象调用；符号对象只用于常量求值。

实体值、事务状态、实际指针、回调捕获对象、诊断字符串仍在初始化时绑定。动态 VF/index resolver 每次访问照常执行，换绑/清除、pre_access 和日志继续生效。事务与快照格式仍为 v2；多个 bank 的值和回调不共享。

静态工厂必须无捕获并可常量求值，局部辅助 lambda 可以捕获该工厂内仍有效的符号对象/局部 view。数量和基址必须可常量求值；读取本次设备配置、REG_DYNAMIC、单 REG/切片映射、内联 View 捕获钩子仍使用原动态 REG_LAYOUT。条件、多字段等 REG_ACCESS_GROUP 可以继续挂在静态映射的实体上。

静态组合返回类型随子节点增加而变化：优先链式 map / map_repeat，不能用 `auto v=layout.view(...); v=v.map(...)` 追加不同类型。旧动态 View 的循环赋回用法继续支持。符号对象的 `view`/`address_space` 方法若与实体成员同名，使用局部 `namespace reg=::reg::compiled;` 构造器。

要求 C++20，验证编译器与结果见 [VALIDATION](../VALIDATION.md)。大型布局可增加 GCC 的常量求值预算：

```cmake
target_compile_options(poe_device PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-fconstexpr-ops-limit=268435456>)
```

自动成员元数据和符号对象会增加编译器工作量；不宣称编译时间不变。只读计划仍按实际占用 word 保存描述，双入口/大数组有代码段及只读数据成本；高地址之间的空洞不分配表。

0.27 的显式接口、详细编译期边界见 [旧静态接口](LegacyStaticLayout.md)。当前性能与验证见 [Performance](Performance.md)。
