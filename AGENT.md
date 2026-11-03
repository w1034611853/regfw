# regfw 0.37.0：开发与集成约定

代码导航见 [模块架构](docs/Architecture.zh-CN.md) 和 [私有实现目录](src/README.md)。`include/regfw/` 是稳定公开入口，`src/internal/` 不对外安装。本版本只进行结构与格式整理，不改变业务接口。

当前生产后端是 **View Range → Shared EntityLayout Hash → Prepared AccessInfo → Existing Access Engine**。`REG_LAYOUT` 接口、REG DSL、MMIO、Resolver/PreAccess、Transaction、Callbacks、Snapshot、Reset 均保持原有行为。运行时的几何计划仅构建一次（按布局类型的函数局部静态对象）；每个 Bank 安装独立的 Entity storage 与访问上下文，共享 View 内地址映射形状。

已移除完整布局的 `consteval` 两遍 FormulaPlan、编译期稀疏元素排序以及旧 Zone/Run/PageDirectory 查找。构建期只保留单个声明类型的宽度、dims、strides 等轻量 constexpr 信息。不要重新引入逐 word `consteval` 输出或多级缓存。

本版结构性非法布局（地址越界/Identity 嵌套/Repeat selector 越界等）在安装阶段抛错；仅声明/工厂捕获与轻量头使用约束仍在编译期检查。历史版本内容如下，**仅作历史参考，以当前源码和 0.36.0 文档为准**。

---

[历史记录：仅供考古，勿在 0.36.0 中照做] 0.33.2 聚焦编译成本与一个查找死循环修复（0.33.0 审查修正版；0.33.2 文档修正）：规则布局直接填写最终计划表；声明形状（计数/占用/密集证明）按声明类型缓存并跨挂载共享；新增轻量声明入口 `<regfw/bundle.hpp>`。公开 API、动态日志等级及回调/事务/快照语义保持；升级时头文件和库配套重编译。布局移到单个 .cpp 的用法见 [LayoutCompilation](docs/LayoutCompilation.md)，当前验证见 [Performance](docs/Performance.md)。

先读本文件，再按任务读取 [完整 API 说明](docs/AgentGuide.md)；不必每次通读源码。构建入口和最小示例见 [README](README.md)，完整快照见 [Snapshot](docs/Snapshot.md)，旧 API 迁移见 [Migration](docs/Migration.md)。

## 当前版本的推荐路径

- C++20，链接编译库 `regfw::regfw`；不是 header-only。头文件与 src 必须来自同一版。
- `REG_ENTITY` / `REG_REPEAT_ENTITY` 创建实体；复杂布局在根 `REG_LAYOUT` 返回 `reg::address_space(...)`，用 `reg::view(name,size).map(target,offset,priority)` 组合。完整说明见 [AddressViews](docs/AddressViews.md)。
- View 负责布局，Entity 负责存储；同一 View 可多次挂载。同级 View 的范围可重叠，未声明 REG 的空洞透明。只在真实实体被遮挡时 warning；最高优先级有多个目标时报错。
- `space()` 默认范围为 `UINT64_MAX & ~uint64_t{3}`，不再限制 4 GiB。地址/局部地址及 identity 请求中的地址字段为 uint64_t；VF ID、数组下标仍为 uint32_t。>4 GiB 范围仍使用 uint64_t 地址，按实际有效地址建立共享 EntityLayout Hash，空洞不分配条目。
- **所有寄存器接口都是实体**，包括命令、doorbell、FIFO 端口；没有独立 command 解码区。
- **所有位宽只使用 TransactionPolicy**；不再有 Split64Policy / split64 builder / 专用 64 bit 暂存。
- ≤64 bit 默认 INDEPENDENT；>64 bit 默认 LOW_TO_HIGH。64 bit 要两拍快照/统一提交时显式配置。
- 不要使用已删除的 `CommandPort`、`window.command(...)`、`map.commands/create_commands`、`REG_COMMAND_REGION`。不要从旧 Rust/API 文档猜测接口。

## 声明与地址

