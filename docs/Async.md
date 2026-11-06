# 保持寄存器语义的可选异步执行

公共头 `<regfw/async.hpp>`，链接 `regfw::regfw`。当前实现以一份真实 Bank 和短状态锁替代旧原型的发布副本。执行模式只在 C++ 初始化代码中选择，没有动态切换接口。原有直接 RegisterMap API 保留；同一个 Map 仍不能绕过端点并发访问。

复杂回调拆分、FIFO 空读返回 0xff 的完整示例、锁的范围、worker 数量及当前验证缺口，见 [设备改造指南](Async-Device-Guide.md)。每个 executor 一个 worker；普通 MMIO 和短回调在调用线程执行，with_state 也不切换线程。

## 访问语义

前台在状态锁内调用原访问引擎，完成权限/宽度/路由解析、RW/W1C/RC、FIFO 读回调、宽事务和短回调，再返回结果。普通稳定 RW 写入后，有序读取立即看到新值，不等待 worker。普通读写和状态轮询不入队。

`on_read` 仍决定本次返回值；`on_read_complete` 得到实际响应；原生 `.bits`/`.ref()` 在这些短回调中已受状态锁保护。动态 PF/VF identity/index 每笔访问按当前状态解析，不需要路由镜像或 refresh。别名共享同一实体存储，后台不 replay MMIO。

耗时业务必须显式用 `defer()` 提交，且只能从配置为 `deferred_writes` 的 on_write 捕获回调提交。每次有效提交调用一次 defer；后台函数使用复制的参数执行。

## 初始化示例

以下片段用于已有 Bank（声明 ADDR、LEN、START、RESULT、DONE 的模型），run_dma 由设备适配层提供：

```cpp
reg::AsyncExecutor executor;  // 必须比端点活得久
// ... 构造 bank，随后绑定回调 ...
std::unique_ptr<reg::AsyncEndpoint> endpoint;
bank->pf.START.on_write([&] {
    const auto address = bank->pf.ADDR.ref();
    const auto length = bank->pf.LEN.ref();
    endpoint->defer([&, address, length] {
        auto result = run_dma(address, length);  // 不持 Bank 锁
        endpoint->with_state([&] {
            bank->pf.RESULT.ref() = result;
            ++bank->pf.DONE.ref();
        });
    });
});
bank.finish();
reg::AsyncAccessOptions options;
options.deferred_writes = { &bank->pf.START.entity() };
endpoint = std::make_unique<reg::AsyncEndpoint>(bank.map(), executor, options);
```

队列槽在原引擎完成解析和验证后、修改寄存器/宽事务状态及运行短回调之前预留。声明使用实体，所以别名、动态 VF 映射和数组都按实际解析目标工作；宽写只在提交拍预留。成功访问结束后提交捕获的后台函数，worker 不会二次执行写行为。

必须先完成配置、绑定捕获回调及 finish，再挂载一个端点。一个 Map 只能有一个端点。executor 和 Bank 必须比端点活得久。

## 同步与异步选择

```cpp
reg::RegisterEndpoint<reg::ExecutionMode::Synchronous> sync(bank.map(), options);
// 或者，在另一种初始化配置中：
reg::RegisterEndpoint<reg::ExecutionMode::Asynchronous> async(bank.map(), executor, options);
```

同步选择同样保护状态，并在释放状态锁后、MMIO 返回之前执行捕获的工作，不创建 executor。异步选择提交给单 worker。两者使用相同寄存器语义，仅后台工作的执行时间不同；命令完成状态按设备原协议处理。两种模板端点都可传给 `make_qemu_bridge()`。

## 状态更新与并发

```cpp
endpoint.with_state([&] {
    bank->pf.STATUS.bits.cnt++;
    bank->pf.RESULT.ref() = result;
    bank->pf.DONE.ref() = 1;
});
```

一次作用域持一次锁，多字段更新一起提交。原生字段仍是普通 C++ 内存，不会自动拦截字段访问。worker、timer 或 CPU work 完成回调访问 Bank 时必须进入该作用域；MMIO 的短回调已持锁，不能再嵌套 with_state。

禁止在状态作用域中调用 DMA、取得 BQL、进入其他 Bank 或等待线程，也不能带出 Bank 的裸引用/指针。with_state 可返回复制值，返回引用会被编译期拒绝。运行时拒绝嵌套状态作用域以及持状态锁调用 executor 等待；`state_lock_held()` 可供设备外部调用入口做断言。

