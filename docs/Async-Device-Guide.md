# 异步设备改造：回调、FIFO、锁与 worker

本文说明如何把已有设备业务接入当前异步端点；接口和生命周期契约见 [Async](Async.md)。寄存器的原生字段和普通 C++ 容器都不会自动加锁。保护来自：MMIO 通过端点进入状态锁，后台操作通过同一端点的 `with_state` 进入状态锁。

## 1. “短锁”由代码范围决定

每个端点有一把状态 mutex，一个 Map 只允许挂一个端点，因此通常是每 Bank 一把锁。它保护真实寄存器、访问暂存状态以及设备选择一起保护的 FIFO/计数等业务状态。

前台在锁内完成地址/权限/宽度/动态路由解析、原引擎行为和同步回调；如有声明的后台命令，还会预留队列槽、捕获参数并提交任务。worker 出队后不持状态锁或队列锁，只在 `with_state` 的作用域内获取状态锁。同步端点也使用状态锁，但捕获的工作在解锁后由调用线程执行。

“短”是设备代码必须遵守的约定，没有按微秒强制解锁、超时抢占或自动拆分回调。resolver、pre_access、on_read/on_write、on_read_complete 也处于前台访问路径；其中的 sleep、大循环、日志阻塞或外部调用都会延长持锁时间。锁内仅做有界的状态处理和必要参数复制，DMA、大计算和等待移到锁外。

框架拒绝嵌套状态区、持状态锁等待 executor 等操作，但不能自动识别任意 C++ 函数中的外部锁或阻塞。`with_state` 返回引用会被拒绝；通过指针、捕获对象或其他方式逃逸状态仍需设备代码避免。当前报告没有单独测量锁持有/等待时间分布，不保证硬实时上限。

## 2. 复杂 on_write/on_read 怎么拆

先按硬件协议判断哪些动作必须在本次 MMIO 返回前完成，再拆函数：

| 阶段 | 执行位置 | 应做的事 |
|---|---|---|
| 接收访问 | 原 on_write，已经持状态锁 | 立即可见的寄存器状态、接收计数、捕获本次命令参数 |
| 执行业务 | `defer` 提交的函数，锁外 | DMA、计算、提交目标 CPU work 等 |
| 完成提交 | 后台的 `with_state` | 结果、错误码、完成号等一起更新 |