```cpp
struct CommandRegs : reg::RegEntityBundle {
    REG_BUNDLE(CommandRegs);
    REG(DOORBELL, std::uint32_t, reg::at(0).access(reg::write_only));
    REG(PACKET, reg::u512,
        reg::at(0x40).transaction(reg::TransactionPolicy::LOW_TO_HIGH));
};
```

`at` 是实体组内字节偏移；view.map(target,offset) 的 offset 相对当前 View。REG 的一个宽值不等于多个独立 uint32 REG。数组必须给 dims 和字节 strides，rank 1..8，维度非零；最大位宽 2048 bit。map/View/base 对齐要求、访问宽度与边界见完整说明 §4。

单次 REG_ENTITY 创建唯一存储，多次 map 只增加入口；重复实体才复制存储。共享实体可为 Pf reset scope，仅有 VF 地址入口也合法。旧 Window DSL（reg::window、reg::Window、sub_windows 和 window.hpp）已经删除；只返回 AddressSpace。低级 legacy alias/aperture 维护接口未在本次移除，但不能与 AddressSpace 根混合。

`AddressView.map` / `AddressSpace.map` 返回新对象，循环中需赋回。`AddressView.map_repeat(name,offset,reg::repeat(N).stride(S),factory)` 在局部 View 中建立重复族；`AddressSpace.map_repeat(name,base,repeat,factory)` 在根上建立。两者自动提供身份实例编号；直接多次 map 不会自动形成 VF 身份族。实体组映射保留其局部偏移，单寄存器/切片映射把首元素平移到指定 offset。权限只取选中的 View 路径；显式 reserve 是终止区。

`REGFW_ENTITY_SHADOWED` 可通过 map.layout_warnings() 查询，在 finish 时按 hlog WARN 输出。warning 包含覆盖双方、首个冲突地址和局部优先级路径。被遮住的实体仍保存/复位，除非设备显式设置 SnapshotKind；显示地址不是实体的唯一身份。

方向标记 `.vf_to_pf()` / `.pf_to_vf()` 不选择实体；map 明确目标。identity resolver 转换 VF，access group 转换数组下标，两者分别处理。

## 回调

- on_read 接受 `(T candidate,const auto& info)`、`(const auto& info)` 或 `()`；返回完整 T、optional<T>、nullopt 或 void。nullopt/void 沿用 behavior 候选值，不代表失败。
- on_read 返回的完整值只用于响应/读取快照，不写回实体；512 bit 返回完整 512 bit，不是本拍 32 bit。
- on_read 返回 std::nullopt、空 optional 或 void 时，使用进入回调前已经计算好的候选值，不重新读取 entity。普通 RW 实体原为 5，回调将其改成 42 后返回 nullopt，本次仍读到 5，实体变为 42；本次要读到新值必须显式返回新值。RC 等 behavior 的副作用仍生效，分拍快照续读仍使用已建立的快照。
- on_write 的 `(T old,info)` 中 T 是**旧值**。新值用 stored_after；宽值用类型化 info / raw info.wide，或同步回调中读取实体。
- on_read_complete 的 T 参数是本次响应快照；不要从 ref() 取 FIFO 读响应。
- 所有回调属于整个 REG 实体，数组也只有一组回调槽；不能逐元素重复注册。按 info.index 分派。
- 每种回调只可首次注册一次；resolver 可换绑。回调同步执行，没有自动线程/异步队列。
- 内部 ref()/原生 union/.data 访问跳过 MMIO、权限、behavior 和回调。

## 分拍事务与同地址不同读写含义

LOW_TO_HIGH：低端首读建立完整快照，高端读完成；写到高端统一提交。HIGH_TO_LOW 反向。完成端按 offset/size 判断，不检查全部字节覆盖，软件必须保证协议。

同一元素读快照与写暂存独立，一笔读和一笔写可以分拍交错。读返回 FIFO 响应，写提交下发命令；两者可用同一个 REG。两笔同方向事务不能在同一元素上混叠，不按 CPU/线程自动隔离。

## 完整快照与既有 QEMU 模块流

