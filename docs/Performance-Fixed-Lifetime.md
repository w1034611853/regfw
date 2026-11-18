# 固定生命周期与紧凑快照（2026-11-18）

更新分支：`codex/regfw-037-fixed-lifetime-20261118`。

此分支基于无异步队列的 0.37 性能分支 `c0a6bab`，不包含 main 的异步修改。前一版的直接 Bundle、按需 info 和映射查找优化继续保留，本次进一步移除设备不需要的生命周期保护，并改正 bank 流序列化的体积问题。

## 固定 View 绑定与双参数 index resolver

本节以上一版同步提交 `b815984` 为基线。后文“MMIO 与原生访问测量”记录的是 `c0a6bab` 到 `b815984` 的历史结果。

`finish()` 为每个挂载分别选择读、写的执行路径。在规则的 32 位 VF 窗口命中地址后，执行器借用已绑定的实体、权限和坐标。identity resolver 仍逐次求值，返回的 VF 编号直接选中该实例的绑定，省掉此前对来源、目标两份 `AliasEntry` 的填写。只有各个可能目标的布局与本方向的访问语义均适合时才启用这条路径。重叠、保留区或特殊行为可以让该方向走通用解码，identity resolver 每次访问仍只执行一次。

命名 index resolver 可以直接接收 `(std::uint32_t vf, reg::RegData logical)`。规则的单下标映射直接调用这个签名。寄存器回调、pre_access 和日志按各自需求取得上下文。通用规则与部分宽度访问也支持双参数签名。错误诊断会记录 converter 调用前的逻辑槽旧值，因此实现保留这一次取值；转换结束后读写最终的物理槽。完整签名与新签名的注册方法见[绑定设备逻辑](Device.md)。

配对测试使用 GCC 14.2，库和基准均为 `-O3 -DNDEBUG -fno-strict-aliasing`，未启用 LTO。用相同的 `benchmarks/mmio.cpp` 配合各版本的头文件和静态库，设置 `-DREGFW_STATIC_BENCH=1`，按组合设置 `REGFW_BENCH_IDENTITY`、`REGFW_BENCH_INDEX` 和 `REGFW_BENCH_LIGHT_INDEX`。例如完整映射读使用

```sh
REGFW_BENCH_THREADS=1 REGFW_BENCH_RANGES=10 REGFW_BENCH_PRE_ACCESS=none \
  taskset -c 1 ./mapped-bench 5 4 1 0 mapped 0
```

最后一个参数改为 1 测写，第四个参数改为 1 启用寄存器回调。每个场景取 5 个交替进程的中位数，进程内四个引擎轮换顺序。计时前逐项比对返回值、回调效果和最终存储。手写对照使用相同的 VF 和 index 路由表。

读 / 写中位数，单位 ns/次。手写列使用对应新版进程的 `range_switch`，开启 index 时取双参数进程。

| 8 VF 映射地址流 | `b815984` | 新版原签名 | 新版双参数 | 手写 |
|---|---:|---:|---:|---:|
| 无转换 | 42.06 / 44.06 | 40.34 / 42.00 | — | 39.04 / 41.42 |
| 仅 identity | 69.01 / 66.46 | 55.30 / 57.34 | — | 39.82 / 40.92 |
| 仅 index | 91.07 / 84.25 | 79.78 / 81.18 | 68.16 / 65.42 | 40.04 / 41.60 |
| identity + index | 88.78 / 88.65 | 79.36 / 76.94 | 77.87 / 75.13 | 41.19 / 42.68 |
| 双转换 + 寄存器回调 | 94.90 / 91.90 | 82.53 / 80.04 | 82.11 / 80.20 | 41.45 / 42.36 |
| 双转换 + 全局 pre_access | 93.89 / 91.96 | 83.90 / 83.70 | 83.03 / 82.07 | 41.66 / 43.24 |
| 双转换 + 全局和 View pre_access + 寄存器回调 | 100.52 / 97.80 | 88.98 / 87.18 | 87.56 / 89.42 | 44.25 / 44.89 |

双转换、无钩子时，保留原签名的读写分别比基线缩短 10.6% 和 13.2%，双参数读写分别缩短 12.3% 和 15.3%。只有 index 时，双参数读写分别比新版完整签名再缩短 14.6% 和 19.4%。双转换并开启寄存器回调、关闭 pre_access 时，两种签名的读写差异均小于 0.5 ns。

另测了 256 元素的直接 Bundle 和 AddressSpace，各含无回调、轻量回调、消费部分 info、复制全部 info 四种读写。AddressSpace 无回调读 / 写从 17.39 / 18.92 ns 降到 14.56 / 13.87 ns。直接 Bundle 的轻量回调写在首轮出现 12.4% 回退，改用 11 组交替进程、每项 500 万次复测为 15.44 → 15.43 ns。复测中完整复制 info 的直接 Bundle 写为 36.94 → 37.70 ns（增加 2.1%）。这些结果来自云端微基准，私有 QEMU/TM 套件仍需在部署环境计时。

