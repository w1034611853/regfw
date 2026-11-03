# HLog 使用 Wiki、推荐与规范

> 适用版本：hlog runtime/client 3.2.0，`HLOG_ABI_VERSION = 3`  
> 适用对象：C、C++、Rust 设备模型开发者，QEMU 集成者，测试与运维人员

## 1. HLog 要解决什么问题

HLog 是 QEMU 进程内共享的日志与诊断查询框架。所有动态加载的 C、
C++、Rust 模型共用一个 `libhlog.so.1`、一个日志文件和一套运行时控制
接口。

这次升级的目标不只是“把日志写出来”，而是建立下面的诊断闭环：

1. 日志用稳定字段记录事件、对象、结果和因果关系。
2. AI 根据错误事件和关联 ID 缩小可疑模块与状态范围。
3. AI 通过 `query-list` 和 `query-help` 发现模块提供的诊断能力。
4. AI 带参数执行只读 query，验证当前状态和配置。
5. AI 把日志历史与当前状态组合为可复核的根因结论。

一句话原则：**日志描述“发生了什么”，query 回答“现在是什么”。**

## 2. 术语与强制级别

本文使用以下约束词：

- **必须**：不满足就不应合入。
- **应该**：默认遵守；偏离时应说明理由。
- **可以**：按模块需要选择。

HLog 中有三种不同的注册：

| 对象 | 注册者 | 作用 |
| --- | --- | --- |
| `libhlog.so.1` | QEMU loader | 提供进程级日志与 query runtime |
| QMP `hlog` 命令 | QEMU loader | 暴露运行时控制和 query 调用入口 |
| 模块 query | C/C++/Rust 模型 | 暴露模块自己的只读状态查询 |

模块 query 未注册不代表 runtime 未加载；runtime 未加载也不代表 QEMU
本身不可运行。

## 3. 五分钟上手

### 3.1 构建并安装 runtime

```bash
cd /path/to/lib/hlog
./scripts/install.sh
```

默认生成：

```text
lib/libhlog.so.1.0.0
lib/libhlog.so.1 -> libhlog.so.1.0.0
lib/libhlog.so -> libhlog.so.1
```

验证安装结果：

```bash
./scripts/verify_runtime.sh ./lib/libhlog.so.1
```

### 3.2 启动 QEMU

```bash
HLOG_PATH=/absolute/path/to/lib/hlog/lib/libhlog.so.1 \
qemu-system-aarch64 \
    -readconfig /absolute/path/to/hlog.conf \
    ...
```

没有设置 `HLOG_PATH` 时，HLog 作为可选能力静默跳过，QEMU 仍可正常
运行。

### 3.3 检查是否可用

HMP：

```text
(qemu) hlog show
```

QMP：

```json
{"execute":"hlog","arguments":{"action":"show"}}
```

返回结果包含当前日志文件、logger 名称、模块名和有效等级。

## 4. Logger 接入

### 4.1 命名规范

logger 的逻辑名称采用：

```text
module/instance
```

例如：

```text
bmu/0
bmu/1
crg/chip0
```

必须遵守：

- `module` 使用稳定、小写的产品模块名，例如 `bmu`、`crg`。
- `instance` 使用稳定实例标识，不使用进程地址或随机数。
- 同一模块的 C、C++、Rust 实现必须使用相同模块名。
- 模块重构时不要随意改名；名称是配置、检索和 AI 规则的一部分。

### 4.2 C

```c
#include "hlog.h"

HLogLogger *logger = hlog_create_named(
    "bmu", "0", HLOG_LEVEL_INFO);

if (logger == NULL) {
    fprintf(stderr, "hlog create failed: %s\n", hlog_last_error());
    return;
}

HLOG_DEBUG(logger, "event=BMU_CMD_RX opcode=%#x", opcode);
HLOG_INFO(logger, "event=BMU_CMD_DONE opcode=%#x result=ok", opcode);
```

### 4.3 C++