guest 写 100 与后台 cnt++ 按状态锁排定顺序：先写再增得到 101，先增再写得到 100。后台在锁外用旧值算好 6 再赋值仍是业务层覆盖，框架不会推断任意赋值的合并意图。

数据和 DONE 应在同一个 with_state 中更新；RAM DMA 必须完成后再提交完成状态，外部 IRQ 通知在解锁后发出。分拍宽寄存器继续遵守原协议，不能将同一元素的两个同方向事务跨 CPU 混叠。

## 队列、错误和生命周期

- AsyncExecutor 使用短 mutex 保护的预分配有界环形队列，默认容量 65,536；任务本身是 std::function，捕获可能分配内存，不是 wait-free 或零分配接口。
- `deferred_writes` 每笔提交预留一个任务。队列满时返回 DeviceError，寄存器行为、捕获回调及最后一拍事务提交均未执行；没有后台工作的普通访问仍可进行。
- 设备必须根据真实硬件的额度/BUSY/最大在途数设计接收策略。该错误不是通用硬件背压协议，不能让依赖无限命令接收的 guest 无修改兼容；本地四设备用例的在途工作远低于配置容量。
- 声明了后台写却没有 defer，或者在未声明的回调中 defer，会报告错误。捕获函数应短小且不抛异常；对任意回调已修改的原生字段不提供通用异常回滚。容量拒绝与回调自身出错不是同一种保证。
- `endpoint.post(task)` 是显式后台任务，函数运行时不持状态锁；需要访问 Bank 时自行 with_state。返回 false 表示未接受。不能在状态锁内调用 post 来绕过预留协议。
- `executor.fence()` 等待队列前缀；停止外部生产者后，`executor.drain()` 还跟随后继任务。worker 内禁止等待同一 executor。
- 同一 executor 的多个 drain 可并发等待，条件为队列为空、无正在执行任务、无未提交预留；等待者不入队。旧的排空标记活锁已修复，见 [设备改造指南](Async-Device-Guide.md#已修复并发-drain-活锁)。drain 仍计入 accepted/completed，不增加队列 high_water；返回不阻止后续提交，executor 必须活到所有调用者退出。
- 停止外部调用后，`endpoint.close()` 拒绝新 MMIO/外部任务，等待该端点已接受的任务及其直接后继，随后解除 Map 挂载。已接受任务在关闭过程中仍可更新状态和提交本端点的后继。不得持 BQL 或其他 worker 需要的锁调用 close。
- 同 executor 上关闭另一个端点也会被拒绝；不能以线程内等待解决设备间依赖。跨设备后继应使用正常 MMIO/显式事件并保证所引用设备仍存活。
- snapshot/reset 可在 with_state 内操作 Bank；取消/排空 DMA、timer、CPU work 和更新代次是设备生命周期职责。快照不会自动保存 std::function 队列或外部任务。

QEMU 线程注册通过 executor 的 thread_enter/thread_leave 注入，SDK 不链接 QEMU。外部 API 的锁规则由设备适配层遵守。

## 从旧异步原型迁移

删除了 `auto_publish`、`publish_last`、`publish*`、`dynamic_routing`、`refresh_routes`、`after_event` 和旧端点 invoke。当前 Bank 状态即时可见，不需要发布；动态路由在正常访问时解析；线程安全状态操作用 with_state。

旧 on_write 中耗时操作拆成“锁内捕获 + defer 的锁外工作 + with_state 完成更新”。原 on_read 的返回值及 RC/FIFO 消费继续即时处理，不能机械迁移为后台事件。

## 已验证范围

原生 async 用例包括：阻塞 worker/队列满时普通 RW 立即读回、拒绝前无状态修改、参数捕获、同步模式、RC/FIFO/W1C、8 线程访问、半字更新、宽事务和提交拍拒绝、前台与 worker 的 17,000 次位域自增、锁外等待约束、ADCDBCA、实时路由及已解析命令身份、关闭时后继任务、重复挂载和错误 defer。

SDK 36 项 CTest 以及 ThreadSanitizer 下的 async 用例通过。真实 QEMU ARM64 MTTCG 验证见宿主工程 `docs/SDK-Validation.md`，结果在 `results/sdk-report.md`。不将原型允许旧读的历史结果作为本实现的验收证据。
