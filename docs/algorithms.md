# 十阶段 GC 算法实现讲解

本文按构建顺序讲解本仓库十个收集器的**算法本身**：每一阶段解决前一阶段留下的什么问题、核心思想如何落成最小实现、哪些测试锚定了正确性。每节配 `file:line` 引用，可直接跳进源码对照阅读。

阅读建议：先读各文件头部注释（算法自述），再按 README 的 diff 命令看相邻差异，最后回到本文看"为什么"。测试是最诚实的文档——每节的"反证"小节告诉你：**把算法的某一半拆掉，哪个测试会以什么方式失败**。

```
gc0 mark-sweep ──递归爆栈──▶ gc1 worklist ──原子sweep停顿──▶ gc2 三色+屏障
                                                                  │
        碎片+malloc开销 ◀── mark/sweep 双双被换掉 ──────────────────┤
        │                                                          │
        ▼                                                          ▼
gc3 Cheney复制 ──50%空间浪费──▶ gc5 Lisp2压缩        gc2 单屏障 ──栈重扫故事──▶ gc6 混合屏障
        │                                                             │
        ├─组合与调度──▶ gc7 区域化（G1：按区收集 + Garbage-First）      │
        └─惰性治愈────▶ gc8 染色指针 + 读屏障（ZGC：对象先搬、指针后修）◀┘
        │
        └─弱分代假设─▶ gc4 分代（gc3 的 nursery + gc0 的老年代）
                        └─首次晋升之弊─▶ gc4s 老化（survivor 空间 + 年龄阈值）
```

---

## gc0 — 基线：mark-sweep 三部曲（`gc0_baseline.c`）

**问题**：手动管理内存的全部痛点（泄漏、悬垂）——这是起点而非缺陷。

**核心思想**：对象图从**根集合**（root set）出发可达即为存活。收集分三步：

1. **标记**（`mark`，gc0_baseline.c:85）：从每个根递归标记，遇已标记即返回——这一行早退正是循环引用的终结者（test4 的环由它存活）。
2. **清除**（`sweep`，gc0_baseline.c:105）：沿侵入式链表扫全堆，未标记者摘链 `free`，已标记者清零标记位供下轮。二级指针 `Object** object = &vm->firstObject` 让摘链无需 prev 指针。
3. **自适应阈值**（`gc`，gc0_baseline.c:126）：`maxObjects = 存活数 × 2`（gc0_baseline.c:132）——堆越大触发越懒，把收集成本摊进分配。

**根集合约定**（全仓库共用）：根 = VM 栈；C 局部变量**不是**根。这条契约在 gc3（移动）与 gc6 test9（屏障剃刀边缘）处成为主角。

**留给后面的问题**：递归 `mark` 每层链一个 C 栈帧（→ gc1）；sweep 整体是原子停顿（→ gc2）；free 造成的碎片与逐对象 malloc 开销（→ gc3）。

---

## gc1 — 迭代标记：显式 worklist（`gc1_iterative.c`）

**问题**：百万层 pair 链 = 百万个 C 栈帧。基线实测：test5（gc1_iterative.c:276）构建 1,000,000 深链时原递归版以 exit 139（SIGSEGV）爆栈。

**核心思想**：把递归隐式使用的调用栈换成**显式的、动态增长的灰栈**（worklist）——`workPush/workPop`（gc1_iterative.c:93/102，realloc 翻倍）。`markAll`（gc1_iterative.c:106）的循环体就是递归体的逐字翻译：弹出一个、查标记位、标记、压入孩子。环仍由"已标记即跳过"终结。

**为什么定长数组不行**：链式深图的 worklist 深度是 O(图深)——深链照样打爆 256 槽的定长数组，必须动态增长（这是 spec 时纠正过的一个设计错误）。

**现代对应**：所有生产收集器都用显式标记栈；递归在"堆是任意敌意图"的前提下不可用。

---

## gc2 — 三色 + 增量标记 + Yuasa 屏障（`gc2_incremental.c`）

**问题**：gc1 只是把标记搬出了 C 栈，收集仍然整体停顿。真实收集器（Go、JVM）把标记**摊进** mutator。

**三色抽象**：`Color`（gc2_incremental.c:44）——WHITE 未达、GRAY 已达待扫、BLACK 已扫尽。灰栈 = gc1 的 worklist 搬进 VM（`grayStack`）。

