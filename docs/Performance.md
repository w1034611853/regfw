# 同步版性能与复测

[返回 README](../README.md) · [基准构建与运行](../benchmarks/README.md)

| 测量内容 | 实测记录 |
|---|---|
| 单轨 MMIO、手写 switch 对照、查表空洞和错误探测 | [同步架构精简](Performance-Architecture-Sync.md) |
| 字表密度、多 View、Reserve、初始化和内存 | [紧凑分发](Performance-Compact-Dispatch.md) |
| 句柄调用、绑定生命周期与快照体积 | [固定生命周期与紧凑快照](Performance-Fixed-Lifetime.md) |
| 按需回调上下文构造 | [标量回调](Performance-MMIO-Scalar.md) |
| 旧版分发和钩子拆分 | [分发分析](Performance-Dispatch.md) · [钩子](Performance-Hooks.md) |

复测从相同优化等级的配套头文件和库构建两个消费者。记录请求分布、日志与回调配置，
单核交替测量并比对读回、存储与回调效果。库内微基准从 MMIO 入口计时。

早期交付报告和实验数据可在[同步版发布标签](https://github.com/w1034611853/regfw/tree/v0.38.0-sync)查阅。