```cpp
auto snapshot = reg_bank.save();
reg_bank.load(snapshot);
// 包含 <regfw/vmstate.hpp>，并提供原 vmstate_fields.hpp/qemu_interface.h 后：
reg_bank.save(file);
reg_bank.load(file);
// 父对象：VMSTATE_FIELDS(reg_bank, fifo, vf_route, ...);
```

沿用 `hac::snapshot::Writer/Reader` → module_var_save/load，不使用 VMStateDescription。保存 Migratable 实体值及每元素的 write_pending/write_mask/read_snapshot/有效性/成功标记；半读/半写恢复后继续，不重放回调。Transient/Derived 有意排除。

加载先验证全部 schema/状态和 payload，再提交，不使 bank 半恢复。格式 v2，旧 v1 拒绝。裸 dump_state(span)/restore_state(span) 仅值数组，不能用作完整事务快照。目标先初始化相同布局及回调；FIFO/路由表/线程等外部业务状态由父字段管理。指针、函数、logger、锁不保存。

## 并发与错误

同一 RegisterMap 不提供线程安全；MMIO、内部更新、reset/save/load 由设备工程同步。允许停在 guest 两次 MMIO 之间保存，不允许 MMIO 回调中递归保存/加载或递归调用同 map 的 MMIO。异步任务复制值，不保存临时 info 引用。

回调异常会捕获；有有效 logger 且 ERROR 启用才打印。on_read 异常当前可能返回 0/Ok，on_write 异常不回滚；不能只凭 success() 判断业务成功。success() 对 Ignored 也为 true。需要总线错误时用解析/验证阶段的错误机制，参见完整说明 §15。

## 日志与排查

布局日志：Debug / describe_layout(Summary) 只输出 View/Entity/保留区的大布局。Trace / describe_layout(Full) 增加紧凑寄存器地址表，实际严重等级为 TRACE。多维数组一行表示 dims、strides、地址公式和可见选择，重复族一份代表加实例范围；禁止逐元素/逐 beat 展开。部分覆盖不能伪装成完整连续映射，详情使用 explain_address。


Debug 一行摘要；Trace 同样单行，用 ` | ` 分隔操作结果、地址路径、方向/身份/多维下标。route 使用名称，vf/id 及下标只在改变时显示箭头，中间地址不重复。ERROR 保留完整实体路径和原因链，框架日志不再附带完整 C++ 函数签名前缀。

`describe_layout()` / `explain_address()` 不执行业务解析器/回调；`resolve_address_with_identity()` 会执行 identity resolver。实际有效数组元素看 info.index，不靠 target 地址推断（下标转换不重编码地址）。

修改 API 时同时更新本文件、README、完整说明、迁移说明及相关测试。运行 CTest 和文档生成检查，关键完整示例必须实际编译运行。

紧凑寄存器地址表按每行代表的最小实际可见入口地址进行 64 位数值升序排序；数组仍只列一行。部分覆盖时不使用可能已隐藏的数组首地址作为排序依据。

PF 自己的业务存储应使用独立的 `REG_ENTITY(pf_business, PoeVfRegs, reg::entity().reset_scope({reg::ResetScope::Pf}))`；VF 用 `REG_REPEAT_ENTITY(vf, PoeVfRegs, VF_COUNT, reg::entity().reset_per_instance(reg::ResetScope::Vf))`。不要为 PF 在 VF 重复数组后额外加一项：若 count 为 VF_COUNT+1，vf[VF_COUNT] 确实是独立实体，并会被标为 Vf(VF_COUNT)，打印它不表示硬件新增了一个 VF。PF 业务实体有独立 PF 入口，不能当作 VF 重复族的副本省略。

先用 view(name,size).map(...).map_repeat(...) 组合完整子模块，再在根 address_space 上 map 若干大 View，是支持的组织方式。局部 count*stride 必须在父 View 内；外层复制布局不复制实体或事务。

