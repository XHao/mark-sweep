# GC 教学 Playground 设计（gc0–gc4）

日期：2026-10-24
状态：待用户审阅
分支：`gc-teaching-playground`

## 1. 背景与目标

本仓库是 Robert Nystrom《Baby's First Garbage Collector》的示例代码（262 行 C，单文件
mark-sweep）。目标：在其基础上构建一个**教学 playground**——把现代 GC 的核心概念各实现
一个最小版本，按难度递进，代码保持教学风格（可读性优先于性能），每个阶段可独立编译运行，
相邻两文件的 diff 即为该阶段引入的算法差异。

### 成功标准

- `make test` 一条命令：5 个阶段二进制全部构建、测试全绿（macOS clang，`-std=gnu99`）
- 相邻文件 diff 一次可读完（单阶段改动聚焦）
- README 更新为五阶段路线图，读者能按文件顺序理解现代 GC 谱系

### 非目标（YAGNI）

真线程并发、标记压缩（Lisp2）、区域化（G1）、Go 混合写屏障的完整实现、指针压缩、
valgrind 强制集成。完成 gc4 后可另行追加 gc5/gc6。

## 2. 总体架构

```
mark-sweep/
├── main.c              # 原版保持不动（上游参照物，不参与构建目标）
├── gc0_baseline.c      # main.c 逐字节拷贝，diff 基准
├── gc1_iterative.c     # 显式 worklist 迭代标记
├── gc2_incremental.c   # 三色抽象 + 增量标记 + Yuasa 写屏障
├── gc3_copying.c       # Cheney 半空间复制 + bump-pointer 分配
├── gc4_generational.c  # 分代（nursery 复制晋升 + old 标记清除）
├── Makefile            # all / test / clean / run
└── README.md           # 路线图 + 概念导读
```

约定：

- C99，零外部依赖，沿用原版代码风格（命名、注释密度、printf 输出风格）
- 每个 `gcN.c` 头部注释块：本阶段概念、与上一版的 diff 看点、对应现代 GC 机制
- gc0 之后自定义 `assert` 改名 `check`（消除对标准库的同名遮蔽），其余风格不变
- 根集合 = VM 栈；测试代码中 C 局部变量持有的指针**不是根**（与原版语义一致，
  gc3 起移动收集会使其失效，测试需适配，见 §5）

## 3. 阶段设计

### gc0_baseline.c

`main.c` 的逐字节拷贝（仅文件头加注释块）。保留原版 5 个测试。

### gc1_iterative.c — 迭代标记

- `mark()` 递归改为显式 worklist：**动态增长数组**（`realloc` 翻倍），封装
  `workPush` / `workPop`。不用定长数组：深链测试下 worklist 深度可达 O(n)，
  定长会重蹈爆栈覆辙
- `markAll`：根全部入栈后循环弹出、标记、子节点入栈
- 行为与 gc0 完全一致；diff 看点 = `mark()` 一个函数的改造
- 新增测试：10 万层深 pair 链，断言标记正常完成（原版递归会爆栈）

### gc2_incremental.c — 三色 + 增量标记 + Yuasa 写屏障

**三色**：`marked` 升级为 `color`（`WHITE / GRAY / BLACK` 枚举）；灰栈 = worklist
（沿用 gc1 的动态数组）。

**增量模型**（确定性模拟并发）：VM 增加 `gcState`（`IDLE` / `MARKING`）。
`numObjects >= maxObjects` 时进入 MARKING（根置灰入灰栈，**不**一次跑完）；
此后每次 mutator 操作（分配、字段写）驱动 `gcStep()`：从灰栈弹出一个对象，
置黑，其子节点置灰入栈。灰栈清空 → 原子 sweep（单趟遍历对象链：白→free，
黑→重置为白供下轮），回 IDLE，`maxObjects` 沿用 gc0 的 ×2 自适应（下限
`INIT_OBJ_NUM_MAX`）。sweep 保持原子并在注释中说明：这是 mark-sweep 的停顿残余，
正是 gc3/gc4 要解决的问题。