测试还覆盖同一几何布局内 VF0 为普通读写、VF1 为写一清零的寄存器。此前只依据 VF0 选择执行路径会写错 VF1 的值。现在按方向检查所有可能的目标，保留写一清零语义。Release 和 ASan/UBSan（启用泄漏检测）均通过 35 个 CTest，安装包的独立三翻译单元消费者通过。

用 `benchmarks/compile.cpp`、GCC 14.2、`-O2 -DELEMENTS=16384` 编译 9 个实例、每个实例 10 个数组的布局 TU，共 1,474,560 个逻辑元素。本次单进程用时 6.19 秒，峰值 RSS 为 337,552 KiB。View 到实例的绑定在初始化阶段计算。

## 使用契约与改动

设备单线程运行，bank 在所有 view、原生引用和 bridge 使用期间保持存活。先建立布局、注册全部 resolver/pre_access/读写完成回调，再调用现有 `finish()`。finish 后绑定冻结，继续调用 set/clear/on_* 会得到 Configuration；已有 defer_* 入口也不能让未绑定依赖通过 finish。已按此顺序初始化的设备不需要改调用点；构造时 `.finalize()` 后才注册回调的代码需要把 finish 移到注册之后。

- RegView 只有实体指针、选择范围和已有的原生值指针；删除生命周期控制块、引用增减、expired 检查和 entity borrow 检查。`is_alive()` 仅表示已绑定，不检测 bank 销毁。
- 所有 identity/index resolver 及普通、宽寄存器回调直接借用已绑定函数。执行时不复制 shared_ptr，不再维护 pre_access 的退休函数队列。
- 删除 DispatchGuard、PreAccessGuard、NodeBorrow 及每拍状态写入。同 map 的递归 MMIO/save/load 仍不在使用契约内，不再运行时检测。
- 保留地址/宽度/权限/数组坐标检查、behavior、掩码、分拍事务、异常分类及回调顺序。resolver 捕获的路由表可变，结果逐次计算；logger 开关和等级可变。
- 完整 RegAccessInfo 字段、aggregate 属性及复制/移动语义保留。前一版已经让无消费者的 scalar 路径跳过 info；本版让内部临时 info 的析构在 GCC/Clang 下内联，避免编译器把析构留成独立调用后阻止无用字段消除。只展开内部清理，不强制内联设备回调。需要完整复制的业务仍支付该成本。

函数对象在配置时仍有所有者，布局构建的冷路径仍使用 weak_ptr。它们不在每拍访问中增减引用；没有为它们再引入一个可切换的动态兼容模式。布局后端仍按类型运行期构建共享表，没有新增逐 word consteval 展开。

对本次 GCC 14.2 Release 对象做反汇编：负责 MMIO 的 access.cpp 中，带 lock 前缀的原子指令位置从 31 处降到 0 处。这是静态指令位置计数，不是每次访问会执行 31 次，也不等于全库不存在配置期引用计数。

## MMIO 与原生访问测量

Xeon Platinum 8573C、GCC 14.2，库和消费 TU 均为 -O3 -DNDEBUG -fno-strict-aliasing，无 LTO，CPU 0。编译及测试结束后交替运行旧/新二进制。基线是已包含前一轮 MMIO 优化的 c0a6bab，不是用户私有 QEMU 的旧实现。

scalar 每项每进程 300 万次，五组进程取中位数；主表为 256 元素、已 finish、无 logger，单位 ns/访问。light 只计数且不接收 info；info 读取 address/index/stored_before；copy 完整复制 info。

| 入口 | 回调 | 旧读 | 新读 | 旧写 | 新写 |
|---|---|---:|---:|---:|---:|
| 直接 Bundle | none | 10.64 | 9.77 | 11.55 | 11.41 |
| 直接 Bundle | light | 14.16 | 13.81 | 15.30 | 14.39 |
| 直接 Bundle | info | 35.12 | 15.05 | 17.97 | 15.89 |
| 直接 Bundle | copy | 37.02 | 33.61 | 35.08 | 35.96 |
| AddressSpace | none | 17.46 | 15.74 | 17.57 | 16.49 |
| AddressSpace | light | 20.28 | 18.04 | 19.00 | 18.45 |
| AddressSpace | info | 22.51 | 20.91 | 21.33 | 19.56 |
| AddressSpace | copy | 43.09 | 39.73 | 41.40 | 40.67 |