```cpp
#include "hlog.hpp"

class Bmu {
public:
    Bmu() : logger_("bmu", HLOG_LEVEL_INFO, "0") {}

    void submit(uint32_t opcode)
    {
        HLOG_DEBUG(logger_, "event=BMU_CMD_RX opcode={:#x}", opcode);
        HLOG_INFO(logger_,
                  "event=BMU_CMD_DONE opcode={:#x} result=ok",
                  opcode);
    }

private:
    hlog::Logger logger_;
};
```

### 4.4 Rust

```rust
use hlog::{create_module_logger_with_name, ldebug, linfo, LogLevel};

let logger = create_module_logger_with_name(
    "bmu",
    "0",
    LogLevel::Info,
)?;

ldebug!(logger, "event=BMU_CMD_RX opcode={opcode:#x}");
linfo!(logger, "event=BMU_CMD_DONE opcode={opcode:#x} result=ok");
```

如果同一模块存在多个实例，应使用当前项目已有的命名接口为 logger
配置稳定实例名，使最终显示名保持 `module/instance`。

## 5. 日志等级规范

| 等级 | 使用场景 | 禁止用法 |
| --- | --- | --- |
| `Trace` | 高频步骤、寄存器级细节、状态机逐步迁移 | 默认开启 |
| `Debug` | 分支决策、输入摘要、资源选择、关键中间状态 | 周期性无信息刷屏 |
| `Info` | 初始化、生命周期、配置生效、重要操作成功 | 每包、每寄存器访问 |
| `Warn` | 可恢复异常、降级、重试、临界水位 | 已导致操作失败的错误 |
| `Error` | 当前操作失败，但进程或设备仍可继续 | 同一错误逐层重复打印 |
| `Critical` | 设备或进程已无法可靠继续 | 普通参数错误 |
| `Off` | 配置中禁用 logger | 业务代码主动打印 |

推荐判断：

- 正常运行时长期需要保留的低频事实用 `Info`。
- 只有定位问题时才需要的决策信息用 `Debug`。
- 高频但可用于重放执行路径的信息用 `Trace`。
- 一个失败只在拥有完整上下文的边界打印一次 `Error`。
- 下层应返回带原因的错误，上层补齐对象和操作信息后打印。

## 6. 面向 AI 的日志格式规范

HLog 自动添加时间、等级、logger 和 host thread ID：

```text
[time.millisecond] [level] [logger] [tid=...] payload
```

模块负责保证 `payload` 稳定、明确、可检索。

### 6.1 必须包含稳定事件码

关键日志必须以稳定的 `event=` 字段开头：

```text
event=BMU_MAP_LOOKUP result=miss vf=3 l_pool=2
```

事件码规范：

```text
MODULE_DOMAIN_ACTION
```

使用大写字母、数字和下划线。事件码表示语义，不表示源码函数名。例如：

```text
BMU_MAP_LOOKUP
BMU_QUEUE_OVERFLOW
CRG_RESET_ASSERT
NIC_PACKET_DROP
```

事件码发布后应该保持稳定。重构函数、文件或类型时不应修改事件码。

### 6.2 使用 `key=value` 字段

推荐顺序：

```text
event -> result/phase -> 关联ID -> 对象ID -> expected/actual -> reason
```

推荐示例：

```text
event=BMU_MAP_LOOKUP result=miss req_id=1842 vf=3 l_pool=2
event=BMU_MAP_REJECT result=error req_id=1842 reason=no_vl_pool
event=BMU_VF_RESET phase=done vf=3 duration_us=117 result=ok
```

必须遵守：

- 字段名使用小写 `snake_case`。
- 布尔值统一写 `true` 或 `false`。
- 枚举使用稳定英文标识，不打印仅供人阅读的模糊描述。
- 地址、寄存器和位图使用 `0x` 十六进制。
- 数量默认十进制，并在字段名体现单位，例如 `duration_us`。
- 缺失值写 `value=unknown`；不要用 `0` 代替未知。
- 同一字段在不同事件中必须保持相同含义和单位。
- 包含空格的字符串应使用双引号，避免 AI 错误拆分字段。