**lockstep 交错**（确定性的并发模拟）：GC 是 VM 里的状态机（`GcState`，gc2_incremental.c:50）。`numObjects >= maxObjects` 时 `gcStart`（gc2_incremental.c:138）只做一件事——把根全部置灰，**不**跑完。此后每次 mutator 操作（分配、屏障化字段写）驱动**一步** `gcStep`（gc2_incremental.c:147）：弹一个灰对象置黑、孩子置灰。灰栈清空 → 原子 sweep → IDLE。单线程的逐步交错，就是并发标记的精确可复现模拟——屏障是 mutator 与收集器之间唯一的契约。

**Yuasa 快照屏障，三条规则**（不变式一句话：**GC 开始时刻的可达对象，本轮全部存活**；代价是浮动垃圾）：

1. **字段覆盖写 → shade 旧值**（gc2_incremental.c:272）：删掉的快照路径必须保活
2. **根弹出 → shade 弹出值**（`pop`，gc2_incremental.c:198）：根删除同属快照删除
3. **标记期间分配 → 出生即黑**（gc2_incremental.c:211 的 `newObject`）：新对象做本轮浮动垃圾

三处 shade 全部套着 `if (vm->gcState == GC_MARKING)` 守卫——idle 期 shade 会把对象种进灰栈、污染下一轮。这个守卫是执行期抓到的真 bug（test2 曾因此失败）。

**反证**：`gc2_bo`（`-DBARRIER_OFF`）把字段写屏障退化为裸写，死于 test7（gc2_incremental.c:415）——"覆盖唯一引用字段后重装指针，快照对象被误回收"，断言只读计数不 deref，无 UB。

**残留问题**：sweep 仍是原子的——这正是 gc3/gc4 出场的原因；同时单屏障在"未屏障栈写"语境下有完整故事（→ gc6）。

---

## gc3 — Cheney 半空间复制（`gc3_copying.c`）

**问题**：gc0 的 malloc/free 堆碎片化，且逐对象分配昂贵。

**核心思想**：不标记、不清扫——**活着的搬走，死的原地蒸发**。堆是两块连续 arena（`heap[2]`，各 4096 槽），分配是指针碰撞（bump）：`&heap[space][freeCount++]`（gc3_copying.c:147）——O(1)、无 free list，这正是 JVM young gen / TLAB 的分配方式。

**Cheney 扫描**（gc3_copying.c:100/118）：

```
gc:  toCount = 0
     ① 复制根，栈槽位就地改写为新地址
     ② scan 从 to-space 头部推进：
        scan ──▶│p3 │i1 │i2 │p1 │…│◀── free(toCount)
        每个 pair 的 head/tail 经 copyObject 转发并改写
     ③ scan == toCount 结束，交换 space
```

**转发指针存在尸体里**：对象首次被复制后，from-space 旧副本已死，原地改写 `type = OBJ_FORWARDED` + union 里放新地址（gc3_copying.c:100 的 `copyObject`）——零旁表。环免费解决：第二次遇到同一对象，命中转发直接拿新地址（test4 的环靠它）。

**移动的代价**：根必须就地重写（根槽位在 ① 里改）；**C 变量持有的指针 GC 后全部失效**——真实系统用栈图/handles 解决，测试统一"GC 后从栈重取"。

**反证**：`gc3_overflow`（`-DOVERFLOW_TEST`）证明容量上限真的会被 `check("Heap overflow!")` 拦住（注意测试必须用链式构建——平坦 pushInt 会先撞 `STACK_MAX` 256，这个坑本身修过一次）。

**残留问题**：堆利用率 ≤50%、容量固定；以及"活过一轮就死"的对象没有便宜的去处（→ gc4、gc5 两条路）。

---

## gc4 — 分代：弱分代假设 + remembered set（`gc4_generational.c`）

**问题**：绝大多数对象朝生暮死，却和百岁对象付同样的全堆收集税。

**弱分代假设**：按年龄分而治之。本实现 = **gc3 的 nursery + gc0 的老年代，用写屏障粘合**：

- **nursery**：单块 256 槽 bump arena（gc4_generational.c:99 的 `newVM`）——比 gc3 更简，晋升目的地就是老年代，连双空间都省了
- **老年代**：逐对象 malloc + 链表 + gc1 式迭代标记清扫（`majorGC`，gc4_generational.c:248），与 gc0 逐字同源
- **Minor GC**（gc4_generational.c:288）：`promoteAll`（gc4_generational.c:165）用**槽位 worklist**（`Slotlist`，存 `Object**` 而非 `Object*`）遍历——从根与 remset 出发，年轻对象首次存活即晋升：malloc 复制进老年代链表、nursery 旧副本原地变转发 husk（gc3 的技巧复用）、所有引用槽位就地改写。**死掉的年轻对象成本为零**——不扫、不free、游标归零。这就是分代的全部红利。

