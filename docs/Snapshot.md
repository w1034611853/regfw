# 完整快照与 vmstate_fields 接入（0.24.1）

[返回 README](../README.md) · [Agent 完整使用说明](AgentGuide.md)

## 一次调用保存 bank

```cpp
auto snapshot = reg_bank.save();    // 值 + 进行中的分拍事务
// ... 运行并修改寄存器 ...
reg_bank.load(snapshot);           // 不重放命令/读写回调
```

`save()` 返回 `reg::VersionedSnapshot`，不是写入磁盘文件。它覆盖该 bank 中所有 `SnapshotKind::Migratable` 实体；共享实体只保存一次，包含所有数组元素。Transient / Derived 的值和事务状态有意排除，load 不触碰它们。

保存内容：实体完整值、所有位宽统一事务的 write_pending/write_mask、read_snapshot/has_read_snapshot/read_source_ok。按实体 stable_id 排序，数组元素按展平编号定位。运行时记录采用稀疏形式：只保存活跃状态，宽值只保存该实体实际需要的 word 数，不把每个空闲元素扩展成 2048 bit 的多个块。

读响应快照与实体值分别恢复；恢复后继续读高端不会重新执行 on_read。写入前半条命令后保存，恢复并写完后半条，on_write 只在完成端触发一次。一个元素的读写可交错，各自状态均会恢复。64 bit 也使用同一套 transaction 暂存；HIGH_TO_LOW 同样支持，不再有单独的 Split64 状态。

不保存：函数、lambda、logger、指针、锁、借用/dispatch 保护标记、设备 FIFO、Host 工作线程、resolver 捕获的路由表。目标 bank 必须先建立相同的实体布局并绑定设备回调/解析器；外部业务状态由父对象登记。实体引用/RegView 不因恢复而失效。

## 沿用已有 vmstate_fields.hpp，不用 VMStateDescription

使用之前的 `hac::snapshot::Writer/Reader` 和 `module_var_save/load` 后端。框架没有引入另一套 QEMU VMStateDescription 注册。两种接法：

### 1. bank 直接写入/读取模块快照流

```cpp
#include <regfw/vmstate.hpp>

void Device::Save(void* file) {
    reg_bank.save(file);
}
void Device::Load(void* file) {
    reg_bank.load(file);
}
```

`<regfw/vmstate.hpp>` 包含用户工程已有的 `<vmstate_fields.hpp>`；让该头文件及其 `qemu_interface.h` 在 include path 中可见。`Save(file)` / `Load(file)` 大写同名入口也受支持，用于兼容原有模块调用习惯。

SDK 的 [extras/vmstate_fields.hpp](../extras/vmstate_fields.hpp) 是此前文件的原样副本，可直接复用；它仍引用设备工程提供的 qemu_interface.h。测试用 qemu_interface.h 只位于 tests/snapshot_backend，**不得用来替换生产接口**。核心 regfw 库本身不依赖 QEMU，也不引用 module_var_*；只有使用流适配器的应用需要该后端。

流调用路径为：`reg_bank.save(file)` → 原有 Writer → 字段递归 → `module_var_save(file,...)`。加载走 Reader/module_var_load，先加载到临时快照，再验证和提交。接入你现有模块的 QEMU save/load 回调即可；框架不会自行注册模块或截获 savevm/loadvm。

### 2. 把 bank 当成父对象的普通快照字段

```cpp
#include <vmstate_fields.hpp>
#include <regfw/regfw.hpp>

struct Device {
    reg::RegisterBank<DeviceLayout> reg_bank;
    std::queue<Command> fifo;
    std::array<std::uint32_t, 8> vf_route;
    // 构造函数先初始化 bank 并绑定回调，然后 finish。
    VMSTATE_FIELDS(reg_bank, fifo, vf_route);
};
```

