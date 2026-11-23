# regfw 0.36.0：共享 EntityLayout 地址分发

## 核心执行路径

```text
REG/Entity/Bundle declaration + View DSL
    → factory (普通 C++，首次使用时每种布局构建一次)
    → RuntimeFormulaPlan: bindings/scopes/identities/mounts/array-shapes
    → installer: instance binding + shared EntityLayout Hash
    → MMIO: View range / priority → Hash.find(local word)
    → PreparedAccessInfo (linear / byte / REG ordinal / available)
    → 原有 Access Engine: pre_access / resolver / permission / transaction / callback / storage
```

- `static_layout.hpp` 中 `build_formula_layout_runtime` 是单遍普通 C++ 函数。不存在 `compile_formula_layout`/`FormulaPlan<N...>` 或两遍 `consteval` 常量求值。
- 寄存器声明类型的 count/width/dims/strides 等简单常量仍由 C++20 `constexpr` 计算。
- 非单调数组的元素地址表使用普通 `std::vector` + `std::sort` 按声明类型只建立一次，避免编译器解释大量排序步骤。
- `FormulaPlanView` 只是一组轻量 span，plan 由拥有 vector 的静态本地对象保持生命周期。
- 每个 EntityLayout 字表记录预计算定位，完整 `RegElementIndex` 按需从实体布局取用。每个 VF/View 绑定自己的 EntityState、Policy、PreAccess、Identity、Callback slot。resolver 结果逐次求值。
- 采用精确 REG 字节占用与优先级/Reserve 检查；View 外包范围交叉时，空洞仍可穿透。
- Hash 目前按每个 4 字节可访问 lane 建键：512-bit 寄存器 16 条键，2048-bit 寄存器 64 条。运行时内存和安装时间与实际有效 lane 数有关；绝不按照完整 AddressSpace 字节跨度分配。
- 同步分支的平坦寄存器也安装一个默认根 View，和显式 AddressSpace 共用 FormulaDecoder。MMIO 解码与校验通过错误结果传递失败。改动和验证见 [同步架构精简报告](Performance-Architecture-Sync.md)。

## 源码模块

- `include/regfw/static_layout.hpp`：DSL 静态布局 factory 的一次性原生展开；声明形状提取与 RuntimeFormulaPlan。
- `include/regfw/detail/formula_plan.hpp`：数组解码公式和共用元数据类型。
- `src/layout/formula.cpp`：静态/动态 View 安装、冲突/Reserve 校验、Identity 绑定。
- `src/internal/formula_decoder.hpp`：共享 EntityLayout Hash、View 绑定及地址查找。
- `src/runtime/access.cpp`：寄存器访问执行、AccessInfo 装配、Scalar/Mapped/Generic、Callback 与 Transaction。

## 验证

共享几何、地址空洞和非单调数组由 `formula_geometry`、`formula_regressions` 和
`formula_fastpaths` 验证。 `formula_diff` 比较动态和静态布局的结果，
`formula_golden` 对比冻结的可见地址记录。当前性能与容量取舍见
[同步架构精简报告](Performance-Architecture-Sync.md)。