### 6.3 关联 ID

跨函数或跨模块操作必须携带稳定关联 ID，例如：

```text
req_id=1842
packet_id=0x1028
reset_seq=7
```

同一次操作的接收、路由、执行、完成和失败日志必须使用同一个 ID。
没有关联 ID 时，AI 很难区分并发请求产生的交错日志。

### 6.4 错误日志必须回答四个问题

每条关键错误至少回答：

1. 哪个操作失败？
2. 哪个对象失败？
3. 期望值和实际值是什么？
4. 直接原因是什么？

推荐：

```text
event=BMU_MAP_PROGRAM result=error vf=3 l_pool=2 \
expected_vl_pool=4 actual_vl_pool=none reason=pool_exhausted
```

不推荐：

```text
map failed
invalid value
unexpected error
```

### 6.5 防止无效日志

禁止：

- 同一错误在每层调用栈重复打印。
- 只有 `enter/exit` 而没有输入、结果或耗时。
- 打印裸指针作为对象身份。
- 在日志中输出密钥、口令、完整凭据或用户隐私。
- 在 `Info` 中持续打印轮询、心跳和寄存器读写。
- 动态拼接字段名，导致相同事件每次结构不同。

## 7. 运行时等级控制

### 7.1 启动配置

使用 QEMU 原生 `-readconfig`：

```ini
[hlog "all"]
level = "warn"

[hlog "bmu"]
level = "info"

[hlog "bmu/0"]
level = "debug"

[hlog "crg"]
level = "off"
```

优先级固定为：

```text
完整 logger > 模块 > all > logger 创建时的默认等级
```

规则同时作用于当前 logger 和以后注册的 logger。

### 7.2 运行时修改

```text
(qemu) hlog set all warn
(qemu) hlog set bmu debug
(qemu) hlog set bmu/0 trace
```

QMP：

```json
{"execute":"hlog","arguments":{
  "action":"set","target":"bmu","level":"debug"
}}
```

运行时修改只更新内存，不回写配置文件。问题定位结束后，应恢复等级，
避免长期保留高频 `Trace`。

### Console 输出（3.1.0 新增）

普通日志默认只写文件。HMP 可以开启/关闭所有当前及未来 logger 的
console 镜像，仍遵守原有等级过滤，不回放已有日志：

```text
hlog console
hlog console on
hlog console off
```

显式 `HLOG_CONSOLE_TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL`（C/C++）和
`lconsole_trace!/lconsole_debug!/lconsole_info!/lconsole_warn!/lconsole_error!/lconsole_critical!`
（Rust）只写 console，不写文件，不受镜像开关及文件 logger 等级影响。
还提供 `HLOG_CONSOLE_HEXDUMP` 与 `lconsole_hexdump!`。

3.2.0 增加带逐行前缀的 u8/u16/u32/u64 buffer 视图和数组/span 宏，例如
`HLOG_HEXDUMP_DEBUG(logger, hlog::u32, buffer)`。完整参数形式、字节序与尾部
规则见 [Buffer / Hexdump](Hexdump.md)。

Console 使用宿主 stderr，不是 Guest UART，也不会自动输出到单独的
`-serial tcp/pty` 窗口。格式是原文件日志前加 `[HLOG]`，按等级着色；
终端默认启用颜色，重定向时默认关闭。文件内容、归档格式不变。
Query 的 HMP 对齐表格输出不受这个开关影响。

示例、颜色表、完整接入步骤见 [Console.md](Console.md)。

## 8. 模块诊断 Query

### 8.1 什么时候应该注册 query

以下状态不适合依赖持续日志记录，应该提供 query：

- 当前配置和映射表。
- 当前队列、池、水位和资源占用。
- 当前状态机状态。
- 当前 PF/VF、端口或通道状态。
- 累计计数器和最近一次错误摘要。
- AI 定位某类错误时必须验证的内部不变量。

query 必须只读。修改状态的控制命令不属于 HLog query。

### 8.2 Query 命名

完整 target：