声明 START 为 `deferred_writes` 后，其 on_write 必须调用一次 `defer`；完整绑定方式见 [初始化示例](Async.md#初始化示例)。任务需要的地址、长度、身份、下标、序号应在接收时复制，不能在执行时重新读取已经可能被 guest 改写的配置，也不能捕获临时 `info` 的引用。

后台读改写要把整个操作放在状态区：

```cpp
endpoint.with_state([&] {
    bank->pf.STATS.bits.cnt++;
});
```

如果锁内读取 cnt，锁外用旧值计算，再锁内赋回，就可能覆盖 guest 或其他完成回调的更新。对于必须锁外计算的业务，设备需定义参数快照、版本检查或合并规则，框架无法推断业务意图。

命令协议也必须完整：若只允许一笔在途，按原硬件的接收规则处理重复 START；若允许多笔，保存各自的命令号、结果槽和完成号，不能让后一次结果提前覆盖尚未消费的结果。单个 DONE 位不能自动表达任意数量的在途命令。合法拒绝应在状态变更前的协议/验证阶段处理；不能在原引擎已存入值后简单 return 或抛异常并假定写操作已回滚。

对于 **on_read**：

- 配置、状态和已经完成的结果，在当前访问内返回。
- RC、FIFO pop 等决定本次响应的状态变化，也必须在当前访问内完成。
- 可以预先产生的数据，在后台生成后提交；on_read 只消费已提交数据。
- 若硬件要求一次读操作现场 DMA/计算后直接返回结果，该读仍须等待；只有原协议允许“触发、查询完成”才能异步化，不能用旧值或占位值代替。

`on_read` 返回值就是本次响应；只改 `.ref()` 后返回 void/nullopt 不会重新取值，而是沿用原引擎已经计算的候选值。需要使用实际返回值的后续短处理，应使用 `on_read_complete` 的响应参数。当前 `defer` 只支持声明的写回调；不能在读回调里调用 defer/post，或嵌套 with_state。读触发补充数据的耗时业务需要独立设计通知路径，不能把这一能力当成当前已经提供的通用接口。

## 3. FIFO：每次读 pop，空时返回 0xff

检查是否为空、取队首、pop 和返回必须属于同一个临界区。在 on_read 中，端点已经持锁，直接操作即可；后台生产、外部线程读取数量或 reset 清空时，使用同一端点的 with_state。不需要再为这个仅属于一个 Bank 的 FIFO 增加第二把 mutex。

以下完整示例使用固定容量 FIFO，后台依次生产 11 和 22，前台依次读到 11、22、0xff。示例统一使用 4 字节访问，`0xff` 是设备约定的无效值。FIFO 数组虽不属于寄存器存储，仍由 Bank 状态锁保护。

```cpp
#include <regfw/regfw.hpp>
#include <regfw/async.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

struct FifoRegs : reg::RegEntityBundle {
    REG_BUNDLE(FifoRegs);
    REG(DATA, std::uint32_t, reg::at(0).access(reg::ro));
    REG(COUNT, std::uint32_t, reg::at(4).access(reg::ro));
};
struct FifoBank : reg::Bundle {
    REG_BUNDLE(FifoBank);
    REG_ENTITY(pf, FifoRegs);
    REG_LAYOUT([&] {
        return reg::address_space("FIFO").map(reg::view("PF", 8).map(pf), 0);
    });
};
struct FifoState {
    std::array<std::uint32_t, 64> data{};
    std::size_t head{}, tail{}, count{};
    // 这两个方法本身不加锁；调用者必须持端点的状态锁。
    std::uint32_t pop() {
        if (!count) return 0xff;
        const auto value = data[head];
        head = (head + 1) % data.size();
        --count;
        return value;
    }
    bool push(std::uint32_t value) {
        if (count == data.size()) return false;
        data[tail] = value;
        tail = (tail + 1) % data.size();
        ++count;
        return true;
    }
};
void require(bool ok) {
    if (!ok) throw std::runtime_error("FIFO example failed");
}
int main() {
    reg::AsyncExecutor executor;
    FifoState fifo;
    reg::RegisterBank<FifoBank> bank(reg::space(8));
    bank->pf.DATA.on_read([&]() -> std::uint32_t { return fifo.pop(); });
    bank->pf.COUNT.on_read([&]() -> std::uint32_t { return fifo.count; });
    bank.finish();
    // 最后构造端点，使其先析构；Bank、FIFO 和 executor 均比端点活得久。
    reg::AsyncEndpoint endpoint(bank.map(), executor);

    for (std::uint32_t packet : {11u, 22u}) {
        require(endpoint.post([&, packet] {
            // 耗时的数据生产放在状态区外；本例的数据已由 packet 给出。
            bool inserted = endpoint.with_state([&] { return fifo.push(packet); });
            require(inserted);  // 示例不应满；真实设备按原协议处理满队列。
        }));
    }
    executor.drain(); // 仅演示程序等待生产完成，MMIO 读不等待 worker。
    require(executor.statistics().failures == 0);
    auto read = [&](std::uint64_t address) {
        auto result = endpoint.read(address, 4);
        require(result.success());
        return result.value;
    };
    require(read(4) == 2);
    require(read(0) == 11);
    require(read(0) == 22);
    require(read(0) == 0xff);
    require(read(4) == 0);
}
```

多核读取由锁排定顺序，每个元素只消费一次；不承诺哪个 CPU 先取得锁。空队列的读和后台 push 同时发生时，谁先取得锁决定本次读返回 0xff 还是新数据。worker 正在忙其他任务不会阻止消费已有元素，但仍可能影响后续生产速度。

设备还需明确：

- FIFO 满时如何背压、拒绝或记录溢出；`post` 已接受和最终 push 成功是不同的结果。任务失败不会自动恢复设备状态。
- 有效数据是否允许等于 0xff；若允许，需按原协议结合 valid/count 等信息区分。
- 允许的访问宽度、对齐和字节序。若协议只允许完整 4 字节读，应在适配/验证层拒绝其他访问，不能 pop 后才报错。若允许半字或分拍，明确每次访问 pop，还是首拍 pop 并保存完整响应快照。框架不会推断 FIFO 协议。
- 独立的 COUNT 读和 DATA 读不是一个原子事务，另一 CPU 可以在两次读取之间消费数据；同一宽寄存器的同方向分拍事务也不会按 CPU 自动隔离。
- 若一个 FIFO 被两个不同 Bank 共用，两把 Bank 锁不能互相保护它。需要统一状态归属或设计共享同步及锁序；不能从一个 Bank 的状态区嵌套进入另一个 Bank。
- FIFO 数组属于外部业务状态，Bank 快照不会自动保存它。清空、reset、save/load 必须与生产者、消费者和在途工作协调。

## 4. worker 到底有几个

**每个 AsyncExecutor 实例创建一个 worker 和一个工作队列。** 端点接收 executor 引用，不会自动再创建线程。多个 Bank 可以共用一个 executor，也可以在初始化时显式使用不同 executor。后者是 API 可表达的配置，不代表当前四设备 QEMU 压测已经验证多 executor 方案。

| 操作 | 实际执行上下文 |
|---|---|
| MMIO 解析、寄存器行为、短 on_read/on_write | 调用 endpoint.read/write 的线程，持目标 Bank 锁 |
| 异步端点的 defer/post 任务 | 该端点所属 executor 的唯一 worker |
| 同步端点的捕获任务 | 原调用线程，释放 Bank 锁后执行 |
| with_state | 调用它的线程；只获取状态锁，不切换线程 |
| QEMU timer 到期回调 | QEMU timer 上下文，短状态更新通过 with_state |
| ARM CPU 系统寄存器 work | 目标 vCPU 线程，完成通知再提交设备状态 |

当前宿主样例 `device/async_model.cpp` 的 A/B/C/D 共用一个 executor，因此四个设备的重任务在一个 worker 上执行；每个 Bank 仍有自己的状态锁。后台 A 发起 DMA 到 B 时，B 的短 MMIO 回调在这个 DMA 调用线程上执行，新的重任务入队，不能内联递归执行它。旧同步基线设备保留同步处理。

同一 executor 按任务入队顺序执行，但前台状态操作、timer、CPU work 会与它交错。任务向外部系统提交请求后返回，不等于外部请求已经完成；单 worker 不保证外部完成顺序，也不提供跨设备的完整链条原子性。不同 executor 更没有全局顺序保证。

共享 worker 的长任务会阻塞后面的全部重任务。它不抢占、不自动切分任务，没有优先级/公平调度保证。不能在 worker 中等待同一队列的后继工作，也不能让不同 executor 互相同步等待形成环；当前检查不能检测所有跨 executor 等待环。

## 5. 当前边界与下一步验证

已有验证覆盖即时 RW、RC/W1C/FIFO、参数捕获、原生位域竞争、半字和宽事务、动态路由、满队列拒绝、关闭后继、四设备 DMA 链、timer 和 CPU work。它不等于对任意业务代码的完整正确性证明。

### 已修复：并发 drain 活锁

旧实现将 drain 表示为队列控制事件；若还有队列项，它会再次排到队尾。两个线程同时 drain 同一 executor，且两个标记都在队列中时，每个标记都能看到另一个，因而反复入队，两个调用均不能返回。历史探针在 worker 受控阻塞时让两个 drain 标记入队，再释放 worker，复现 `finished=0 accepted=3 completed=1`。

当前实现删除了排空标记，多个 drain 调用者通过条件变量等待同一条件：队列为空、无正在执行的任务、无已经预留但尚未提交的写。等待会释放队列 mutex，不持 Bank 锁，不占数据或控制队列槽。worker 完成任务及捕获对象清理后、或最后一个未提交预留取消后，会检查条件并通知所有等待者。后继任务在当前任务结束前入队，因此也被排空；后台任务抛异常后仍会正确更新执行状态并唤醒等待者。

现在允许多个线程并发调用同一 executor 的 drain，不再要求调用方额外串行化。每次 drain 仍计入 accepted/completed 控制操作统计，但不计入队列占用和 high_water。停止外部生产者的要求不变；drain 返回表示观测到一次 executor 空闲，不阻止之后新提交工作，也不等待尚未回调的外部 timer/CPU 请求。executor 必须活到所有调用者退出，不能与析构并发。

回归覆盖 2/8/80 个并发调用者、满数据队列、多代后继任务、空队列但任务仍在执行、任务异常、worker 自等拒绝、已预留写的提交/取消，以及空闲时直接返回。宿主保留历史探针 `build/docs-async-guide/drain_probe.cpp` 和失败日志 `drain-probe.log`；修复后的重跑记录使用独立日志，不能覆盖历史复现证据。

| 边界 | 当前状态及接入真实设备前需要处理的内容 |
|---|---|
| 锁竞争与尾延迟 | 未单独测持锁/等锁分布；应在真实重回调下测 p95/p99、队列等待和最长任务，并评估 BQL 等待状态锁的影响 |
| 共享 worker 的阻塞 | 一个长任务延迟所有共享设备的重任务；拆任务或分 executor 前，明确顺序、共享状态与跨设备依赖 |
| 容量与完成通道 | 满队列拒绝已测；尚无通用业务完成专用队列或硬件背压协议，后继 post 也可能失败。executor 的内部控制槽不等于业务完成保留额度 |
| 失败与异常 | MMIO 捕获异常、后台任务失败、DMA 部分成功不提供通用回滚；设备需正确更新 ERROR/BUSY/DONE，避免永久等待或重复外部写入 |
| 外部回调生命周期 | endpoint.close/executor.drain 不能代替外部系统的取消和排空；退出前必须停止并排空 timer、CPU work 或其他完成源，保证捕获对象存活 |
| reset/暂停/迁移 | 样例覆盖 quiesced 生命周期；尚未完成任意 guest reset/flush、VM 暂停时的全部后台行为及热拔插/迁移。generation 防旧完成，不能撤销已发出的 DMA；FIFO 等外部状态另行保存 |
| IRQ 与 guest 顺序 | 未做 IRQ 验证；完成结果、DMA 内存可见性、完成标志和中断次序按真实协议设计。guest 多核仍需要自己的 MMIO 屏障、命令生产者协调与宽事务约束 |

这些是需要逐项落实的适用条件和验证缺口，不应宣传为框架已经自动解决。生产设备迁移时，先按上述规则审查回调及状态归属，再复用现有测试环境增加对应业务用例。