bank 已有 `VmstateFields(Archive&)`，会自动保存完整快照，不能登记 bank 私有实现、map 指针或 bundle view。此方式只需原有字段框架和 regfw 的公共头；不要求包含可选的 regfw/vmstate.hpp。父对象原来已有 Save/Load 时，用 `VMSTATE_FIELDS_ONLY(...)` 或直接在原有入口中调用 bank.save/load。

若不同模块相互依赖，按既有字段框架在最外层 `AfterSnapshotLoad()` 中重连。不要在 load 时执行寄存器 on_write 来“重建”设备，这会重发命令。框架不为整个父对象提供事务式恢复；bank 自身的恢复是先验证后提交。

### 3. 自定义容量限制

```cpp
hac::snapshot::Limits limits;
limits.max_container_elements = 4'000'000;
limits.max_total_bytes = 512ull * 1024 * 1024;
hac::snapshot::Writer writer(file, limits);
reg_bank.save(writer);
// 读取时用匹配的容量限制构造 Reader，再 reg_bank.load(reader)。
```

也可 `hac::snapshot::Save(file, device, limits)` / `Load(...)`。默认沿用原有框架的每容器 1,048,576 元素、256 MiB 字节预算、128 层递归限制。高容量设备需按实际大小配置，不能靠关闭接收端验证解决容量问题。

原有 vmstate_fields 的数值流使用 Host 原生表示；此适配不声称跨端序、跨 size_t ABI 兼容。module_var_load 的 void 接口没有短读长度返回：底层必须完整读取或报告失败，不能在短读时静默返回。C++ 异常需在既有模块的 C/C++ 回调边界转换为 QEMU 错误，不能越过不支持异常的边界。

## 校验与错误恢复

格式 v2 检查：magic/版本、schema、值 word 数、每实体 runtime 数、实际位宽、数组维度/stride、transaction 读写策略、唯一且递增的元素编号、宽 payload 长度与高位、写掩码字节完整性、状态与策略一致性。先为所有实体准备值和事务状态，全部成功后一次提交；错误时 bank 的实体值和活跃事务保持不变，不触发回调。

源快照 schema 必须与目标本地 schema 匹配；不能把收到的 data 套上本地 schema 以跳过源格式验证。自定义字段含义/behavior 变化仍需设备层版本管理；schema 不等于任意设备语义兼容性证明。0.20.x 的 v1 完整对象不包含进行中事务，新接口明确拒绝，不会假装旧格式能恢复丢失的半条命令。

选择稳定 ID 时，每个参与快照的实体必须唯一；重复 VF 不要声明完全相同的显式 ID/order。默认包含实例号的实体名适合多数情况。

## 兼容的旧接口

- `map.dump_state_versioned()` / `restore_state_versioned()` 已升级为完整格式 v2，与 bank.save/load 等价。
- `map.dump_state(span)` / `restore_state(span)` 保留为**值数组接口**，没有位置存储分拍状态；恢复时仍清除该实体的分拍状态。不能用这对旧接口实现任意时刻的完整命令快照。
- map/bank reset 仍清除分拍状态；reset 与 load 的语义不相同。

## 同步与测试范围

保存/加载时必须暂停或同步所有会访问 bank 的 Host 线程，并让 FIFO 等业务状态处于同一一致点。允许 guest 停在两次 MMIO 拍之间；不允许框架正在执行某次 MMIO 回调时递归保存/加载，同 map 的 dispatch 检查会拒绝。新增 API 不提供自动锁，也不保证两 CPU 交错的同方向事务各自隔离。

`tests/snapshot.cpp` 使用**原样 vmstate_fields.hpp**，以 module_var_save/load 内存流后端验证：同/新实例恢复，512 bit 两元素同时半读/半写、跨共享入口继续，64 bit 高/低端完成及独立读写模式，HIGH_TO_LOW，失败读源标记，96/2048 bit 值，schema/损坏流拒绝、原子性、父对象 VMSTATE_FIELDS(bank,fifo,route) 递归。该测试验证模块字段协议，不代替真实 QEMU 设备的 savevm/loadvm 集成测试。