**写屏障 → remembered set**（gc4_generational.c:326/338）：老年代对象写入年轻引用时记录之（`remembered` 标志去重）。minor GC 把 remset 成员当额外根重扫。存**对象**而非**槽位**是教学简化（注释里写明了真实收集器付什么代价）。

**代际归属零开销**：`isYoung`（gc4_generational.c:130）用指针范围比较（nursery 是连续数组），无需额外字段；显式判 NULL 防 NULL 落进范围。

**反证**：`gc4_bo`（`-DREMSET_OFF`）死于 test5（gc4_generational.c:460）——仅被老年代引用的年轻对象在 minor 后被丢；利用"nursery 游标归零但内存未释放"的窗口，用槽位复用让丢失**按值可观测**，零 UB。

**诚实的故事**：perfTest 统计 78 minor / 32 major / 636 晋升——首次存活即晋升让"恰好在场的短命对象"晋升污染老年代，major 频发。这不是 bug，是**没有年龄阈值**的必然后果（真实 JVM 加 survivor 空间正是为此），注释与 spec 修订里都有说明。

---

## gc4s — 老化迭代：survivor 空间 + 年龄阈值（`gc4_survivor.c`）

**问题**：上一节"诚实的故事"不是注释修辞——78 minor / 32 major / 636 晋升是
首次存活即晋升在 churn 负载下的必然结局：每次 nursery 填满时恰在栈上的短命
对象全体被授予终身，老年代被污染。gc4 原地保留这个第一刀；本文件是修正，
`diff gc4_generational.c gc4_survivor.c` 就是全部老化增量。

**机制**（HotSpot 同构的最小版）：

- **双空间 nursery**：eden 与 survivor 每次 minor 后互换角色——gc3 的空间
  互换技巧用在年轻代内部。平时只有 eden 活着（分配照旧 bump，
  gc4_survivor.c:336）
- **老化晋升**：`Object` 加 1 字节 `age`；复制点分流
  （`copyYoung`，gc4_survivor.c:172）——`age + 1 >= AGE_MAX`（=2）则 malloc
  晋升老年代，否则复制进 survivor、`age+1` 继续年轻。出生 0 → 存活一轮 1 →
  第二轮晋升
- **remset 按边的存续保留**（gc4_survivor.c:239）——这是执行期抓到的真缺陷：
  gc4 可以每个 minor 无条件清空 remembered set，因为立即晋升使 old→young 边
  活不过一次收集；老化后边可以跨多个 minor 存续，无条件清空会在下一跳 minor
  **静默丢弃**仅经老年代可达的年轻对象（t5 的晋升计数断言咬住了它）。修复：
  收集末尾压缩 remset——仍指年轻者留、否则移除并清 `remembered`

**头条与数字的诚实**：同负载下 **83 minor / 0 major / 0 晋升**（gc4 为
78/32/636）。minor 反而多 5 次：存活者占据 eden 尾部，每轮净容量略降——这个
漂移本身写着"为什么 HotSpot 有独立的 survivor 空间"。perfTest 只**断言两个
零**（gc4_survivor.c:625）：minor 数会随实现细节漂移，"老年代零污染"才是
分代假设要兑现的东西。t1（gc4_survivor.c:433）两行断言讲完年龄语义：一次
minor 后仍年轻、两次后晋升。

**没有失败构建**：老化没有可关闭的屏障语义，行为由 t1/t6 的精确计数与
perfTest 的零断言直接锚定。

---

## gc5 — Lisp2 标记-压缩：滑动三趟（`gc5_compact.c`）

**问题**：gc3 用一半堆换无碎片。能不能 100% 堆、也无碎片？能——代价是多趟。

**Lisp2 三趟**（`gc`，gc5_compact.c:147，标记在其前）：

```
堆（单块 8192 槽 = gc3 的总内存）：
标记后:   │L1 │✝ │L2 │L3 │✝ │✝ │L4 │  (L=活 ✝=死)
Pass1:    给每个活者按下标顺序派发新址 → forward 字段
Pass2:    改写一切引用（栈根 + 活 pair 的字段）→ forward
Pass3:    逐个下滑 *dest = *object（dest ≤ src，升序无前向覆盖）
结果:     │L1 │L2 │L3 │L4 │·············空闲······
```