布局表中的地址映射行统一显示 `VIEW`；映射实体时显示 `VIEW → entity: <原始实体组或 REG 名称>`，`reset` 表示目标存储的复位归属。PF、PF_VF、VF 的多个入口会显示同一个共享实体目标名，不再把每个入口标成新 ENTITY。折叠重复族的目标名/复位信息取 i=j=k=0 代表实例；实体范围列是映射边界，不表示整段地址都有实体寄存器。

## 按 AddressView 绑定 pre_access（0.24.0）

优先使用命名键，布局只声明挂载范围，在 bank 初始化后绑定设备函数：

```cpp
REG_PRE_ACCESS(CheckBusiness); // 放在命名空间作用域

// REG_LAYOUT 中的片段，vf[i] 为此前声明的实体实例：
auto business = reg::view("BUSINESS", POE_BUSINESS_SIZE)
    .map(vf[i])
    .using_pre_access(CheckBusiness);

// bank 初始化后、finish 前：
reg_bank.map().set_pre_access(CheckBusiness,
    [this](const reg::RegAccessInfo& info) {
        // 此处做设备检查；info.address 是原请求地址。
        // info.view_path 是 identity 转换后实际选中目标的 View 路径。
    });
```

独立的小函数也可直接 `.pre_access(fn)`，fn 签名为 `void(const RegAccessInfo&)`。builder 返回新对象，必须使用返回值；同一 View 只能声明一个钩子。不要在 REG_LAYOUT 中捕获局部变量的引用，或捕获可能随 Bundle 构造返回而移动的 this；设备函数推荐使用命名键，等 bank 构建完成后绑定。重复工厂里的 i 如需直接捕获必须按值。

钩子执行顺序为 **全局 map.set_pre_access(fn) → 最终选中路径的父 View → 子 View**，发生在 identity、权限/宽度和 access-group 下标解析后、behavior/事务更新前。动态 VF i→j 时执行 j 的实际目标路径，保留来源 info.address/request_id，info.index/resolved_id 已是转换结果。只执行获胜分支；透明空洞落到另一分支时不会执行空洞 View 的钩子；共享实体在不同入口可使用不同检查。

每次有效 MMIO beat 都调用，包括宽 REG 的暂存写和快照续读。RO 忽略写、入口权限拒绝、未映射/保留地址及 resolver 失败不会运行钩子。内部 ref()/data 更新、reset、save/load、describe_layout、explain_address 不运行钩子。

命名键未绑定时 finish 报 Configuration；有意延迟绑定可 map.defer_pre_access(key)。之后可 set/clear/has_pre_access(key)；clear 只清该键，不影响全局钩子或其他键。已声明但未绑定的 View 钩子在访问时按钩子失败处理，不会静默放行；需要禁用时显式绑定空函数。重绑/clear 与 MMIO 由设备同步，不新增线程安全。

异常沿用原有 pre_access 行为：打印并停止后续钩子、behavior 和寄存器回调；写返回 Ignored，读返回 0（状态可能为 Ok），不回退其他映射。不要用此异常机制要求明确的总线 AccessError；此类需求仍在解析/验证阶段处理。钩子是 void，不使用返回 false 表达拒绝。

钩子列表在构建时预编译，无钩子的记录为空指针；MMIO 不按 View 数量扫描。函数、捕获对象和钩子链不保存到 bank 快照；恢复前按设备初始化流程重建布局并绑定，外部业务状态由父对象保存。

0.24.1 优化了钩子调用：普通 MMIO 调用不复制函数的 shared_ptr；仅当回调在 pre_access 链执行期间重绑/清除时，保留旧函数至本次 pre_access 链结束。回调内自修改仍受支持，后续钩子读取最新绑定；旧捕获对象的析构时点可能延后到本次 pre_access 链结束。框架仍不支持未同步的并发重绑。

## 0.26.0 MMIO 执行计划与回调适配

公开 Entity/View、resolver、pre_access、回调和 transaction 写法保持。finish 把稀疏页目录冻结为连续探测表，并为已映射地址生成 32 字节分发记录；原生 32-bit RW 的数据地址、掩码和静态允许方向已确定，不再逐拍通过冷元数据计算存储地址。大地址间的空洞仍不分配页。

