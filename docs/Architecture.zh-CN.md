# regfw 0.37.0：源码架构与阅读顺序

这份文档是面向维护者的**代码导航**，不是新 API。0.37.0 只整理实现职责和 C++ 排版：设备侧原有 `#include <regfw/...>`、REG DSL、MMIO 接口、钩子、权限、事务、快照格式全部保留。

## 从哪里开始读

| 想了解什么 | 从这里开始 | 主要职责 |
|---|---|---|
| 寄存器声明与用户 DSL | `include/regfw/dsl.hpp`、`include/regfw/detail/declarations_base.hpp` | `REG`、Entity、Layout 声明与元数据 |
| REG_LAYOUT / View 组合 | `include/regfw/static_layout.hpp`、`src/layout/address_view.cpp` | Layout Factory、View、Repeat、优先级与地址上下文 |
| 公式与共享地址表 | `src/layout/formula.cpp`、`src/internal/formula_decoder.hpp` | 共享 EntityLayout Hash、局部地址到多维索引/字节偏移的定位 |
| 访问入口 / 控制流 | `src/runtime/access.cpp` | `try_mmio_{read,write}_sized`、权限、映射、事务和快速执行 |
| 实体生命周期 | `src/runtime/lifecycle.cpp`、`src/runtime/bindings.cpp` | Entity Storage 初始化、注册、finalize、回调槽绑定 |
| 寄存器行为 | `src/runtime/behaviors.cpp`、`include/regfw/behaviors.hpp` | 字段读写规则、掩码等 |
| Callback / Handle | `src/runtime/callbacks.cpp`、`src/runtime/handles.cpp` | 回调持有与 Handle 生命周期、检查 |
| Snapshot / Reset | `src/snapshot/snapshot.cpp`、`src/snapshot/state.cpp`、`include/regfw/vmstate.hpp` | 事务快照、恢复、状态枚举与 reset |
| 日志与诊断 | `src/diagnostics/`、`include/regfw/diagnostics.hpp` | hlog、布局描述、诊断文本、错误链 |
| 内部结构 | `src/internal/entity_state.hpp`、`access_context.hpp`、`formula_decoder.hpp` | 实例所有权、访问上下文、共享解码器 |

公开头文件仍保持在 `include/regfw/` 的旧路径，方便 QEMU 设备代码直接升级；`src/internal/` 不安装，也不属于公共 ABI。

## 寄存器访问流程

```text
QEMU MMIO(address, size, value)
       |
       v
runtime/access.cpp  [read/write 总入口，DispatchGuard]
       |
       v
View range / priority  [View 可能交叠；空洞透明，Reserve 阻断]
       |
       v
EntityLayout Hash      [共享几何，局部地址 -> PreparedAccessInfo]
       |
       v
AccessContext          [绑定此 View 的真实 Entity/Storage、Scope、Identity]
       |
       v
Access Engine          [PreAccess / resolver / access group / 权限 / transaction]
       |
       v
存储访问 + Callback + Log
```

* 一种 Entity 的不可变局部寄存器几何可被多个 View/VF 复用。不同 Entity 实例的存储和动态 Callback **不能共用**。
* 多维坐标沿用 `RegElementIndex`，最高 `MAX_REG_DIMS`，不是固定 `index0/index1`。
* 访问期需要用动态 Resolver 决定的映射依旧实时计算；不能把某次结果冻结到共享 Hash。
* 访问不应重新运行整个 REG_LAYOUT 工厂；它只在第一次创建类型布局时按普通 C++ 流程生成一次。

## 初始化与运行时

1. DSL 描述 Entity、REG、View、Repeat、Identity 的静态规则。
2. 原生 C++ 构建小型布局描述，并构建按局部有效地址索引的共享 Hash；对大型宽寄存器需要按实际可访问字节构建记录。
3. Bank 初始化时绑定存储、Scope、动态 Resolver/PreAccess/Callback 引用并执行合法性检查。
4. MMIO 访问只做地址选择和访问执行；Storage、Callback、Transaction、Snapshot 的既有规则保持不变。

0.36.0 已把大型 `consteval` 计划生成转移到运行时。0.37.0 **不再改变建表算法**，也不提供新缓存层。

## Snapshot 与运行时状态

`src/snapshot/snapshot.cpp` 负责完整快照序列化 / 恢复；`src/snapshot/state.cpp` 负责 reset、原始值导入导出及需要保存的实体顺序。Snapshot 格式仍是 V2；Reset、事务中间态与回调有效性必须有回归测试保护。

`<regfw/vmstate.hpp>` 仍是可选 QEMU 适配入口，不随内部目录移动。

## 修改时的守则

- C++ 排版统一使用根目录 `.clang-format`，避免压缩为一行的 `if/for` 或多个语句。
- 修改 Layout 解析，不要顺手修改 Snapshot 或事务；修改访问控制流，要跑全套 CTest。
- 不允许从共享布局缓存动态 Storage、Resolver 结果或 Callback 状态。
- 外部的 `include/regfw/` 包含路径不得改动；内部新增逻辑优先放进对应 `src/` 子目录。
- 大型 REG 数组初始化与真实 QEMU machine init 需要在业务设备环境测量；合成示例只能证明 SDK 回归。

## 复验命令

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-Wall -Wextra -Werror"
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
cmake --install build --prefix /tmp/regfw-install
```

完整的命令与本次结果以根目录 `VALIDATION.md` 和交付报告为准。
