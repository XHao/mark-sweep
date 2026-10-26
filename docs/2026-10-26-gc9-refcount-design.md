# gc9 — 朴素引用计数设计

日期：2026-10-26
状态：待用户审阅
分支：`gc9-refcount`
父 spec：`docs/2026-10-24-gc-teaching-playground-design.md`（约定与风格继承）

## 1. 背景与目标

前十阶段全是 **tracing** 范式（从根出发找活的）；引用计数（RC）是对偶——
**对象知道自己被引用几次**，计数归零即死即回收，无收集循环、无停顿、天然
增量。这是 Swift/ObjC/Python/COM 的日常。本阶段教朴素形态，并**把它最著名的
缺陷钉成断言**：

1. **没有 gc() 函数**：释放随 `release` 级联即时发生——与 gc0"攒到阈值再收"
   的直接对照；malloc/free 逐对象（gc0 堆形态），成本模型诚实
2. **栈即强引用**：push = retain、pop = release；字段写走三步曲
   （见 §3）——**顺序本身就是课**
3. **环杀手（反向形态）**：a↔b 互持计数、丢根后双双泄漏——
   `check(liveObjects == 2, "环在朴素 RC 下泄漏")`。测试以**钉住缺陷**为通过，
   注释指向 gc10（混合修复）；随后用公开惯用语**手动断环**清零——ARC 程序员
   的日常操作。与 gc0 test4"mark-sweep 天然处理环"构成范式对偶的首尾呼应
4. **成本统计**：`retain+release` 操作计数——perfTest 量化"范式把成本摊进
   每次写"，对比 tracing 的 mutator 零计数写

成功标准：`make test` 全绿（**19/19**：11 PASS + 8 预期失败）；`make asan`
11/11（零泄漏零 UB）；t5 的"泄漏→断环→清零"三段断言成立。

## 2. 文件与集成

```
gc9_refcount.c    # 从 gc0_baseline.c 派生（malloc 堆形态最近源）
Makefile          # 无需改动（wildcard 自动纳入；无 flag 构建）
README.md         # 表格加行 + diff gc0_baseline.c gc9_refcount.c；计数 eleven-stage
docs/algorithms.md # gc9 节（合并后独立小任务，先例同 gc7/gc8）
```

无 flag 构建：可关的机制不存在；本阶段的"杀手"是泄漏断言本身。
诚实注记（头注释）：Swift/ObjC 编译器自动插桩 retain/release（ARC）；Python
以混合收集对付环；工业解法（周期追踪/混合/弱引用）点名留给 gc10+。

## 3. 机制

**Object**：`type` + `int refCount;` + union（无 marked、无链表指针——
没有全堆遍历，也就没有链表）。

**原语**（均计入 `countOps` 统计、NULL 安全）：

```c
void retain(VM* vm, Object* object);   /* count++ */
void release(VM* vm, Object* object);  /* count--; 归零: 级联 release 字段
                                          → free → liveObjects-- */
```

**栈即强引用**：`push` 尾部 retain；`pop` 尾部 release 但照常返回指针
（调用方继续持有即为转移惯用语的一部分）。

**转移惯用语**（`pushPair`，栈→字段）：**先 retain 后 pop**——pop 会 release，
不先 retain 则唯一引用可能在转移中途死亡。计数净变化为零 = 转移。

**存储三步曲**（`objectSetHead/Tail`）——本阶段最微妙处，顺序即正确性：

```c
retain(vm, value);          /* ① 保护新值：自赋值/新值恰为旧值唯一引用时 */
Object* old = pair->tail;
pair->tail = value;         /* ② 先换字段：级联死亡读到的是新状态 */
release(vm, old);           /* ③ 后释放旧值：①②使旧值之死不伤及新值 */
```

②的必要性由环杀手的手动断环证明：`objectSetTail(a, NULL)` 触发 b→a 的级联
死亡，若字段未先换为 NULL，a 之死会 `release` 已释放的 b（use-after-free）。

**`freeVM`** = 逐根 pop（级联释放）；无收集器可调。

## 4. 测试

| 测试 | 内容 |
|---|---|
| t1 | 即死即回收：push 2、pop 2 → `liveObjects == 0`（无收集器运行过） |
| t2 | 级联死亡：7 对象嵌套树、丢根 → 瞬间清零（RC 卖点 vs gc0 需要收集） |
| t3 | 共享引用计数：同对象双根 → pop 一次仍活、再 pop 才死 |
| t4 | 自赋值惯用语：`objectSetTail(p, p->tail)` → 不中途释放、结构完好 |
| t5 | **环杀手三段**：丢双根 → `liveObjects == 2`（泄漏钉住，注释指向 gc10）→ `objectSetTail(a, NULL)` 手动断环 → 级联清零 `== 0` |
| t6 | 循环覆写：`objectSetTail` 千次替换 → `liveObjects` 恒定（计数无泄漏无重释放，ASan 护航） |
| perfTest | 沿用负载 + 统计行 `N objects live, M refcount ops` + 终态 `liveObjects == 0` |

## 5. 决策记录

| 决策 | 选择 | 理由 |
|---|---|---|
| 派生源 | gc0_baseline.c | malloc 堆、无屏障机制，是 RC 的最小底座 |
| 回收时机 | 即时（无 defer） | 朴素形态是 gc10 优化的基线；deferred RC 留给后续 |
| 存储顺序 | retain→换字段→release | ②防级联重入 use-after-free（t5 断环证明）；①防自赋值（t4）|
| 环处理 | 泄漏钉住 + 手动断环 | 泄漏即本课；自动收集是 gc10 的整章 |
| flag 构建 | 无 | 无可关机制；杀手 = 泄漏断言本身 |
| 统计 | liveObjects + countOps | 范式卖点（即时回收）与范式成本（写流量）都可量化 |

## 6. 风险与已知限制

- ASan 零泄漏依赖 t5 的手动断环把环内存正确清零——若断环步骤遗漏，
  LeakSanitizer 会在 `make asan` 报警（这本身是防线而非负担）
- `pop` 返回已 release 的指针：调用方必须立即转移（转移惯用语）——C 局部
  变量非根的契约在本阶段收紧为"非根且可能即死"
- 千次覆写循环的计数正确性由 ASan 兜底（无双重释放即证据）