原生 32-bit 无 identity/index 派生或 pre_access、方向 Independent 且没有访问日志时，回调使用轻量调用帧。完整 info 在与用户函数一起编译的适配器中准备，让编译器删除该函数未使用的字段准备；需要完整字段、按值复制 info 或 typed index 的回调仍得到全部语义。读回调的两条适配路径共享同一个可变函数对象，不能各自复制一份计数或 FIFO 状态。异常和回调内启用日志时仍能构造完整诊断信息。

常见的单 uint32 坐标、无条件命名 index 映射减少字段查找；仍逐拍调用当前 resolver，并执行原有维度范围、映射冲突和容量检查。其他条件、多个字段、自定义字段转换、部分访问、RC/W1C、自定义 codec/behavior、宽寄存器和事务保留完整路径。

晚绑定回调、resolver 换绑/清除、全局/View pre_access 和动态日志等级仍生效。on_read 返回值不写回实体，nullopt 使用回调前候选值；读完成顺序、异常和快照 v2 不变。回调 info 只在同步调用期间借用，值复制保留有效字段；名称/View 路径仍借用，禁止 memcpy 整个 info。每 Map 仍需外部同步，不允许同 Map MMIO 重入。

本版以额外的紧凑分发记录和冻结页索引换取访问速度；读源保存一份函数状态和两条调用适配器，初始化开销增加。分发表、函数和缓存指针不进入快照。头文件和库必须一起升级并重新编译。数据、额外内存说明和测试范围见 [MMIO 性能报告](docs/Performance.md)。

## 0.27.0 固定布局与执行计划

固定几何优先阅读 [StaticLayout](docs/StaticLayout.md)。0.28 推荐 `REG_LAYOUT([](auto layout){...})`，自动收集宏声明，用 layout.vf[i] 引用实体，不写 REG_SCHEMA。原 `REG_STATIC_LAYOUT([]{...})` / static_view / target 显式入口继续兼容；显式 REG_SCHEMA 若保留仍检查遗漏。两种静态工厂都必须可常量求值，不能捕获实际 this。REG_DYNAMIC 和按运行期值决定的布局继续使用 `REG_LAYOUT([&]{...})`。初始化仍检查实际实体完整性，禁止静默少映射。

编译期计算实际占用地址、优先级赢家、路径权限、身份族、固定执行路径类别、重定向目标索引和只读地址索引；不是第一次初始化时的缓存，也不在启动时重新排序/建 hash 表。实体值/事务、实际存储指针、命名 resolver/pre_access 槽、诊断字符串仍需实例化。多个 bank 共享只读表，不共享任何实体、回调或事务状态。当前每个占用 word 有静态描述，巨大数组/大量别名仍要评估常量表大小。

支持完整 Bundle 映射、重复 View、多个 64 位入口、透明洞、优先级覆盖 warning、保留区、只读入口、命名 identity/pre_access。单 REG/切片目标、内联 View 捕获钩子和运行期布局参数继续使用动态 View；不要伪称其已在编译期处理。静态实体表不进入快照，快照仍为 v2。

`REG_STATIC_INDEX(NAME,REG,SOURCE,OUTPUT,DIM)` 是单 uint32 坐标的无条件命名下标映射，维度/范围/线性系数在编译时验证；resolver、映射结果、换绑和清除仍动态。条件、多字段、自定义转换、metadata_only 使用原 REG_ACCESS_GROUP。所有路径继续保留完整 info；回调 info 的复制和访问语义不变。

初始化默认不再强制 O0，遵循项目优化等级。需要旧编译速度取舍时显式定义 REGFW_FAST_COMPILE_INIT；它不是 static layout 的必要条件。动态入口仍完整验证。对外评估需同时报告构造/绑定/finish、MMIO 相对 switch 和额外只读数据成本，不能只宣传 MMIO 提升。


