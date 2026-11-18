# 0.37 MMIO 回调与按需上下文

日期：2026-11-18。基线是 `e885a434ff21ab25a297f08d18d5094a35b4de51`
（已包含单线程句柄和此前 MMIO 优化），候选分支为
`codex/regfw-037-mmio-callbacks-20261118`。继续基于同步 0.37，不合并 main 的异步队列。

## 找到的固定开销

`RegAccessInfo` 在本次 GCC/Linux x86_64 构建中为 336 字节，但大小本身不等于每拍的实际成本。
问题是构造位置及路径合流影响编译器删除无用写入：旧版普通 scalar 回调即使不接收任何参数，
生成的适配器仍包含 info 初始化、填充和销毁。基准中的无参写事件适配器机器码由 1200 字节降到
8 字节；这是代码大小证据，不能直接换算成时间加速比。

另外发现两个入口的区别：

- 显式 AddressSpace/View 在 `finish()` 后已有 scalar 执行路径；挂回调并不必然退到完整通用引擎。
  此前仍为回调装配 `AliasEntry`，再从中生成调用帧和 info。
- 直接以寄存器 Bundle 作为根、没有 AddressSpace 的入口，此前即使 `finish()` 也走完整解析。
  它已有地址到实体的索引，可以复用 scalar 执行器。

因此，只检查“无回调直接 memcpy”那段条件不足以判断所有回调的执行路径；表大小不影响时延，
也只能说明这些样本没有显著随规模增长，不能证明查找本身完全免费。

## 实现

1. View 挂载时准备不可变的地址、权限、route、instance 和名称信息。普通 scalar 分发借用这些
   元数据和已准备好的元素坐标，每拍传递地址、选中索引、值，不再先填写整个 `AliasEntry`。
2. 无参读/写/读完成回调直接调用用户函数，明确跳过 info 构造。接收 info 的回调把构造留在
   与用户函数一起编译的适配器中，并分开 scalar 与 mapped 对象的生存期，使编译器有机会
   删除未使用字段。**这不保证任意回调都能删除全部无用字段**；函数指针、跨 TU 调用或完整复制
   仍可能需要完整装配。
3. index resolver 的帧入口只来自 mapped 执行器，直接使用 mapped info 构造入口。把两种构造
   混在这里曾引起 11%～20% 的回归，复测发现后已修正；resolver 生命周期和换绑保护保留。
4. 无 AddressSpace 的直接映射在 `finish()` 后允许走同一 scalar 执行器。仅覆盖没有 alias
   window、identity、入口策略、reserve 或 instance window 的布局；选中 REG 还必须支持
   原生 32 位 Independent 方向、没有 index 派生、访问对齐且宽度为 4。其他情况继续原路径。

没有删除 `RegAccessInfo` 字段，没有修改前端 API、完整复制、回调顺序、旧值/候选值语义或日志等级
动态生效规则。`include/regfw/types.hpp` 未修改。回调内改存储、异常、晚绑定、动态 pre_access、
完整 info 复制及不支持 scalar 的寄存器继续由功能测试覆盖。没有引入逐地址模板实例化或大型
`consteval` 布局计算；不可变元数据在已有的运行期安装阶段准备。

identity 转换后的复用查找已在基线中实现，本轮没有再次把它计作收益。
[旧分发采样](Performance-Dispatch.md) 的百分比属于旧实现，不能直接分摊当前版本的一次访问。

## 测量方法

GCC 14.2、Linux x86_64、Xeon Platinum 8573C；库及消费者均为
`-O3 -DNDEBUG -fno-strict-aliasing`，没有 LTO。固定 CPU 0，线程 CPU 时钟；每项每版每轮
300 万次，5 轮交替进程，随机场景顺序，取中位数。性能计时期间不编译、不跑功能测试。
各版本使用同一份 `benchmarks/mmio_scalar.cpp`，分别配套头文件与库。