```text
module/instance/name
```

例如：

```text
bmu/0/map-table
bmu/0/pool-usage
bmu/0/vf-state
crg/chip0/reset-state
```

规范：

- 三段都使用小写。
- `name` 使用 `kebab-case`，并描述返回的状态。
- 不使用 `get-`、`show-` 前缀；query 本身就是查询。
- 不在 query 名称中编码参数，例如不要注册 `vf3-map-table`。
- 实例差异放在 `instance`，筛选条件放在参数中。

### 8.3 发现和调用

```text
(qemu) hlog query-list
(qemu) hlog query-list bmu
(qemu) hlog query-list bmu/0
(qemu) hlog query-help bmu/0/map-table
(qemu) hlog query bmu/0/map-table vf=3
(qemu) hlog query bmu/0/map-table "vf=3 l_pool=1"
```

过滤是大小写敏感的完整路径段匹配：

- `bmu` 只匹配 `bmu/...`，不会匹配 `bmu2/...`。
- `bmu/1` 不会匹配 `bmu/10/...`。
- 没有匹配项时返回空列表，不回退到全量列表。

QMP：

```json
{"execute":"hlog","arguments":{
  "action":"query-list","target":"bmu"
}}
```

```json
{"execute":"hlog","arguments":{
  "action":"query",
  "target":"bmu/0/map-table",
  "args":"vf=3 l_pool=1"
}}
```

### 8.4 参数规范

当前支持：

| 类型 | 示例 | 说明 |
| --- | --- | --- |
| `string` | `port=eth0` | 含空格时使用引号 |
| `u32` | `vf=3` | 非负 32 位整数 |
| `u64` | `address=4096` | 非负 64 位整数 |
| `i64` | `offset=-4` | 有符号 64 位整数 |
| `bool` | `detail=true` | 使用明确布尔值 |

参数可以是 required、optional 或 repeatable。限制如下：

- 每个 query 最多声明 64 个参数。
- 一次调用的原始参数字符串最多 16 KiB。
- 一次 query 输出最多 1 MiB。
- 未知参数、重复的非 repeatable 参数、缺少必填参数和类型错误会在
  回调执行前被 runtime 拒绝。

参数名称必须使用小写 `snake_case`。描述必须写清有效范围、单位、默认
行为和特殊值，例如：

```text
vf: required u32, valid range 0..7
l_pool: optional u32, omitted means all logical pools
detail: optional bool, default false
```

### 8.5 C 注册

```c
static const HLogQueryParamSpec params[] = {
    HLOG_QUERY_REQUIRED_U32("vf", "VF index, range 0..7"),
    HLOG_QUERY_OPTIONAL_U32(
        "l_pool", "Logical pool; omitted means all pools"),
};

bmu->map_query = hlog_query_register(
    "bmu", "0", "map-table",
    "Show l_pool to vl_pool mappings",
    params, HLOG_ARRAY_SIZE(params),
    bmu_map_table_query, bmu);

if (bmu->map_query == HLOG_QUERY_INVALID_HANDLE) {
    HLOG_ERROR(logger, "event=BMU_QUERY_REGISTER result=error reason=%s",
               hlog_last_error());
}
```

销毁设备状态前必须执行：

```c
(void)hlog_query_unregister(bmu->map_query);
```

### 8.6 C++ 注册

```cpp
map_query_ = hlog::register_query(
    "bmu", "0", "map-table",
    "Show l_pool to vl_pool mappings",
    {
        hlog::required_u32("vf", "VF index, range 0..7"),
        hlog::optional_u32(
            "l_pool", "Logical pool; omitted means all pools"),
    },
    [this](const hlog::QueryArgs &args,
           hlog::QueryOutput &output) {
        uint32_t vf = args.u32("vf");
        auto l_pool = args.optional_u32("l_pool");
        if (l_pool && *l_pool >= 4) {
            throw std::invalid_argument("l_pool is out of range");
        }
        output.print(
            "BMU map-table (instance=0, vf={})\n", vf);
        output.print("L_POOL  VL_POOL  VALID\n");
        output.print("------  -------  -----\n");
        if (l_pool) {
            output.print(
                "{:<6}  {:<7}  {}\n",
                *l_pool, lookup(vf, *l_pool), "yes");
            output.print("Total: 1 entry\n");
        } else {
            for (uint32_t lp = 0; lp < 4; ++lp) {
                output.print(
                    "{:<6}  {:<7}  {}\n",
                    lp, lookup(vf, lp), "yes");
            }
            output.print("Total: 4 entries\n");
        }
    });
```