映射的原生 32-bit 专用路径使用轻量调用帧；命名 resolver 和读写回调的完整 info 在各自适配器中按实际用途优化。完整字段/复制 info 仍有效。命名 resolver 的普通与专用适配器共享同一个可变函数对象，部分访问与完整访问交替不能分裂闭包状态。全局 pre_access 已绑定时继续通用路径；resolver 在本次访问中新增全局钩子时，专用路径构造完整上下文并在同一次访问中执行，不能重新调用 resolver。

静态索引按平均已用页密度选择：每页至少 256 个 word 使用常量页目录与页内 ID，较稀疏时使用 word hash；二者都在常量求值中生成。禁止为了页表把巨大高地址空洞实体化。密集/稀疏路径必须同时验证页内空洞、缺页、别名、保留区及 snapshot/late binding，不得把 hash 碰撞视为命中。

## 0.29.0 直接路径选择与读写专用处理

finish 为 32 字节分发记录分别确定读/写的普通 scalar、mapped scalar 或 generic 路径；访问入口直接选择已确定的类别，普通与映射处理按读写分别编译。动态 logger/pre_access、借用状态、callback/resolver 换绑继续即时生效。处理类型按类别共享，不为每个寄存器生成独立函数；日志/错误/宽寄存器/事务/部分访问继续保留完整路径。

resolver 仍使用原 shared_ptr 局部副本保护调用生命周期，没有缓存 resolver 结果。同步调用内 clear/换绑、旧捕获对象析构时点、析构中换绑和异常展开均有专项回归。试验过的 CallbackSlot 借用实现没有稳定收益，未进入发布版；不要参考消融试验补丁使用这个内部类型。

性能比较须保持编译选项、CPU、负载和线程运行库状态一致。本轮基准即使不额外创建线程，进入 main 时 libc 的单线程标志也已为 0，因此只报告确认采用多线程引用计数路径的进程。MMIO 访问串行执行；这不是同一 Map 并发安全测试。每 Map 仍需外部同步。

## 0.30.0 大数组初始化

同一 RegScope 的已声明区间始终不重叠，使用有序索引检索新声明的前驱/后继，不再逐元素扫描所有旧声明。越界、对齐、重复地址和部分重叠仍报 Configuration，数组空洞可被其他声明使用。多个冲突同时存在时，诊断所选的冲突对象可能与声明顺序扫描不同，但仍包含实际冲突地址、双方名称及声明位置。

单 REG 数组按完整元素占用区间检查重叠；地址非单调时排序后检查相邻区间，不能假设多维数组的线性索引顺序等于地址顺序。宽元素不能仅检查首字地址。所有验证完成后才提交实体，不允许非法数组留下实体或地址索引。

名称仍保留完整逐元素信息，静态布局只把公共前缀移到元素循环外；没有将必要诊断移到 MMIO 热路径，也没有通过关闭日志获得构造收益。TRACE 报告生成仍有成本，需单独测量。初始化还需分配存储、事务状态和实例映射；不声称全部编译期完成。

基准必须覆盖同一 Bundle 中多个大数组及 9 份实体，另外测试完整静态 PF/PF_VF/VF 拓扑。仅构造测试不能称为完整布局或 machine_init；报告 init、finish、Summary/Full 格式化及内存，并保留旧版同轮结果。

## 日志与当前优化约束

访问日志最终字符串始终在 emit_log 的等级检查后格式化。0.32.0 进一步在映射快路径准备完整 Resolution/RegAccessInfo 之前检查当前日志等级；不能将此描述为原来无条件拼接最终字符串。不能在 finish 时永久缓存日志开关，resolver/on_read 在当前访问中调整等级仍必须生效。Normal/Trace/Hidden 策略保持。

映射路径保留固定 Entry 的局部副本，仅在日志、钩子或错误处理需要时构造完整 Resolution；减少 Entry 副本的试验没有稳定收益，未进入发布版本。resolver 与 pre_access 可能修改实体，原有旧值获取和刷新阶段必须保留；不得缓存 resolver 的返回值，不得删除必要范围/权限检查。规则块只用于编译期计数，最终访问不增加按块除法或扫描。初始化 word 索引绑定依赖 attach_static 对 vector 存储的 move/swap 保持元素地址，禁止后续扩容使指针失效。