56 个场景覆盖直接根 Bundle / AddressSpace、4/256/4096 元素、读写、无回调/无参回调/
读取部分 info/复制完整 info、未绑定 logger/绑定 ERROR logger、finish 前后。
每项检查访问结果、最终存储校验和、回调次数及所用 info 字段的校验和。
绑定 ERROR logger 是访问日志关闭但 logger 指针存在的对照，并非开启逐拍输出。

`light` 回调累加计数；`info` 读取 address、index.linear()、stored_before；`copy` 将完整 info
保存到外部对象。循环下标为 `(i * 13) & (N - 1)`，没有测 QEMU 总线调度本身。

### 256 元素、finish 后、未绑定 logger

单位：ns/次。这里的收益包括分发记录和 info 两部分。

| 入口 | 回调 | 操作 | 基线 | 优化后 |
|---|---|---|---:|---:|
| 直接 Bundle | 无 | 读 | 95.32 | 10.97 |
| 直接 Bundle | 无 | 写 | 97.20 | 11.64 |
| 直接 Bundle | 无参 | 读 | 101.02 | 14.28 |
| 直接 Bundle | 无参 | 写 | 98.58 | 14.82 |
| 直接 Bundle | 部分 info | 读 | 99.94 | 31.86 |
| 直接 Bundle | 部分 info | 写 | 104.43 | 16.63 |
| 直接 Bundle | 完整复制 | 读 | 107.59 | 36.40 |
| 直接 Bundle | 完整复制 | 写 | 111.02 | 35.21 |
| AddressSpace | 无 | 读 | 16.60 | 16.53 |
| AddressSpace | 无 | 写 | 17.04 | 16.87 |
| AddressSpace | 无参 | 读 | 51.35 | 19.84 |
| AddressSpace | 无参 | 写 | 41.68 | 18.77 |
| AddressSpace | 部分 info | 读 | 50.42 | 22.45 |
| AddressSpace | 部分 info | 写 | 41.03 | 20.56 |
| AddressSpace | 完整复制 | 读 | 50.64 | 43.46 |
| AddressSpace | 完整复制 | 写 | 48.69 | 40.33 |

### 单独分离 info 构造的收益

另一个控制版本已经包含紧凑分发和直接 Bundle 路径，但保留原 info 构造方式。同轮配对的
AddressSpace 结果如下；不要把上一表的全部收益都归因于 info。

| 回调 | 操作 | 仅分发优化 | 再优化 info |
|---|---|---:|---:|
| 无参 | 读 | 42.65 | 19.84 |
| 无参 | 写 | 31.95 | 18.77 |
| 部分 info | 读 | 43.66 | 22.45 |
| 部分 info | 写 | 32.21 | 20.56 |
| 完整复制 | 读 | 42.75 | 43.46 |
| 完整复制 | 写 | 42.19 | 40.33 |

完整复制的实际工作仍在，不能通过减少默认填充把这部分成本消掉。直接 Bundle 的部分 info 读
也仍明显慢于无参读，说明编译器的删除效果并非所有实例都一样。

同一候选二进制的 3 轮 A/A、16 个核心场景出现约 −17.0%～+14.8% 波动。因此不把较小变化
作为可靠收益，也不把微基准绝对值移植到其他机器。显著的普通回调收益需与这个噪声范围区分。

### 转换及钩子回归对照

identity + index、纯映射流、8 VF、10 范围，5 轮交替进程。12 项变化为 −3.62%～+1.90%，
属于回归对照范围，不作为提速结论。全部与手写模型及版本间校验一致。普通访问的 56 项对照
最大退化约 1.49%；完整复制及其他小差异应结合上述 A/A 噪声解读。

| pre_access | 寄存器回调 | 操作 | 基线 ns | 优化后 ns |
|---|---|---|---:|---:|
| none | 无 | 读 | 100.88 | 102.34 |
| none | 无 | 写 | 97.39 | 99.23 |
| none | 有 | 读 | 107.23 | 103.35 |
| none | 有 | 写 | 100.30 | 100.42 |
| global | 无 | 读 | 106.64 | 107.30 |
| global | 无 | 写 | 102.68 | 102.85 |
| global | 有 | 读 | 109.66 | 107.72 |
| global | 有 | 写 | 109.64 | 106.74 |
| both | 无 | 读 | 109.39 | 109.32 |
| both | 无 | 写 | 107.20 | 108.64 |
| both | 有 | 读 | 111.03 | 109.66 |
| both | 有 | 写 | 106.18 | 106.61 |

