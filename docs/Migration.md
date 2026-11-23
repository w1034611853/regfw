# 升级到 regfw 0.38.0 同步版

[返回 README](../README.md)

## 地址布局

旧 `map_alias_window*`、`map_routed_alias_window*`、`reserve_region` 和 `RegLayout`
并列表迁移到 `REG_LAYOUT`。在 View 中声明局部寄存器映射、权限、保留区和钩子，
用同一 View 的多个 `AddressSpace::map` 入口共享存储。

- 由 `REG_ENTITY` / `REG_REPEAT_ENTITY` 声明实体，REG 偏移保留在实体中。
- 地址组合使用 `reg::view(name, size).map(...)`、`.reserve(offset, size)` 与
  `reg::address_space(name)`。重复窗口使用 `map_repeat`，身份绑定使用 `.identity(...).using_resolver(...)`。
- 只声明平坦 `REG` 的设备使用默认根 View。由 `RegisterMap::create_reg` 手工声明时，
  在声明和绑定后调用 `finalize()`。
- 静态/符号布局入口见[固定布局](StaticLayout.md)，完整迁移和复用示例见[地址布局](AddressViews.md)。

## 绑定时机和生命周期

先建立 bank，注册所有 resolver、pre_access 与寄存器回调，再调用 `bank.finish()`。
需要这些绑定的设备应把构造参数上的 `.finalize()` 改成注册完毕后的 `bank.finish()`。
动态路由可以由 resolver 读取构造时捕获的可变表。使用 `RegView`、原生引用或 QEMU bridge
期间须保留 bank，对同一 map 串行执行访问、reset 和快照操作。

## 保存与恢复

流式快照保存布局哈希、实体值和活动事务。旧的完整 bank 流需要重新生成，
或在旧 SDK 中还原为 `VersionedSnapshot` 后显式加载。设备 FIFO、路由表等外部状态在父对象中保存。
升级时配套重编译库和设备，流协议和批量 IO 接入见[快照](Snapshot.md)。

完整旧接口删除清单和验证见[同步架构精简](Performance-Architecture-Sync.md)。更早的迁移记录见
[同步版发布标签中的迁移文档](https://github.com/w1034611853/regfw/tree/v0.38.0-sync/docs/Migration.md)。