`hlog::QueryRegistration` 是 move-only RAII 对象，必须保存在设备实例中，
不能只保存在构造函数的局部变量里。

### 8.7 Rust 注册

```rust
let map_query = hlog::register_query("bmu", "0", "map-table")
    .description("Show l_pool to vl_pool mappings")
    .required_u32("vf", "VF index, range 0..7")
    .optional_u32(
        "l_pool",
        "Logical pool; omitted means all pools",
    )
    .register(move |args, output| {
        let vf = args.required_u32("vf")?;
        let l_pool = args.optional_u32("l_pool")?;
        if l_pool.is_some_and(|value| value >= 4) {
            return Err(QueryError::invalid_argument(
                "l_pool is out of range",
            ));
        }
        writeln!(output, "BMU map-table (instance=0, vf={vf})")
        .map_err(|_| QueryError::internal("query output failed"))?;
        writeln!(output, "L_POOL  VL_POOL  VALID")
        .map_err(|_| QueryError::internal("query output failed"))?;
        writeln!(output, "------  -------  -----")
        .map_err(|_| QueryError::internal("query output failed"))?;
        if let Some(l_pool) = l_pool {
            writeln!(
                output,
                "{:<6}  {:<7}  {}",
                l_pool,
                lookup(vf, l_pool),
                "yes",
            )
            .map_err(|_| QueryError::internal("query output failed"))?;
            writeln!(output, "Total: 1 entry")
                .map_err(|_| QueryError::internal("query output failed"))?;
        } else {
            for lp in 0..4 {
                writeln!(
                    output,
                    "{:<6}  {:<7}  {}",
                    lp,
                    lookup(vf, lp),
                    "yes",
                )
                .map_err(|_| QueryError::internal("query output failed"))?;
            }
            writeln!(output, "Total: 4 entries")
                .map_err(|_| QueryError::internal("query output failed"))?;
        }
        Ok(())
    })?;
```

`QueryRegistration` 必须作为设备成员保存。对象被 drop 时会注销 query。

## 9. 面向人的 Query 输出规范

Query 主要通过 HMP 供开发、测试和运维人员现场查看，默认输出必须优先
保证人的可读性。多条同类记录推荐使用列对齐表格：

```text
BMU map-table (instance=0, vf=3)
L_POOL  VL_POOL  VALID
------  -------  -----
0       4        yes
1       7        yes
Total: 2 entries
```

必须遵守：

- 第一行使用简短标题，说明查询对象、实例和实际生效的过滤条件。
- 多条同类记录使用表格；列名、列含义和列顺序必须长期稳定。
- 列名应简洁明确，例如 `VF`、`L_POOL`、`VL_POOL`、`VALID`。
- 表格末尾必须有 `Total: N entries` 汇总，便于人确认结果是否完整。
- 同一次 snapshot 中的所有记录应来自一致状态视图。
- 空结果仍然输出表头和 `Total: 0 entries`，不能只返回空字符串。
- 参数错误使用框架错误返回，不要输出半份成功结果后再报错。
- 地址、寄存器值和位图统一使用带 `0x` 前缀的十六进制；数量、索引和
  ID 默认使用十进制；布尔值统一使用 `yes`/`no`。
- 单元格内容不得包含换行。内容较长时提供 `detail=true` 等显式参数，
  不要让默认表格无限变宽。
- 如果结果因框架输出上限被截断，必须明确显示 `Truncated: yes`，不能
  让用户误以为结果完整。