## 复现

两个版本分别构建 Release 库，再用相同的新基准源文件编译；以下命令各执行一次，替换 SDK、
build 和输出路径。只给 benchmark 加 `-O3` 不会改变已编译静态库的优化等级。

```sh
cmake -S /path/to/sdk -B /path/to/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /path/to/build --parallel 2
g++ -std=c++20 -O3 -DNDEBUG -fno-strict-aliasing \
  -I/path/to/sdk/include -I/path/to/sdk/third_party/hlog/include \
  /path/to/candidate/benchmarks/mmio_scalar.cpp /path/to/build/libregfw.a \
  -L/path/to/sdk/third_party/hlog/lib -Wl,-rpath,/path/to/sdk/third_party/hlog/lib \
  -lhlog -pthread -o /path/to/scalar-bench

python3 benchmarks/measure_mmio_scalar.py \
  --baseline /path/to/before --candidate /path/to/after \
  --out /path/to/new-results --blocks 5 --iterations 3000000 --cpu 0

# 单项：元素数、入口、finish、回调、写/读、迭代数、logger
taskset -c 0 /path/to/scalar-bench 256 space 1 info 1 3000000 0
```

输出目录必须不存在。脚本记录环境、二进制 SHA256、原始样本和汇总；`--quick` 只测 16 个核心
场景，`--control` 可加入上文中间版本，基线与候选指向同一二进制可做 A/A。

映射回归对照使用同样的编译/链接选项，将源文件换成 `benchmarks/mmio.cpp` 并加上
`-DREGFW_STATIC_BENCH=1`。两个版本交替运行以下单项，分别取 5 个进程的中位数；将
`PRE_ACCESS` 换成 `global`、`both`，将最后一项换成 0 测读，将第四个参数换成 0 关闭寄存器回调。
这个基准还会与手写范围分发及 switch 模型对比有效访问和最终存储。

```sh
REGFW_BENCH_THREADS=1 REGFW_BENCH_PRE_ACCESS=none REGFW_BENCH_RANGES=10 \
  taskset -c 0 /path/to/mapped-bench 5 4 1 1 mapped 1
```

这里创建并退出一个宿主线程后才计时，MMIO 仍串行执行。每进程每引擎 8 轮、每轮 5 passes，
丢弃首轮；这是 resolver 引用计数处于多线程运行库状态的对照，不是并发访问测试。

## 更新及限制

检出 `codex/regfw-037-mmio-callbacks-20261118` 的固定提交，清理旧构建目录，配套重编译 regfw
和设备消费者。不能只替换 `.a` 或只复制头文件。仍需现有的 `bank.finish()` 初始化步骤；
不自动 finish，也不冻结回调或日志开关。单线程约束与[上一版句柄优化](Performance-Handles.md)相同。

用户私有环境报告的 160/214 ns 是有效的排查线索，但没有对应代码和库构建产物，本轮不能断言
其绝对值由某一个函数造成。未运行私有 `TmDropType_Func` 或 266 项 QEMU 套件，不能承诺
19s 降到 8s，实际部署后仍需运行设备回归。

## 验证记录

- 严格 Release、ASan + UBSan + 泄漏检测，各 35/35 项 CTest 通过。
- 新增直接 Bundle 已 finish/未 finish 的差分测试：混合宽度、掩码/自定义行为、空洞、回调修改
  存储、返回候选值、异常、完整 info 复制、动态全局钩子和快照一致性。
- 现有 identity/index、resolver 换绑及生命周期、View pre_access、动态日志、宽事务测试。
- 安装后独立三 TU 消费者、clang-format 17、生成文档及链接检查、源码摘要检查。
- 以 `benchmarks/mmio.cpp` 的 identity + index 纯映射流作为回归对照；与手写模型验证结果一致。