完整矩阵另含 4/4096 元素、已绑定 ERROR logger，以及未 finish 的 generic 对照，合计 56 项；新/旧结果、回调次数、最终存储校验一致。生产调用仍按初始化→finish→MMIO 的契约使用。最大候选增幅为 3.26%；不把几个百分点的变动单独宣称为提升。完整复制 info 仍比轻量回调贵，框架没有通过删除字段或改变复制语义获得表中的收益。

原生访问每项 2000 万次，五组配对；raw 和 view 使用同一内存屏障防止整个循环被优化掉，并验证校验和相同：

| 路径 | 旧读 ns | 新读 ns | 旧写 ns | 新写 ns |
|---|---:|---:|---:|---:|
| .data()[i] | 0.375 | 0.364 | 0.595 | 0.620 |
| .at(i).bits | 1.275 | 0.638 | 1.577 | 0.784 |
| .at(i).ref().bits | 2.316 | 0.628 | 2.874 | 0.781 |

未消除内存访问的裸循环不能直接与一次完整 MMIO 流程作等工作量比较，因此不使用用户报告中的“500x”来计算本版收益。用户实测的 160～228 ns 需要在其同一 QEMU 构建上复测；仅给 benchmark 加 -O3 不会优化已编译的 libregfw.a。

### identity/index 完整映射路径

使用现有 mmio.cpp 的 identity + index、mapped trace、10 组手写范围分发，含无回调/有回调和 none/global/both pre_access。每版本每项 5 个进程，进程内每引擎 8 轮（首轮不计）、每轮 5 passes；访问结果、回调效果与最终存储先后对拍。宿主线程在线程状态检测前创建并退出，正式 MMIO 串行运行，确保旧版 shared_ptr 走与 QEMU 类似的 libc 多线程引用计数分支。

| pre_access | 寄存器回调 | 旧读 | 新读 | 手写读 | 旧写 | 新写 | 手写写 |
|---|---|---:|---:|---:|---:|---:|---:|
| none | 无 | 100.37 | 88.43 | 39.31 | 99.73 | 86.36 | 40.89 |
| none | 有 | 104.08 | 93.58 | 40.55 | 100.62 | 89.13 | 41.53 |
| global | 无 | 106.39 | 93.32 | 39.78 | 103.40 | 92.88 | 42.38 |
| global | 有 | 107.62 | 97.26 | 41.28 | 105.69 | 92.30 | 42.63 |
| both | 无 | 107.73 | 94.71 | 42.94 | 105.33 | 96.03 | 42.67 |
| both | 有 | 114.13 | 96.59 | 43.05 | 107.72 | 96.73 | 43.62 |

该组实测相对 c0a6bab 降低 8.82%～15.37%；仍为同进程手写 range_switch 的 **2.11～2.36 倍**。生命周期保护精简没有消除全部复杂映射开销，本报告不把结果表述为已与手写等速，也不推算私有 TM 整机套件耗时。

环境波动检查：用最终 scalar 二进制同时充当基线/候选，16 项各三组的 A/A 中位数差为 -2.97%～+12.17%。因此表中几个百分点的变化只作为回归范围记录，不能单独作为稳定收益结论。

## 快照规模对照

使用同一个 `benchmarks/snapshot_stream.cpp`，分别链接 c0a6bab 和当前库。通过真实 bank `save(void*)/load(void*)`、hac Writer/Reader、module_var_save/load 内存后端往返，reset 后恢复并逐值核对；两端使用同一附带 archive 实现。IO 次数为 module_var_* 调用数，不是寄存器数或 archive 节点数。

| 9 实例负载 | 实际值字节数 | 原 bank 流 | 紧凑 bank 流 | 原读/写调用数（各） | 新读/写调用数（各） |
|---|---:|---:|---:|---:|---:|
| 每实例 2048 个独立 u32，独立稳定名称 | 73,728 | 2,441,510 | 73,784 | 1,353,986 | 2 |
| 每实例 65536 元素 u32 数组 | 2,359,296 | 4,719,509 | 2,359,352 | 590,237 | 2 |

第一组缩小约 33.1 倍，第二组约 2 倍。原格式中每个独立 REG 的 stable_id/布局描述以及字段递归放大开销；数组只有一份 schema，但原 data 将每个 u32 扩为 uint64 word。紧凑格式同时消除这两种浪费。

无活动事务时，流大小严格为 **56 + 实际值字节数 + 最多 7 字节补零**。有事务时，只附加活动元素的定位和实际位宽 payload。别名不重复保存实体；只读实体仍保存，避免丢失设备内部更新的状态。现有总字节预算仍是 256 MiB，未提高上限。