- **`forward` 是专设字段**（gc5_compact.c:52）而非 union 复用：Pass 2 要一边读旧字段一边改指针，旧存储不能被覆写。真实 JVM 位打包进 mark word 或用 Brooks 指针；教学版付 8 字节买清晰。
- **保序滑动**是可断言指纹：test7（gc5_compact.c:353）断言存活根地址严格升序。
- **不变式**：Pass 1 的 `newCount` 单调递增 ⇒ `dest ≤ src` 恒成立 ⇒ Pass 3 升序扫描永不覆盖未读源。

**反证（变异级）**：test7 的三轮收集中，第三轮"两条链死亡"专钉**标记清理**——终审时真的删掉 `object->marked = 0;` 做过变异验证：变异体死于 "Dead chains must not leak across rounds."。另一个教训写在测试里：**freeVM 触发的 gc 根集为空，不构成跨轮测试**（gc5 终审的重要发现）。

**成本诚实账**：Pass 1/3 扫**全占用区间**而非活集——压缩是 O(堆占用)，这正是区域化/分代堆存在的原因（gc4 注释与此呼应）。

---

## gc6 — Go 式混合写屏障（`gc6_hybrid.c`）

**问题**：gc2 的单屏障故事不完整。Go 1.8 为什么要**两个半边一起上**？

**混合屏障**（gc6_hybrid.c:264/288，映射 Go 提案 17503 的伪代码）：

```
shade(*slot);              // 删除半边—— 覆盖写时保活被顶掉的旧值
if current stack is grey:  // 插入半边—— 写入黑对象的白引用必须着色
  shade(ptr);
*slot = ptr;
```

Go 以**写方 goroutine 的栈色**为条件；本玩具没有 per-mutator 栈色，故取经典 Dijkstra 目标形式：`if (pair->color == COLOR_BLACK) shade(value)`。取舍的实质：纯插入屏障下，未屏障的栈写要求标记末尾 **STW 重扫所有栈**；混合屏障用一点浮动垃圾把重扫整个删掉。

**`pushRoot`**（gc6_hybrid.c:311）：不 shade、不驱动步——故意模拟**未屏障栈写**（goroutine 栈的真实待遇）。

**屏障矩阵**——两个半边各有专属杀手与专属失败构建：

| 构建 | 屏障形态 | 杀手测试 | 失败实证 |
|---|---|---|---|
| `gc6_hybrid`（默认） | 混合 | t8、t9 均过 | — |
| `gc6_dijkstra` | 仅插入（pre-1.8 Go） | t8（gc6_hybrid.c:472）：未屏障根写 + 路径删除 → 已入根对象被回收 | 2 remaining |
| `gc6_yuasa` | 仅删除 | t9（gc6_hybrid.c:496）：idle 期弹出的无路径白对象 `pushRoot` 后存入**黑** pair → 只有插入半边能救 | 3 remaining |

**精确的等价范围**：在 gc2/gc6 测试集内 hybrid 与纯 Yuasa 行为等价（gcStart 预灰根 = 起点栈快照，模型从不需要重扫）；但等价**不是**全模型的——t9 的序列就是反例（spec §1 的修订记录了这个边界）。

---

## gc7 — 区域化：G1 的组合与调度（`gc7_regions.c`）

**问题**：前面八个收集器各有一个大杠杆；G1 的洞见是**组合与调度**——把堆切成
等大 region，按区收集、按脏乱程度排序、每停顿只做有预算的量。

**三课**（每课都有断言锚定）：

1. **堆 = 区域表**（16×512）：分配在 eden 区内 bump，**满了换区不触发 GC**
   （gc3/gc4 是"分配即触发"）——直到空闲区 <2 才走收集梯子（`ensureEden`，
   gc7_regions.c:424）。t5 前半：1535 对象跨 3 区，零收集
2. **逐区疏散**：收集一组区 = 把它们的活对象**作为单一闭包**复制进其他区并
   改写入边——入边来自栈根（每次停顿重扫）与**槽位级 per-region RSet**
   （写屏障记录跨区指针的槽位地址——gc4s"记槽位"教训的落地形态）。
   复制体的跨区字段必须**重新登记**进目标区 RSet（旧槽位随旧对象死亡）。
   t8 是杀手：仅经老区 pair 引用的年轻对象靠 RSet 存活，`gc7_bo`
   （`-DRSET_OFF`）经槽位复用按值检测丢失