## 0.33.3 稀疏规则构建器

规则快路径以寄存器元素为单位建模几何：每个挂载记录首元素地址、绑定与共享的 `StaticElementPlan`（元素数、占用宽度、单调性与维度步长，或交错布局的有序元素表）。地址冲突按**实际占用区间**判定——稀疏 stride、多维交错、混合宽度（含 128/2048-bit）、大地址空洞、以及包围盒重叠但字节互补的 View 叠加都走直接填表；真实字节重叠、保留区覆盖实际寄存器、越界与 identity 家族越界仍回退完整优先级求解器（行为不变）。稠密布局另有连续块快路由，与 0.33.2 相当或更快。

identity 重定向只收集真实占用 word 的 `(identity, inner)` 对；稀疏空洞不会生成 redirect。`family.count>64` 与 `2×word` 预算规则保持。几何遍结果以定容 `Snapshot` 按值传递，填表不再二次展开；容量溢出（bindings>2048/scopes>2048/mounts>20480/identities>128）自动回退两遍路径，不需要使用者干预。

`REGFW_REQUIRE_NONOVERLAP_FAST_PATH` 严格模式下，无法走快路径的布局在编译期按 ActualOverlap/ReservedIntersection/SelfOverlap/OutOfBounds/IdentityBoundary/ArithmeticOverflow/UnsupportedGeometry 分类终止。`StaticBuild::stats`（mounts/elements/words/pages/redirects/path）供诊断与基准使用，默认构建行为不变。

## 0.33.0 编译成本与查找修复

规则布局（measuring 阶段已证明占用块连续不重叠、无保留区相交）在第二次常量求值中直接写入最终 `StaticPlan` 的 words/addresses/index/pages/redirects：不再生成与 word 数同体积的候选表、不再排序、不再经过临时 words/redirects/pages 缓冲，也不再最终复制。重叠覆盖、保留区相交、稀疏数组等复杂布局仍走完整构建器。两条路径的完整计划语义经 `tests/static_regular.cpp` 逐字段差分与 `plan_digest` 哈希对拍验证一致；`compile_static_layout` 增加测试用 `RegularFill` 模板参数强制旧路径做 oracle，正常代码不使用。

寄存器声明的元素计数、占用宽度、密集矩形证明与 stride 排序按声明类型（`StaticRegisterShape`）求值一次，供同一声明的所有挂载复用；稠密转置数组的"物理序→线性序"表同样按声明共享，且仅在两序不同且被实际使用时常量求值，不会进入最终二进制。运行期 `StaticPlanView`/`StaticWord`/分发路径与 0.32.0 完全一致。

新增轻量声明入口 `<regfw/bundle.hpp>`：包含 REG/ENTITY、句柄访问、回调、RegisterBank 与 `REG_LAYOUT_DECL` 所需全部定义，但不引入 constexpr 布局编译器。只声明寄存器的设备头应改用它；定义 `REG_LAYOUT_IMPL`/`REG_STATIC_LAYOUT` 或在 TU 内使用带工厂的 `REG_LAYOUT` 仍需 `<regfw/regfw.hpp>`。旧 `declarations.hpp` 与 `regfw.hpp` 行为不变。在仅包含 bundle.hpp 的 TU 里使用 `REG_LAYOUT`/`REG_LAYOUT_IMPL` 会在编译期被 `static_assert(REGFW_COMPILED_LAYOUT_ENTRY,...)` 立即拒绝（GCC 13/14 与 Clang 一致；0.33.0 依赖"局部类型模板缺定义"的诊断，仅 GCC 13 可靠），已纳入编译失败契约测试。

修复：GCC 13 对 consteval 生成的大型静态对象，默认成员初始化的查找索引哨兵槽可能被发射为零值，使开放寻址探测永不终止（0.32.0 及更早即存在，与布局路径无关，症状随二进制布局漂移）。现在 `compile_static_layout` 显式赋值全部索引槽位，`tests/static_regular.cpp` 增加对发射后对象的运行期扫描。
