# 固定布局与符号前端

推荐 `REG_LAYOUT([](auto layout) { ... })`。REG / REG_ENTITY / REG_REPEAT_ENTITY 自动提供成员描述，不再要求 REG_SCHEMA。`layout` 是工厂使用的符号对象，不包含设备实例指针；`.map(layout.vf[i])` 选择已有实体，不分配存储。实际成员和回调仍通过 bank->vf[i] 访问。

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

带符号参数的布局工厂必须无捕获。声明类型的形状等常量由编译器生成；工厂首次使用时
运行一次，几何检查、排序及地址表安装在初始化阶段完成。按设备实例选择布局或映射单个
REG / 切片时，可使用动态 `REG_LAYOUT([&] {...})`。resolver 与回调在 `finish()` 前注册。

静态组合的返回类型随子节点变化，采用链式 `map` / `map_repeat`。符号 `view` /
`address_space` 与实体成员重名时，使用 `namespace reg = ::reg::compiled;`。

现有显式布局使用 `REG_STATIC_LAYOUT`、`reg::static_view` 和 `reg::target<&Root::member>()`。
`REG_SCHEMA` 可显式列出成员；安装时检查列表完整性。新声明由 REG 宏自动收集成员。
见[实现与验证](FormulaBackend.md)和[分离翻译单元示例](LayoutCompilation.md)。