空结果示例：

```text
BMU map-table (instance=0, vf=7)
L_POOL  VL_POOL  VALID
------  -------  -----
Total: 0 entries
```

AI 同样可以读取列名稳定的文本表格。若未来需要程序稳定消费 query
结果，应在 QMP 中提供结构化对象，或者增加显式的 `format=json`，不要
把 HMP 默认输出改成 JSON 或逐行 `key=value`。

## 10. Query 实现安全规范

query 回调在调用路径上同步执行，必须做到：

- 只读，不修改设备、寄存器或 guest 可见状态。
- 获取必要状态的短锁，优先复制 snapshot 后释放锁再格式化。
- 不等待 guest 事件，不 sleep，不执行无界 I/O。
- 不递归调用自身，不在自身回调中注销自身。
- 不返回裸 host 指针、敏感信息或未初始化内存。
- 输出前先验证业务范围，例如 `vf < VF_NUM`。
- 遍历必须有确定上限，并尊重 1 MiB 输出限制。
- 销毁模块状态前先注销 query。

推荐 snapshot 模式：

```text
获取读锁 -> 复制必要字段 -> 释放锁 -> 格式化输出
```

注销时 runtime 会先从 registry 中移除 target，再等待正在执行的回调
结束。因此回调持有的 opaque/closure 状态在注销完成前必须保持有效。

## 11. AI 自主定位标准流程

推荐把以下流程固化到诊断 Agent：

### 第一步：确认 HLog 状态

```text
hlog show
```

确认 runtime 可用、日志文件位置、相关模块 logger 名称和有效等级。

### 第二步：刷新并收集错误窗口

```text
hlog flush
```

按 `event=`、关联 ID、对象 ID 搜索当前日志：

```bash
rg -n 'event=BMU_|req_id=1842|vf=3' logs/qemu_device.log
```

历史压缩日志：

```bash
zgrep -nE 'event=BMU_|req_id=1842|vf=3' \
    logs/qemu_device.*.log.gz
```

### 第三步：发现模块诊断能力

```text
hlog query-list bmu
hlog query-help bmu/0/map-table
```

AI 不应猜 query 名称或参数；必须先读取列表和 help。

### 第四步：验证当前状态

```text
hlog query bmu/0/map-table "vf=3 l_pool=2"
```

把 query 结果与日志中的 `expected`、`actual`、`reason` 进行交叉验证。

### 第五步：形成可复核结论

AI 的结论至少包含：

- 首个异常事件及时间。
- 关联请求或对象。
- 日志显示的因果链。
- 执行过的 query 和关键返回字段。
- 已确认事实、推断和仍缺失的证据。
- 建议下一步，不把推断描述成事实。

## 12. 日志文件、轮转与检索

默认行为：

- 当前文件：`./logs/qemu_device.log`。
- 单文件大小：100 MiB。
- 每批文件数：10。
- 每约 1000 MiB 切走一批并后台压缩。
- 新日志立即写入新的 `qemu_device.log`，不等待压缩。
- 归档文件：`qemu_device.<时间戳>.<PID>.<序号>.log.gz`。
- 压缩失败时保留原始 staging 文件，不阻断日志。
- 已完成归档不会自动删除。

搜索当前日志：

```bash
rg -n 'event=BMU_MAP_REJECT' logs/qemu_device.log
```

搜索 gzip：

```bash
zgrep -n -- 'event=BMU_MAP_REJECT' \
    logs/qemu_device.*.log.gz
```

普通 `grep/ggrep` 不会自动解压 gzip。也可以使用支持压缩输入的：

```bash
rg -z -n 'event=BMU_MAP_REJECT' logs/
```

生产环境必须另行配置归档保留期限和磁盘水位告警。

## 13. 常用 HMP/QMP 命令

