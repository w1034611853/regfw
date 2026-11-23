# regfw 0.38.0 同步版维护指南

当前版本面向固定生命周期、单线程 QEMU SysBus 设备。完整的 API、默认值和设备示例见
[使用说明](docs/AgentGuide.md)，实现位置见[源码架构](docs/Architecture.zh-CN.md)。

## 使用约定

- 依次建立 bank、绑定 resolver、pre_access 和寄存器回调、调用 `finish()`，再提供 MMIO。
  `finish()` 冻结布局和绑定并检查所需依赖。捕获的路由表、寄存器值和 logger 等级可在串行访问中变化。
- `RegView`、原生引用和 QEMU bridge 借用 bank。使用期间保持 bank 存活。同一 map 的 MMIO、
  reset 和 save/load 必须串行，回调中禁止递归调用同一 map 的 MMIO 或快照入口。
- 用 `REG_ENTITY` / `REG_REPEAT_ENTITY` 创建存储，用 `REG_LAYOUT` 与
  `AddressSpace` 安装地址入口。View 的映射偏移是局部地址。多个入口可以共享同一存储和事务状态。
- 仅声明寄存器的 TU 可包含 `<regfw/bundle.hpp>`。定义布局的 TU 使用 `<regfw/regfw.hpp>`。
  头文件、静态库和设备需要配套重编译。见[布局编译](docs/LayoutCompilation.md)。
- `bank.save()` 返回包含值和活动事务的内存 V2 快照。模块流使用校验布局的紧凑格式。
  迁移旧流时按[快照文档](docs/Snapshot.md)转换。

## 实现与验证

- 平坦和显式布局都通过 `FormulaDecoder` 分发。共享字表只记录布局定位，resolver 结果和
  日志等级逐次求值；回调适配器按需装配 `RegAccessInfo`。
- `include/regfw/` 是安装入口，`src/internal/` 为私有实现。改动 MMIO 时检查权限、回调顺序、
  部分访问、宽寄存器事务和错误返回。
- 使用 CMake Release 构建并运行全部 CTest。修改 C++ 时运行 `bash scripts/format_cpp.sh --check`；
  修改示例时用 `scripts/update_docs.py --build-dir BUILD` 更新生成区。见[构建和发布说明](docs/Release.md)。
- 性能复测使用[当前基准入口](benchmarks/README.md)，保存版本和配对数据。