**Yuasa 快照屏障，三条规则**（不变式一句话可证：GC 开始时刻的可达对象——
"快照"——本轮全部存活，代价是产生浮动垃圾）：

1. 字段覆盖写 → shade **旧值**（WHITE 则置灰入灰栈），再写入新值
2. 根弹出（`pop()`）→ shade 弹出值。快照语义要求根的删除也被覆盖，否则
   "pop 后移入已黑对象"的目标会被误回收
3. MARKING 期间分配的新对象**出生即黑**（本轮作为浮动垃圾存活）

规则 1 的动机注释中对照 Dijkstra 插入屏障（黑不指白）与 Go 混合屏障（= 两者之和）。

**受控写 API**：原版测试的 `a->tail = b` 裸写会绕过屏障。gc2 提供
`objectSetHead(vm, obj, val)` / `objectSetTail(vm, obj, val)`，测试改用它。
教学点：真实 VM 由编译器/解释器插桩屏障，玩具中手动插。

**杀手测试**：MARKING 进行中覆盖某 pair 的唯一引用字段——无屏障则旧目标被误回收。
提供 `#ifdef BARRIER_OFF` 编译版（屏障退化为裸写），预期其杀手测试失败。
`make test` 运行该编译版并断言"该失败的确实失败"（非零退出码记为通过）。

### gc3_copying.c — Cheney 半空间复制 + bump 分配

- **堆**：`Object heap[2][HEAP_MAX]`（建议 `HEAP_MAX = 4096`）+ `free` 游标。
  分配 = `&from[free++]` 初始化，O(1)。逐对象 `malloc`、`next` 链表、
  markAll/sweep 全部删除
- **转发**：union 增加 `Object* forward` 成员。对象复制到 to-space 后，
  from-space 旧副本（已死）原地置 `OBJ_FORWARDED`（枚举新增值）+ 写入新地址，
  省掉旁表
- **收集（广度优先）**：交换 from/to → 逐个复制根，**栈槽位就地改写为新地址** →
  `scan` 从 to-space 头部推进：PAIR 的 head/tail 逐个经 `copyObject()` 转发并改写 →
  `scan == free` 结束。无单独 mark 阶段——存活即被复制
- **容量**：from-space 满即 `check` 失败（"Heap overflow"）。注释说明代价：
  堆利用率 ≤50%、容量固定
- **测试适配**：GC 后 C 变量指针全部失效，测试改为 GC 后从 VM 栈重新取指针。
  注释点明：真实系统中这是编译器栈图 / handle 存在的原因
- 新增测试：GC 后对象地址改变（证明是 moving GC）、深链复制后内容完整保留、
  容量上限触发 check

### gc4_generational.c — 分代收集

叙事结构：**gc4 = gc3 的 nursery + gc0 的 old gen，用写屏障粘起来**。

- **nursery**：单块 arena（`Object nursery[NURSERY_MAX]`，建议 256）+ bump 分配。
  比 gc3 更简：晋升目标就是老年代，不需要双空间与转发
- **old gen**：逐对象 `malloc` + 侵入式链表 + 标记清除，与 gc0 逐字相同；
  `oldMax` 沿用 ×2 自适应
- **分配**：一律进 nursery
- **Minor GC**（nursery 满时触发）：gc1 式迭代标记，起点 = 根 + remembered set
  成员；沿途到达的年轻对象**首次存活即晋升**（malloc 复制进 old 链表），
  到达的老对象仅作遍历通道（标记位在 minor 结束时全部清除，不影响老年代收集）；
  最后 nursery 游标归零、remset 与 remembered 标志清空。注释：真实 JVM 有
  年龄阈值与 survivor 空间，此处教学从简
- **写屏障 → remembered set**：向**老年代对象**写入指向**年轻对象**的引用时，
  若该老对象未记录（`remembered` 标志字节），压入 remset。年轻代内部写与
  老指老引用无需记录。代际归属用指针范围判断（nursery 是连续数组），
  无需额外字段