| 目的 | HMP |
| --- | --- |
| 查看 logger 和等级 | `hlog show` |
| 修改全部等级 | `hlog set all warn` |
| 修改一个模块 | `hlog set bmu debug` |
| 修改一个实例 | `hlog set bmu/0 trace` |
| 强制刷新 | `hlog flush` |
| 列出全部 query | `hlog query-list` |
| 只列 BMU query | `hlog query-list bmu` |
| 只列 BMU 0 实例 | `hlog query-list bmu/0` |
| 查看 query 参数 | `hlog query-help bmu/0/map-table` |
| 执行 query | `hlog query bmu/0/map-table "vf=3 l_pool=1"` |

## 14. 构建与升级注意事项

当前 runtime/client ABI 是 v3，SONAME 仍为 `libhlog.so.1`。升级顺序：

1. 替换 hlog 源码、头文件和 Rust client。
2. 执行 `scripts/install.sh`，生成 ABI v3 runtime。
3. 执行 `scripts/verify_runtime.sh`。
4. 重新编译 QEMU loader。
5. 重新编译所有 C/C++/Rust 模型。
6. 完全退出并重新启动 QEMU。

已经加载到进程中的 `.so` 不会因磁盘文件替换而自动更新。

出现下面错误时：

```text
hlog ABI mismatch: client=3, runtime=2
```

说明 client 已更新，但 QEMU 实际预加载的 runtime 仍是旧版本。应检查：

```bash
readlink -f lib/hlog/lib/libhlog.so
readlink -f lib/hlog/lib/libhlog.so.1
```

以及启动时的 `HLOG_PATH`。

如果由外层 Rust `build.rs` 触发 `install.sh`，新版安装脚本会清理外层
Cargo/Make jobserver 变量并串行化并发安装，避免：

```text
failed to acquire jobserver token
early EOF on jobserver pipe
```

## 15. 模块接入验收清单

每个模块合入前应完成：

- [ ] logger 使用稳定的 `module/instance` 名称。
- [ ] 关键日志有稳定 `event=`。
- [ ] 对象 ID、关联 ID、结果和原因使用独立字段。
- [ ] 等级符合本文规范，默认运行不会被高频日志淹没。
- [ ] 不记录凭据、敏感数据和无意义裸指针。
- [ ] 至少为关键配置、状态或资源表提供只读 query。
- [ ] query 名称符合 `module/instance/name`。
- [ ] 参数 help 包含类型、范围、单位和省略行为。
- [ ] query 默认输出为列名和顺序稳定的人类可读表格。
- [ ] query 表格包含标题、表头和 `Total: N entries` 汇总。
- [ ] query snapshot 一致、执行有界、不会长时间持锁。
- [ ] query registration 生命周期与设备状态一致。
- [ ] 测试未知、缺失、重复、越界和无匹配参数。
- [ ] 使用 `query-list module` 验证模块过滤。
- [ ] 用一次真实故障验证 AI 能从日志找到 query 并形成证据链。

## 16. 推荐的 Code Review 问题

Reviewer 应重点询问：

1. AI 能否只靠 `event=` 找到同类故障？
2. 并发请求交错时，是否有 ID 能还原单次操作？
3. 错误是否包含对象、期望、实际和原因？
4. 这条日志应该持续记录，还是更适合做 query？
5. query 是否只读、快照一致、有界且能安全注销？
6. query 参数和输出 schema 是否可长期兼容？
7. 开启模块 `Debug` 后，日志量是否仍可控制？
8. AI 的结论能否被人用同一组命令复核？

## 17. 设计边界

HLog 当前负责：

- 进程内统一日志。
- logger 等级与启动规则。
- 文件轮转、批量压缩和 flush。
- 模块只读 query 的注册、发现、参数校验和执行。
- QEMU HMP/QMP 控制入口。

HLog 当前不负责：

- 修改设备状态的控制命令。
- 远程日志采集、索引平台和长期归档删除。
- 自动替用户确定根因。
- 替代模块自身的锁、快照和生命周期管理。

AI 自主定位依赖两部分共同成立：框架提供稳定入口，模块开发者提供
高质量事件和可验证的当前状态。只有日志很多，没有结构和 query，不能
形成可靠的自主诊断能力。