这不是私有 POE 的 735 MB 流复测：云端没有 POE 源码和原测试程序，不能报出该设备修复后的确切体积。此测试证明相同 bank 序列化入口不再随 schema 字符串和逐字段节点膨胀。更新后请用原 poe_snapshot_save/load 路径复测；不要改成 `ar(bank.save())`，后者仍显式请求完整 VersionedSnapshot。

同机内存流保存/加载计时（五组交替进程中位数，单位 ms；不含布局初始化和 reset）：

| 负载 | 旧保存 | 新保存 | 旧加载 | 新加载 |
|---|---:|---:|---:|---:|
| 独立 u32 | 33.71 | 8.48 | 29.60 | 12.42 |
| u32 数组 | 8.98 | 2.45 | 13.56 | 17.16 |

独立寄存器的保存/加载都明显缩短。大数组保存更快，但本轮加载由 13.56 ms 增至 17.16 ms：紧凑格式仍需解包为现有恢复引擎的 word 暂存，再验证并提交。已把解包改为直接 word 组合，并把元素计数移出循环；这个数组恢复成本仍是当前实现的限制。体积减少与加载时延分别报告。

## 格式与集成

`ar(bank)`、VMSTATE_FIELDS(bank,...)、bank.save/load(archive)、bank.save/load(void*) 统一使用 compact v1。8 字节长度前缀和 48 字节头之后按实际位宽写值，头部携带现有 schema 算法的 layout hash；只存活动事务，不输出 schema 明细。恢复验证 hash、长度、补零和事务状态后准备全部实体，成功后统一提交，不触发业务回调。详细格式见 [Snapshot](Snapshot.md)。

无参 save() 的 VersionedSnapshot v2 对象接口保留。旧版 bank 文件流不自动兼容新格式，升级需重新生成快照，或用旧代码先解析为 VersionedSnapshot，再显式 load(snapshot) 并重存。源、目标的业务字段语义仍由设备版本管理。

建议同步更新 extras/vmstate_fields.hpp 的公开 Bytes 方法，并保留设备自己的 qemu_interface.h。Bytes 将总 payload 一次交给底层，仍计入父 archive 字节预算；旧 archive 没有 Bytes 也能使用同一紧凑格式，只是逐 word 调用后端。头文件和 libregfw 必须一起重编译，确认库本身也启用了优化。

## 复现

先分别构建基线和候选库，再分别用对应版本的 include/libregfw.a 编译同一基准源码：

```sh
g++ -std=c++20 -O3 -DNDEBUG -fno-strict-aliasing \
  -I/path/to/sdk/include -I/path/to/sdk/third_party/hlog/include \
  -I/path/to/candidate/extras -I/path/to/candidate/tests/snapshot_backend \
  /path/to/candidate/benchmarks/snapshot_stream.cpp /path/to/build/libregfw.a \
  -L/path/to/sdk/third_party/hlog/lib -Wl,-rpath,/path/to/sdk/third_party/hlog/lib \
  -lhlog -pthread -o snapshot-bench
./snapshot-bench scalar
./snapshot-bench array
```

这里的 tests/snapshot_backend 仅用于基准，不能替换生产设备的 QEMU 接口。基准输出体积、IO 次数和保存/加载耗时；体积表不依赖计时。MMIO 和原生句柄的配对脚本及参数沿用 [MMIO Scalar](Performance-MMIO-Scalar.md) 与 [Handles](Performance-Handles.md)，必须在编译结束后固定 CPU 测量。

## 验证范围

最终代码：Release 35/35、ASan/UBSan/LSan 35/35，安装后的独立消费者运行通过。

严格 Release 构建使用 GCC 14.2、C++20、-O3 -DNDEBUG -Wall -Wextra -Werror，无 LTO；ASan/UBSan 使用 -O1 -g、frame pointer、非 PIE，并开启泄漏检查。测试覆盖直接/映射/generic 路径差分、别名/权限/宽度/behavior、identity/index 输出动态变化、固定绑定所有权、异常、完整 info 复制和全部快照事务。

快照回归新增：批量/逐 word archive 字节一致，9 实例 u8/u32/96-bit 与只读值，别名不重复，长度与补零错误、错误 hash、越界/重复事务、超长前缀和短读拒绝，验证失败时值与事务不变，以及旧 rich 流显式读取/转换。固定绑定测试用改变捕获数据来验证动态路由，同时验证 finish 后 set/clear/首次回调注册拒绝；不再测试设备契约禁止的悬空句柄访问或同 map 递归。

安装后另建三个翻译单元的消费者，使用 find_package(regfw 0.37.0 EXACT)、轻量 bundle 头、独立布局实现与回调完成链接运行。格式、文档生成/本地链接以及文件校验和另行核对。上述内存流和独立 MMIO 测试不替代私有 POE/TM 整机套件。