- **Major GC**（minor 后 `oldCount >= oldMax` 时触发）：nursery 存活者全部晋升，
  然后对 old 链表做 gc1 式迭代标记清扫
- **可观测性**：统计并输出 minor / major 次数、晋升对象数。~~perfTest 跑完可
  直观看到分代假设生效（major 次数远少于 gc3 的收集次数）~~
  **修订（2026-10-24，终审后）**：该验收信号在"首次存活即晋升"策略 +
  原 perfTest 纯 churn 负载下不可达（实测 78 minor / 32 major / 636 晋升——
  每次 nursery 填满时恰在栈上的短命对象被晋升，污染老年代）。交付版本保留
  原负载与真实数字，在 perfTest 注释中说明原因；年龄阈值/survivor 空间
  列为后续可选扩展（正是注释要教的那一课）
- **杀手测试**：年轻对象仅被老年代对象引用，minor GC 后必须存活（已晋升）。
  同 gc2 提供 `#ifdef REMSET_OFF` 预期失败编译版，`make test` 验证其确实失败
  （用内容断言检测，避免未定义行为）

## 4. 测试与验收

| 阶段 | 测试内容 |
|---|---|
| gc0 | 原版 5 测试逐字保留（栈上保留 / 不可达回收 / 嵌套可达 / 循环引用 / 压力） |
| gc1 | gc0 的 5 + 10 万层深链不爆栈 |
| gc2 | gc0 的 5（字段写改走 barrier API）+ 增量交错正确性 + 杀手测试；`BARRIER_OFF` 版预期失败 |
| gc3 | 核心场景（GC 后从栈重取指针）+ 地址改变 / 内容保留 / 容量上限 |
| gc4 | 核心场景 + minor 不动老年代 + 晋升计数 + remset 救回；`REMSET_OFF` 版预期失败 |

- `make test`：构建并运行全部 5 个二进制 + 2 个预期失败的屏障关闭版；
  每阶段输出一行总结（PASS / EXPECTED-FAIL）
- `make all`（默认）只构建；`make clean` 清理；`make run` 保留原版行为
- 验收：macOS clang `-std=gnu99` 下 `make test` 全绿；README 含路线图与每阶段概念导读

## 5. 关键设计决策记录

| 决策 | 选择 | 理由 |
|---|---|---|
| 文件组织 | 每阶段独立 .c | 并排 diff 即算法差异，教学价值最大；样板重复可接受 |
| 写屏障 | Yuasa 快照（实现）+ 注释对照 Dijkstra / Go 混合 | 不变式一句话可证，规则最少 |
| 增量模拟 | 单线程确定性交错（mutator 一步 / GC 一步） | 可复现、可测试；并发标记的单线程精确模拟 |
| gc1 worklist | realloc 动态数组 | 深链下定长数组同样溢出 |
| gc3 堆 | 定长双 arena + 转发指针存于旧副本 | 真实体现 bump 分配与 Cheney 扫描；无旁表 |
| gc4 晋升 | 首次存活即晋升，单空间 nursery | 本课教分代假设而非复制本身，diff 最小 |
| 代际判断 | 指针范围比较 | nursery 连续数组，零字段开销 |
| 根集合 | 仅 VM 栈；C 局部变量非根 | 与原版语义一致；gc3 的失效问题在测试内解决 |
| `assert` | gc0 后改名 `check` | 消除标准库同名遮蔽 |

## 6. 风险与已知限制

- gc2 的灰栈与 gc1 的 worklist 需动态增长；增长失败（OOM）按 `check` 处理
- gc3 / gc4 堆容量固定，压力测试参数需按容量调校（不触发 Heap overflow 的前提下
  覆盖目标行为）
- 预期失败测试依赖确定性（单线程模型保证）
- `make run` 的 valgrind 在 macOS 上通常不可用，保持现状不强制