3. **Garbage-First**：census（gc7_regions.c:210，gc1 式 worklist）统计各区
   活度；mixed 停顿**先挑最脏的老区**、受 `CSET_BUDGET` 与空间预算双重约束
   （gc7_regions.c:385）。t6 指纹：脏区 255 活对象搬走、满区 512 纹丝不动；
   t7：每次 mixed 恰好有界

**纯死区零成本回收**：census 后 `live == 0` 的区直接退役（freeDeadRegions，
gc7_regions.c:338）——G1 在标记周期里回收空区的同构物，也是 t5 巨链能在一次
young 里释放 13 个区的机制。

**终审揭出的三层教训**（本文件最值得读的一段，全部有推演+插桩证据）：
最初的逐区疏散把 RSet 条目一律当根，t5 的 young 为 599 活对象搬了 7168 个
（死主人留下的陈旧条目拖动整条垃圾链）。修复分三层，每层揭开下一层：

1. **cset 过滤**暴露**次序缺陷**——升序逐区疏散会丢"只被后疏散区引用"的对象
   → 改为 cset 整体闭包（gc7_regions.c:265）
2. **整体闭包**暴露**目标区供给**问题——闭包结束才释放区，而 599 活需要 2
   个目标区 → census 进 young + 纯死区即时回收 + 前置目标预算（诚实的
   "Heap exhausted"规则，`gc7_overflow` 预期失败构建钉住）
3. **陈旧种子仍在拉垃圾**（插桩：census 599 vs 搬移 601）→ **标记位贯穿停顿**
   ：census 只统计不清标记，疏散弹栈时未标记者直接丢弃——死主人的边必然
   指向未标记者，这是"RSet 条目只是提示、标记才是真相"的教科书结论

最终 t5 断言 `objectsMoved == 599`：**复制成本重新跟住活集**——这条断言的
三层修复史本身就是区域收集器工程难点的微缩教材。

---

## gc8 — 染色指针 + 读屏障：ZGC 的惰性治愈（`gc8_zgc.c`）

**问题**：gc3 的停顿在 mutator 恢复前**改写一切指针**；ZGC 把它反转——**对象
先搬走，指针以后再修**：读屏障在每次引用加载时惰性治愈，搬迁因此能与 mutator
并存（低延迟谱系的根基）。

**三机制**（全部有断言锚定）：

1. **指针染色**（gc8_zgc.c:142）：堆字段引用存储时带 **bit0 视图位**（8 对齐
   腾出的空位）；每轮 relocate 翻转视图——停顿后，复制体里的全部旧字段**一次
   比较即判陈旧**，零查表。t5 直接断言"字段颜色仍陈旧"（染色本身可观测）
2. **读屏障 `loadRef`**（gc8_zgc.c:157）：字段读取的唯一通道（对应真实系统
   编译器对每次引用加载插桩）。快路径 = 颜色比较；慢路径 = 经转发表就地治愈
   （改写槽位 + 刷当前色 + 计数）。**双检查的必要性**（t5 剃刀情况）：把 C 局部
   持有的旧空间地址存入字段时带的是**当前色**——颜色比较放行，只有 husk 检查
   能救
3. **惰性搬迁**（gc8_zgc.c:203）：停顿复制活对象、只**就地治愈栈根**（ZGC 的
   STW 根阶段同构），**堆字段一律不改写**（与 gc3 的语义分水岭）；旧空间整个
   充当转发表**保留到下一轮停顿开头**的治愈扫（`healAll`，gc8_zgc.c:193——
   ZGC 按页保留的同构物）

**治愈纪律循环往复**（t6 的课）：每一轮翻转都会把上一轮存活者的字段**再次**
判陈旧——治愈扫 `==2`、读后 `==4` 的断言顺序把这个循环本身钉成了可观测事实。
**单停顿有效窗口**（头注释明示）：C 局部变量的可治愈性只到**下一停顿为止**
——旧空间复用后连 husk 检查都会读到新住户；每停顿后重取指针（gc3 契约延续）。

**反证**：`gc8_bo`（`-DLOAD_BARRIER_OFF`）把 loadRef 退化为裸读，死于 t5 的
值检查——返回的 husk 之 union 读作指针位，确定性 ≠ 42，有效内存零 UB。

