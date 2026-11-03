# regfw 0.36.0：共享 EntityLayout 地址分发

## 核心执行路径

```text
REG/Entity/Bundle declaration + View DSL
    → factory (普通 C++，首次使用时每种布局构建一次)
    → RuntimeFormulaPlan: bindings/scopes/identities/mounts/array-shapes
    → installer: instance binding + shared EntityLayout Hash
    → MMIO: View range / priority → Hash.find(local word)
    → PreparedAccessInfo (RegElementIndex 1..8D / linear / byte / REG ordinal)
    → 原有 Access Engine: pre_access / resolver / permission / transaction / callback / storage
```

- `static_layout.hpp` 中 `build_formula_layout_runtime` 是单遍普通 C++ 函数。不存在 `compile_formula_layout`/`FormulaPlan<N...>` 或两遍 `consteval` 常量求值。
- 寄存器声明类型的 count/width/dims/strides 等简单常量仍由 C++20 `constexpr` 计算。
- 非单调数组的元素地址表使用普通 `std::vector` + `std::sort` 按声明类型只建立一次，避免编译器解释大量排序步骤。
- `FormulaPlanView` 只是一组轻量 span，plan 由拥有 vector 的静态本地对象保持生命周期。
- 每个 EntityLayout 的 Hash `.second` 保存预计算定位与完整 `RegElementIndex`，每个 VF/View 绑定自己的 EntityState、Policy、PreAccess、Identity、Callback slot。不会将可动态变化的回调结果与 Resolver 返回值冻结进共享 Hash。
- 采用精确 REG 字节占用与优先级/Reserve 检查；View 外包范围交叉时，空洞仍可穿透。
- Hash 目前按每个 4 字节可访问 lane 建键：512-bit 寄存器 16 条键，2048-bit 寄存器 64 条。运行时内存和安装时间与实际有效 lane 数有关；绝不按照完整 AddressSpace 字节跨度分配。
- `SparseLookup` 仍服务于**手工非 View 映射 API**，不是恢复旧逐字静态编译器；Snapshot/Transaction/Callback 等其他功能保留。

## 源码模块

- `include/regfw/static_layout.hpp`：DSL 静态布局 factory 的一次性原生展开；声明形状提取与 RuntimeFormulaPlan。
- `include/regfw/detail/formula_plan.hpp`：数组解码公式和共用元数据类型。
- `src/formula.cpp`：静态/动态 View 安装、冲突/Reserve 校验、Identity 绑定。
- `src/detail/runtime_impl.hpp`：共享 EntityLayout Hash、View 绑定、地址查找及 AccessInfo 装配。
- `src/runtime.cpp`：寄存器访问执行、Scalar/Mapped/Generic、Callback、Transaction、Snapshot 相关分发。

## 验证边界

编译期布局检查迁移到构建时运行的原生 C++ 后，**非法几何布局的异常出现时机改变**：由旧的 constexpr 编译错误变成初始化期配置错误。捕获限制、声明 API、轻量头误用仍在编译期诊断。
这不是 ABI 承诺；用户需要将头文件和静态库配套重编译。性能与 QEMU 整机要求在真实 POE/DAG 上测量，不得仅凭合成示例断言 4 分钟问题已完全消失。

---

### 历史版本性能记录（不代表 0.36.0 实测）

## 历史性能数据

### 7.1 MMIO A/B（GCC 16.2 -O2，Apple M 系主机，与冻结构建交替测量；副本见 `benchmarks/results-0.35.1/`）

对照基准为 0.34.0 GLM 构建的 StaticWord 逐 word 后端（同一台机器、同一编译器与优化级别、
相同访问 trace、交替执行取中位数）。mixed30 = 1000 寄存器 + 30% VF identity 映射负载
（`benchmarks/mmio.cpp`），ns/access：

| 负载 | 0.35.0 | **0.35.1（静态）** | StaticWord | 0.35.1 vs StaticWord |
|---|---|---|---|---|
| rw hot read | 44.5 | **28.3** | 24.8 | +14% |
| rw hot write | 41.7 | **28.5** | 24.8 | +15% |
| rw random read | 46.7 | **33.1** | 36.2 | **快 8.5%** |
| rw random write | 45.3 | **31.8** | 34.6 | **快 8%** |
| rw mapped read | 111.9 | **77.5** | 83.0 | **快 7%** |
| rw mapped write | 104.0 | **75.5** | 80.1 | **快 6%** |
| callbacks hot read | 68.2 | 50.6 | 33.3 | +52% |
| plain（无 identity）random read | 24.4 | **12.9** | 26.8* | **快 52%** |

\* plain 行的 StaticWord 数字来自 0.34.0 动态构建（静态构建不含 plain 负载）。
对照参考：基准自带的手写 switch-case（不执行权限/回调/事务检查，语义不等价）在
4.6–26ns 区间，仅作数量级参照，不作为等价对照。

验收目标「Scalar32、Mapped 和 Random 相对旧后端回退不超过 5%」：random 与 mapped
（读/写）已**快于** StaticWord；hot 混合访问仍慢 ~14%（~3.5ns/access），来源是 30%
identity 子路径无法使用逐 word redirect 表而需两次算术解码 + resolver；有界 word memo
与 zone cache 已覆盖其余分量。callbacks 负载（on_read/on_write 全开）仍慢于 StaticWord
的预建 word 表，是当前公式后端的主要剩余差距。

### 7.2 编译与安装（GCC 16.2 -O2；`benchmarks/measure_formula.py`）

副本见 `benchmarks/results-0.35.1/`。编译期、峰值 RSS 与静态元数据相对 0.35.0 持平或
更低；动态布局安装成本从逐元素 mount 降为每声明段一个 mount（元数据不再随元素数线性
膨胀）。真实 QEMU/POE/DAG 业务源码未接入，跨平台数字不能据此宣称。
