# 路线 A 设计审查：在 5.10.274 保留并发读的 PI rwsem

**状态：设计稿，尚无内核实现或运行数据。** 实验基点 `e59ee8288ae3ce42e969464aca7ae22d94f1da3f`（路线 B 冻结）。工作树 `~/os-lock/linux-rwsem-pi-rtbase`，分支 `experiment/rwsem-pi-rtbase`。计划增加独立类型，不替换 `struct rw_semaphore`、不启用生产调用点、不做路线 C，也不移植 `rwlock` 或整个 PREEMPT_RT。

**设计结论：** 多读者通过一个原子计数并发持锁。一把普通、无 PI tag 的 RT-Mutex 负责写者互斥及关闭读入口之后的准入。如果 H 等这把准入锁，可以向它的持有者捐赠优先级；如果写者已经得到准入锁，只在等活跃读者排空，则不会向读者捐赠，即使只剩一个读者也一样。这两个等待阶段应分别测试和统计。

## 1. 源码依据

| 版本 | 实现位置与审计结论 |
| --- | --- |
| Linux v6.12 | [`include/linux/rwbase_rt.h`](https://github.com/torvalds/linux/blob/v6.12/include/linux/rwbase_rt.h#L8-L27) 包含 `atomic_t readers` 与 `struct rt_mutex_base`。 [`kernel/locking/rwbase_rt.c`](https://github.com/torvalds/linux/blob/v6.12/kernel/locking/rwbase_rt.c#L53-L300) 中 `rwbase_read_trylock()` 用 acquire CAS；`__rwbase_read_lock()` 持 `wait_lock` 进入 RT-Mutex 等待，成功后先加读计数再释放准入锁。`rwbase_write_lock()` 先取准入锁、关闭入口，再等待排空；读解锁用 `atomic_dec_and_test()`，最后一位退出才进入唤醒慢路径。写释放、取消及降级通过不同增量发布计数。 |
| Linux v6.12 集成 | [`kernel/locking/rwsem.c`](https://github.com/torvalds/linux/blob/v6.12/kernel/locking/rwsem.c) 的 `CONFIG_PREEMPT_RT` 分支以 `__rt_mutex_lock()`、`__rt_mutex_slowlock_locked()` 等内部入口接入共享 `rwbase_rt.c`，外层仍按 rwsem 的读/写语义记账。[`kernel/locking/rtmutex.c`](https://github.com/torvalds/linux/blob/v6.12/kernel/locking/rtmutex.c#L1678-L1803) 与 [`include/linux/rtmutex.h`](https://github.com/torvalds/linux/blob/v6.12/include/linux/rtmutex.h#L23-L66) 分离 `rt_mutex_base` 和带调试信息的 `rt_mutex`。不会照搬共享 `spinlock_rt.c` 所需的状态和调度宏。 |
| 历史 PREEMPT_RT | kernel.org 的 [`patch-5.10.237-rt131.patch.xz`](https://www.kernel.org/pub/linux/kernel/projects/rt/5.10/older/patch-5.10.237-rt131.patch.xz) 中 `include/linux/rwsem-rt.h` 和 `kernel/locking/rwsem-rt.c` 使用 `struct rt_mutex`，调用该补丁提供的 `rt_mutex_slowlock_locked()` 和 `__rt_mutex_lock_state()`。它有 `readers != WRITER_BIAS` 的绕过分支，新读者可在写者排空期间进入。此分支已被主线 [`286deb7ec03d`](https://github.com/torvalds/linux/commit/286deb7ec03d941664ac3ffaff58814b454adf65) 删除；v6.12 `rwbase_rt.c` 顶部的旧流程注释不代表实际准入行为。路线 A 选用 v6.12 的不绕过语义，不直接套用旧补丁。 |
| 项目 Linux 5.10.274 | [`include/linux/rtmutex.h`](../include/linux/rtmutex.h) 提供 `struct rt_mutex`。[`kernel/locking/rtmutex.c`](../kernel/locking/rtmutex.c) 的 `rt_mutex_slowlock()` 自己取 `wait_lock`、初始化 waiter、建队及清理；`__rt_mutex_slowlock()` 只是已入队 waiter 的等待循环，已经使用 `signal_pending_state(state, current)`。B 新增的 `rt_mutex_lock_killable()` 能用于普通 RT-Mutex，但和其他公开 API 一样带底层 lockdep 记账，不能直接用作 A 的外层后端。项目现有 `*_nolockdep()` 专用于带 `RT_MUTEX_PI_TAG` 的 Mutex，也不能用于 A。 |
| 原生 5.10.274 rwsem | [`kernel/locking/rwsem.c`](../kernel/locking/rwsem.c) 有独立计数标志、等待列表和读者批量唤醒（`MAX_READERS_WAKEUP`）；`__downgrade_write()` 以 release 语义原地将写改读。`down_read_non_owner()` / `up_read_non_owner()` 的协议与常规读锁不同。路线 A 保留原生类型和所有调用点，不承诺这些行为全部兼容。 |

## 2. 数据结构与状态机

独立类型暂名 `struct rwsem_pi_rtbase`：`atomic_t readers`、`struct rt_mutex gate`；仅在 `CONFIG_DEBUG_LOCK_ALLOC` 下增加外层 `dep_map`。不记录活跃读者身份，不另建队列，不动态分配。gate 是**无 tag**的普通 RT-Mutex，初始空闲；`readers` 初始为 `READER_BIAS`。两种 bias 沿用上游编码：`RB = READER_BIAS`（`1U << 31`，`atomic_t` 中为负）和 `WB = WRITER_BIAS`（`1U << 30`）。要求合法活跃读者数 `0 <= n < WB`，在每个任务最多持一次且不开放 non-owner 的契约下，正常任务数离该上限很远。

| 状态 | `readers` | gate 约束 | 进入/离开 |
| --- | --- | --- | --- |
| `OPEN(n)` | `RB + n < 0` | gate 并非一定空闲：可由正在准入的慢读者，或尚未关门／已开门待释放的写者持有 | 快读加一；拿到 gate 的写者减 `RB` 后进入 `DRAIN(n)` |
| `DRAIN(n)` | `n >= 0` 且 `< WB` | 写者持 gate，尚未取得外层写锁 | 原有读者减计数；`DRAIN(0)` 可提交为 `WRITE`。取消或 trylock 失败时加 `RB` 回到 `OPEN(n)` |
| `WRITE` | `WB` | 写者持 gate，外层写锁已取得，无活跃读者 | 写释放加 `RB - WB` 得 `OPEN(0)`；降级加 `RB - WB + 1` 得 `OPEN(1)`。两者随后释放 gate |

`OPEN → DRAIN` 的线性化点是计数上的原子减 bias，不是 gate 入队，也不是仅仅取得 gate。`DRAIN(0) → WRITE` 仅由 gate 持有者在 `wait_lock` 下提交。`WRITE → OPEN(1)` 是**一次** release RMW，不经过空闲读状态：当前写者原子地计为一个读者，其他读者从此可进入，写者仍须等它 `up_read()`。这和路线 B 的逻辑降级不同。

### 操作和取消

| 操作 | 设计 |
| --- | --- |
| `down_read` / 读 trylock | 仅在 `readers < 0` 时以 `atomic_try_cmpxchg_acquire()` 加一。与关门的 RMW 竞争时，CAS 要么先成功并被纳入排空计数，要么失败并看到入口已关。读 trylock 就在这里返回，不入队。 |
| 读慢路径 | 获取 `gate.wait_lock` 后直接调用「已持 wait_lock 的 gate 获取」；无历史版本 `readers != WB` 绕过，也不在准入注册前先放掉 `wait_lock` 再调用普通加锁。成功必须已持 gate，此时入口开放；在 `wait_lock` 下先加读计数，再放 `wait_lock`，最后释放 gate。此后读侧临界区只持外层读锁。失败则不改计数、不释放未持有的 gate。 |
| `down_write` | 先按指定 `state` 获取 gate；是普通 RT-Mutex 等待，可向 gate 持有者捐赠。之后持 `wait_lock` 关闭入口，置等待状态并以 acquire 重查是否排空；未排空时放自旋锁再 `schedule()`。重新取得自旋锁、重置等待状态并复查，直到提交 `WRITE` 或取消。此阶段 `current` 不加入 gate 队列。 |
| 写者 gate 等待取消 | 返回 `-EINTR`，由 RT-Mutex 撤销 waiter 与 PI；本写者未关门，不能更改 `readers` 或解锁别人的 gate。 |
| 写者排空期间取消 | `wait_lock` 下检查 `signal_pending_state()`；已观察到零可先完成获取，否则命中信号则将 current 恢复 `TASK_RUNNING`，以 release RMW 加 `RB`，保留尚活跃的 `n`，放 `wait_lock` 后归还 gate，外层撤销 lockdep、返回 `-EINTR`。不能直接把计数写回 `RB`。 |
| 写 trylock | 只试取 gate，不入队，失败立即返回。成功后关门，在 `wait_lock` 下尝试 `DRAIN(0) → WRITE`；仍有读者则按相同的开门协议回滚并释放 gate。不会等待排空；失败尝试可能短暂让读者转去慢路径。 |
| `up_read` | `atomic_dec_and_test()` 提供 release（实际全序）。只有从 `DRAIN(1)` 减到零的读者进入唤醒分支；其他并发退出者不负责唤醒。 |
| `up_write` / `downgrade_write` | 在 `wait_lock` 下以 release RMW 发布上述状态，先放 `wait_lock`，再走 gate 正常解锁。快读者可以在 gate 物理释放前看到已开放的入口，但此时写临界区已结束。 |

接口范围拟与原生常用部分对齐：初始化、`down/up_read`、`down/up_write`、两种 trylock、`down_read_interruptible()`、读/写 killable、写降级。仅用在 IRQ 已开启且可睡眠的任务上下文。首版不提供 `down_read_non_owner()` / `up_read_non_owner()`、递归、`nest_lock`/子类接口及原生 owner 查询语义；不新增写 interruptible 公开接口。计数协议原则上可容纳跨任务读释放，但需要独立的不记账接口、所有权契约和 I/O 完成上下文验证，不能让普通 `up_read()` 跨任务释放，也不能照搬原生调用点。持有同一实例的读者不得递归获取：关门后递归读可与等待它退出的写者构成死锁。对象必须活到所有并发调用返回，不能凭读计数归零就回收：最后一个读者仍可能在唤醒慢路径上。

## 3. 5.10.274 最小适配与锁顺序

拟增加独立头文件和 `kernel/locking/rwsem_pi_rtbase.c`。仅在 `kernel/locking/rtmutex_common.h` / `rtmutex.c` 添加准入机制确实需要的内部入口；不导出底层 helper，不依赖 `CONFIG_MUTEX_PI` 或改动 B 的公开接口。以下是接口契约，不是已实现符号：

1. **无 tag、无外层重复记账的 gate `lock_state()` / `trylock()` / `unlock()`**：用 5.10 的同一快慢路径，tag 固定为 0。不能把 tag 版 helper 用在普通 RT-Mutex 上，也不能以 `rt_mutex_timed_lock()` 代替 killable。
2. **`gate_slowlock_locked(gate, state)`**：调用方进入时本地 IRQ 已禁用且持有该 gate 的 `wait_lock`；不得另外持有 raw lock。正常返回（含 `-EINTR`）时仍持该 `wait_lock` 且 IRQ 仍关闭，成功才拥有 gate。优先从现有 `rt_mutex_slowlock()` 中提取并共用「重试、栈上 waiter 初始化及入队、等待/取消、修复 owner waiter 位」的逻辑，原公开入口仍控制原有 irqsave/restore、定时器和诊断约定。须逐条核对清理和早期返回，保留旧的加锁/解锁行为；不改变 `try_to_take_rt_mutex()` 的选择条件或 PI 链。不能直接调用现成 `rt_mutex_slowlock()`（它会重复取锁），也不能拿未入队的 waiter 直接调用 `__rt_mutex_slowlock()`。不复制第二套 RT-Mutex 慢路径，也不直接包含 `.c` 解决版本差异。
3. **最后读者唤醒**：`dec_and_test()` 命中零以后再取 `gate.wait_lock`，复查 `readers == 0`，只在确认仍为 `DRAIN(0)` 时以 5.10 `rt_mutex_common.h::rt_mutex_owner()` 取去位标记的 gate owner。设计要求关门、提交写态、回滚和开门都持 `wait_lock`；因此该谓词成立期间 gate 的写者不能脱手或退出，不依赖「取 wait_lock 就阻止任意 fast unlock」这个错误假设。用 5.10 `wake_q_add()` 在锁内排队/持引用，放锁后 `wake_up_q()`（`wake_up_process()` 使用 `TASK_NORMAL`，覆盖三种拟支持的睡眠状态）；不用 v6.12 的 `rt_wake_q_head`，也不用解锁专用的 `rt_mutex_postunlock()` 冒充排空唤醒。排空状态改变或稍晚的唤醒都以写者重查谓词为准，不能按一次 wake 判定获取成功。

慢读的检查与登记不能拆成「在 `wait_lock` 下检查，放锁，再调用普通 gate 获取」。两者之间可能发生末读者退出、原写者取得并释放写锁、下一位写者先取 gate 并再次关门，使读者在还未登记竞争时就从一轮写请求滑到下一轮。按 v6.12 从持锁入口直接尝试或登记 RT-Mutex 等待；如核心为遍历 PI 链临时放锁，则由核心维护已登记 waiter 的状态。这缩小了检查与登记的窗口，不提供 FIFO 或禁止优先级插队的保证。

锁顺序上，写者先取得 gate，再短暂拿 `wait_lock`。读慢路径在 `wait_lock` 下要求 gate，只通过核心内部入口登记竞争；该入口阻塞前会放 `wait_lock`。不在持 `wait_lock` 时调用会重新拿它的公开 API，不带 `wait_lock` 跨普通调度点。RT-Mutex 内部用 `wait_lock → task->pi_lock` 等现有协议；链遍历的重试和 trylock 规则仍由核心维护。A 不操作 `waiters`、`pi_waiters`、`pi_blocked_on` 或任务优先级；仅测试可观察这些关系。无 owner proxy 修改，无活动读者反向 PI。

### 内存序和丢唤醒防线

- 读侧取得采用 acquire CAS，读侧释放用 fully ordered `atomic_dec_and_test()`。关门拟用有序原子 RMW；writer 认定排空须 `atomic_read_acquire()` 读到零，与读者退出发布相配对。不能只因持 `wait_lock` 就假设快读者也同步于这把锁。慢读侧取得 gate 的 acquire 与队列慢路径的同步亦须保留。
- 写释放、回滚和降级用带 release 的原子加法（如 `atomic_add_return_release()` 忽略返回值）。新读者即使完全不碰 gate，也能通过 acquire 看到前一写侧的发布。仅在闭门、零读者、持 gate 与 `wait_lock` 时可把 `DRAIN(0)` 设为 `WB`；其他有读者的转换都用 RMW，避免丢失并发读者减计数。
- 排空循环必须在 `wait_lock` 下先发布睡眠状态再查计数；最后读者若已减到零，写者要么看见零而不睡，要么让醒方在同一 `wait_lock` 下看到已准备睡眠的状态。信号退出、确实拿到写锁和后续 schedule 返回都要按协议恢复/重设任务状态。对「最后退出撞上取消／trylock 回滚／新写者接管」单独验证；不把 x86 实测当作弱内存序证明。

## 4. Lockdep 与调度边界

- 外层只记一个 rwsem 类型的 `dep_map`：读用 `rwsem_acquire_read()`，写用 `rwsem_acquire()`，trylock 只在成功后登记且带 try 标记，失败的可中断获取撤销登记，`up_*` 做一次 `rwsem_release()`。降级用 `lock_downgrade()` 将外层排他记录改成共享，然后做物理转换。初始化类 key 在外层初始化调用点生成。
- 内部 gate 用无外层记账的 hooks；其 RT-Mutex 结构在调试构建可以有自己的元数据，但不把 gate 登记成跨整个临界区的第二把排他锁。底层 raw `wait_lock` 仍正常参与 lockdep。这使外层读→写和多锁顺序可以按真实的共享/排他关系检查，同时不会错误地把两位快读者当成互斥。
- **FIFO/RR：** gate 等待遵守 5.10 RT-Mutex 的现有有效优先级队列与调度策略。不会把持有者的基础 `policy` 改成等待者 policy。验证 FIFO 的提权/恢复；RR 增加独立检查，不将时间片造成的调度差异归因于 gate 公平性。
- **Deadline：** `rtmutex.c` 的 waiter 比较、`kernel/sched/core.c::rt_mutex_setprio()` 的 DL 继承机制照旧。单 gate 持有者可能继承 DL 调度参数；排空阶段仍没有向现有读者的捐赠。测试须记录调度类、`normal_prio` 和恢复，DL 需客体接纳成功并记录配置；不把一次提权等同于预算可复制或 deadline 保证。
- **公平性：** 成功关门后，新读者不能延长本次排空；若既有读者不退出，仍无等待上界。写者还可能在获得 gate 之前被高优先级 waiter 压住；重新开门后的快读者可在下个写者关门前进入。持 gate 排空的写者就算被其他 waiter 提权，也不会因此加快不可运行的读者，或替读者对抗中优先级抢占。优先级队列不是全局 FIFO，不能宣称无饥饿、读写轮转或比原生 handoff 更公平。

## 5. 验证矩阵（拟执行）

扩展 [`os-lock/rwsem_pi_test.c`](rwsem_pi_test.c) 的后端分派；保持原生和 B 用例及语义原样。以可控的握手固定角色、对象和 CPU，区分 `WAIT_GATE`（RT-Mutex 真实 waiter/owner）、`DRAIN`（gate owner、入口关闭、有活跃读者、写者还没取得外层锁）、`ACQUIRED`（API 成功返回）。诊断日志可在相应线性化点记录线程和对象，配套 `sched_switch` / `sched_pi_setprio`；不能仅凭睡眠状态分类。诊断和正式计时分开。

| 组别 | 设置与验收依据 |
| --- | --- |
| 并发读与末读者唤醒 | 至少两个读者同时持锁（握手证明两个临界区重叠），写者关门，释放一个不能取得写锁，最后一个退出后才可获取。2/4 vCPU 加并发退出，重复竞争门槛；1 vCPU 用受控交错验证。 |
| 关门拒绝新读者 | 已确认 `DRAIN(n>0)` 后启动新读者/读 trylock。读 try 返回失败，阻塞读先排到 gate；排空并写解锁后才取得外层读锁。另测关门前读取得成功并被计入排空。 |
| PI 正例和锁链 | L 已持写锁，H 申请读锁（再加 H 写竞争变体）。确认同 gate 等待边、L 实际提权、释放后还原及 H 获锁。同 CPU 加 M，对比 H 等待和 M 完成；跨 CPU 核对线程亲和。另做至少一条 `H→gate owner→第二把 RT-Mutex` 链，确认两个实际等待边及逐层恢复。 |
| PI 反例 | L 为活跃读者时 H 写等待排空，分别测一位和多位 L、同 CPU 有 M 和跨 CPU。确认 H 拥有 gate 且仍未取得写锁，各 L 没有来自 H 的捐赠。增加第三个 waiter 排在 gate 的组合，区分「向排空写者捐赠」和「活跃读者仍无捐赠」。 |
| 降级、trylock | 写→读后新读者可并发、后续写者仍阻塞到降级者退出；检查无写者插空及共享数据可见性。分别验证 gate 竞争失败、gate 空闲但活跃读者导致写 try 回滚、可成功获取时的读/写 try；回滚后原读者计数不丢，新读者可继续。 |
| 状态与信号 | 读 interruptible 在 `WAIT_GATE` 收 `SIGUSR1` 返回 `-EINTR`；读 killable、写 killable 在 `WAIT_GATE` 忽略 `SIGUSR1`、受 `SIGKILL` 取消，PI 和 waiter 状态清除。写 killable 在 `DRAIN` 也做两种信号，验关门回滚、剩余读者仍被计数、gate 无残留及后续请求能取锁。重复最后读者退出与信号并发，两种合法结果均须安全清理。无条件阻塞路径作不被信号取消的对照。 |
| 多 CPU、lockdep、压力与回归 | 非调试和 `PROVE_LOCKING`/`DEBUG_LOCK_ALLOC` 分别跑 1/2/4 vCPU。跨实例多锁按固定次序、共享读重叠、写降级与取消后验证 `debug_locks=1`。纯读、读多写少、混合和多写者有节奏及持续压力分别报告：每线程成功数、最长等待，写排他与读并发断言、退出/排空。保存警告、失效轮次和 RT 设置，不弱化原生用例；复跑 B、原生 rwsem、Mutex 和 PI-futex。弱序风险另做 LKMM 检查及条件允许的弱序 QEMU 验证。 |
| 策略与性能 | FIFO 基线，RR 和可接纳的 DL 单独跑。非调试下用同一负载、固定 CPU/临界区/总请求量，至少三组独立且轮换启动；比较 native/B/A 的读写完成率、H 请求至实际获锁的延迟、M 完成延迟、L 每线程最大等待、CPU 时间/操作。样本不够不报 P99；充分样本时按启动报告。不要用带探针或 lockdep 的计时替代正式结果。 |

## 6. 预期代价与实施顺序

预期 A 在有显著读侧重叠且未被共享计数缓存线瓶颈限制时，比 B 保留更多读吞吐；对原生 rwsem 没有先验性能优势。纯读仍会争同一原子计数。写者多一个 gate 和计数状态切换；闭门时新读者可能变成 RT-Mutex 入队/唤醒/PI 操作，缺乏原生 rwsem 的读者批量授予；最后读者也有条件唤醒成本。高优先级 gate 等待的收益可能以更长的 M 完成延迟为代价，排空期仍可能主导写请求长尾。以上均为待测假设，不能与 B 的正确性结果混用。

**待审核的实施顺序：** 先实现独立类型和最小内部入口，验证无竞争读写、真并发读、关门和末读者唤醒；再补 PI 正反例和双阶段取消；之后验证锁链、降级、跨 CPU、调试压力，最后才做性能比较。若持锁 slow helper 无法在不改变既有 RT-Mutex 行为的前提下提取，停止实现并回到设计审查。本文不引入任何内核改动；审查批准后才开始最小原型。