**诚实注记**：真 ZGC 的颜色在 64 位指针高位（Marked0/1/Remapped）、治愈经全局
转发表无需类型检查、搬迁真并发、旧页按页惰性回收——本玩具分别用 bit0、husk
类型检查、单停顿、停顿首治愈扫替代，惰性治愈纪律不变。

---

## 十阶段横向对比

| | gc0/基线 | gc1 | gc2 | gc3 | gc4 | gc4s | gc5 | gc6 | gc7 | gc8 |
|---|---|---|---|---|---|---|---|---|---|---|
| 收集停顿 | 全程 STW | 全程 STW | 标记摊入 mutator，sweep 原子 | STW 但只碰活对象 | minor 便宜 / major 全堆 | 同 gc4，churn 零 major | STW，O(占用) | 同 gc2 | 每停顿有界（CSET 预算）| 停顿只碰根，读时治愈 |
| 分配 | malloc | malloc | malloc | **bump O(1)** | nursery bump | nursery bump | bump O(1) | malloc | 区内 bump，满区换新 | bump O(1) |
| 空间利用 | 碎片化 | 碎片化 | 碎片化 | **≤50%** | 分代各计 | 分代各计 | **100%** | 碎片化 | 按区回收，整区复用 | ≤50%（同 gc3）|
| 对象顺序 | 稳定 | 稳定 | 稳定 | BFS 重排 | 晋升重排 | 晋升重排 | **保序滑动** | 稳定 | 被收集区重排，其余不动 | 复制重排 |
| 每次收集成本 | O(堆) | O(堆) | 活集 + 摊销 | O(活集) | minor O(nursery) | minor O(nursery) | O(占用) | 活集 + 摊销 | 活集（标记真相制）| 活集 + 治愈摊销 |
| 屏障 | 无 | 无 | Yuasa 删除 | 无 | 代际 remset | 代际 remset（按边存续） | 无 | 混合两半 | 跨区槽位 RSet | 读屏障（染色+husk）|
| 失败构建 | — | — | `gc2_bo` | `gc3_overflow` | `gc4_bo` | —（零断言直钉） | — | `gc6_dijkstra` / `gc6_yuasa` | `gc7_bo` / `gc7_overflow` | `gc8_bo` |

生产系统是这些原语的**组合**：JVM young gen = gc3+gc4；G1 = gc4+gc5 的区域化变体 + gc2 式并发标记；Go = gc2 的交错 + gc6 的混合屏障 + gc5 式的整理演进。

## 术语对照

| 术语 | 首现 | 一句话 |
|---|---|---|
| root set 根集合 | gc0 | 可达性判定的出发点（本仓库 = VM 栈） |
| worklist / gray stack 灰栈 | gc1 | 显式标记工作队列，递归的可迭代替代 |
| tricolor 三色 | gc2 | WHITE/GRAY/BLACK 的抽象不变式框架 |
| floating garbage 浮动垃圾 | gc2 | 快照保活导致的"多活一轮"的死者 |
| snapshot-at-the-beginning | gc2 | Yuasa 屏障的不变式：起点快照全存活 |
| semispace 半空间 | gc3 | 复制收集的两块等分堆 |
| forwarding pointer 转发指针 | gc3 | 旧地址 → 新地址的映射（存在尸体里） |
| bump allocation 指针碰撞 | gc3 | 游标递增的 O(1) 分配 |
| weak generational hypothesis | gc4 | "多数对象朝生暮死"的经验假设 |
| remembered set | gc4 | 记录跨代引用的额外根集合 |
| sliding compaction 滑动压缩 | gc5 | 保序地把存活者挤到堆头 |
| hybrid write barrier | gc6 | 插入 + 删除两半并用的写屏障（Go 1.8+） |
| age threshold / tenuring | gc4s | 存活 N 次收集后才晋升老年代（HotSpot 同构） |
| survivor space | gc4s | 每次 minor 与 eden 互换角色的年轻代第二空间 |
| region / collection set | gc7 | 等大堆区 / 单次停顿决定疏散的区集合 |
| garbage-first | gc7 | 按垃圾密度优先选择收集目标（G1 之名） |
| colored pointer 染色指针 | gc8 | 颜色编码在指针自身的位上（ZGC 高位 / 本玩具 bit0）|
| load barrier 读屏障 | gc8 | 引用加载时检查并治愈陈旧指针的屏障 |
| self-healing 自愈 | gc8 | 陈旧指针在后续读取点被就地修复的纪律 |
