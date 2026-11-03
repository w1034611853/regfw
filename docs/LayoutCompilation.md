# regfw 0.36.0：布局声明与独立 TU

寄存器声明放头文件，布局实现放单个 `.cpp`，保持原有 `REG_LAYOUT_DECL`/`REG_LAYOUT_IMPL` API；无需增加初始化调用或重新组织设备模型。

```cpp
// poe_layout.hpp
#include <regfw/bundle.hpp>
struct PoeLayout : reg::Bundle {
  REG_BUNDLE_DECL(PoeLayout);
  REG_ENTITY(regs, PoeRegs);
  REG_LAYOUT_DECL();
};
```

```cpp
// poe_layout.cpp
#include "poe_layout.hpp"
#include <regfw/regfw.hpp>
REG_BUNDLE_IMPL(PoeLayout);
REG_LAYOUT_IMPL(PoeLayout, [](auto layout) {
  namespace reg = ::reg::compiled;
  return reg::address_space("POE")
    .map(reg::view("REGS", 0x10000).map(layout.regs), 0xC4000000ULL);
});
```

普通消费者只 include `<regfw/bundle.hpp>` 可以减轻模板头文件负担；布局实现 TU include 完整 `<regfw/regfw.hpp>`。原有 `REG_LAYOUT` 和 `REG_STATIC_LAYOUT` 写法仍被支持。

**0.36.0 不再对整个布局执行 consteval。** 编译期只做 REG 声明的基础类型和尺寸计算。首次执行布局 factory 时，普通 C++ 单遍生成 RuntimeFormulaPlan（同一工厂后续实例共享）；每个 RegisterMap 初始化时绑定实例并构建共享 EntityLayout Hash。内存与时间主要随有效 REG lane、挂载数量和实例数增长，而非地址空间的总跨度。合法性检查与冲突诊断在布局安装阶段执行。

留意：如果真实布局具有大量有效宽寄存器，每个 4 字节有效 lane 均有 Hash 键，启动时间/堆内存仍需要测量，不能保证 10 个模块加起来必然小于 2 秒。

跨 TU 可运行示例：`tests/layout_separate.cpp`、`tests/layout_separate_impl.cpp` 和 `tests/layout_separate_other.cpp`；轻量头和 PCH 的端到端测试见 `tests/bundle_light*`。
