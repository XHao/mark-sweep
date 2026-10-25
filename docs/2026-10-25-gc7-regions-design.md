# gc7 — 区域化收集（G1 思想）设计

日期：2026-10-25
状态：待用户审阅
分支：`gc7-regions`
父 spec：`docs/2026-10-24-gc-teaching-playground-design.md`（约定与风格继承）

## 1. 背景与目标

前六个阶段各自回答一个问题；G1 回答的是**组合与调度**：堆切成等大 region，
按区收集、按脏乱程度排序、每停顿只做有预算的量。本阶段教三课：

1. **堆 = 区域表**：分配在 region 内 bump，region 满了**换区而不触发 GC**——
   只有空闲区耗尽才收集（与 gc3/gc4"分配即触发"的分水岭）
2. **逐区疏散**：收集一个 region = 把它的活对象复制进其他 region，入边来自
   **该区的 remembered set**（槽位级）与全局根——gc3 的复制 + gc4 的屏障，
   全部按区粒度重组
3. **Garbage-First**：标记普查各区活度；老区垃圾累积后 **mixed GC 先挑最脏
   的区**、每停顿最多 `CSET_BUDGET` 个——可观测指纹：脏区先回收、干净区对象
   **地址不动**（与 gc3 全量搬家对照）

成功标准：`make test` 全绿（14/14：9 PASS + `gc7_bo` 预期失败）；gc7 与 gc3
的 diff 聚焦区域机制；t6/t7 把"脏区优先"与"CSET 预算"变成断言。

## 2. 文件与集成

```
gc7_regions.c    # 从 gc3_copying.c 派生（复制机制最近源）
Makefile         # 追加 gc7_bo 规则（-DRSET_OFF，预期失败）
README.md        # 表格加行 + diff gc3_copying.c gc7_regions.c；计数 nine-stage
docs/algorithms.md # gc7 节（合并后补，独立小任务）
```

不做（YAGNI，头注释说明对应真身）：真并发标记（gc2/gc6 已教）、老化晋升
（gc4s 已教；本阶段 young 存活者直接晋升 old region）、humongous 对象、
软实时调度器。疏散无目标区时 `check` 诚实拦截（G1 的 fallback 是 full GC）。

## 3. 数据结构

```c
#define HEAP_MAX 8192
#define REGION_COUNT 16
#define REGION_SIZE 512        /* 16 x 512 = 8192，与 gc3/gc5 同总内存 */
#define CSET_BUDGET 2          /* 每次 mixed 最多疏散的区数 */

typedef enum { REGION_FREE, REGION_EDEN, REGION_OLD } RegionType;

typedef struct {
  RegionType type;
  int used;                    /* bump 游标 */
  int live;                    /* 普查后的活对象数（mixed 选择用） */
  Object*** rset;              /* 指入本区的槽位数组（动态增长） */
  int rsetCount, rsetCapacity;
} Region;
```

`Object` 在 gc3 基础上加回 `unsigned char marked`（普查标记）。VM 持
`Object* heap; Region regions[REGION_COUNT]; int edenIndex;` 与统计
（youngGcCount / mixedGcCount / regionsEvacuated / objectsMoved）。

**RSet 为槽位级**（`Object**`），插入按精确槽位地址去重——gc4s 教训的正解
在此落地：真实收集器记录的就是槽位。跨区边（源对象与目标对象不同区）才
记录；栈不是 region，根每次停顿全扫（G1 同）。

## 4. 算法

**分配**：eden region 满 → 找 FREE 区当新 eden；**无 FREE** 时按 §5 时序。
`regionOf(object)` 由指针算术得区号（heap 是单块连续 arena）。

**写屏障**（`objectSetHead/Tail`）：`regionOf(pair) != regionOf(value) &&
value != NULL` → 把**槽位地址**记入 `regions[regionOf(value)].rset`。

**疏散一个区 R**（young 与 mixed 共用）：

1. 工作集 = 栈根中指向 R 的槽位 ∪ R.rset
2. 槽位弹出：目标在 R 内 → 复制（首次经 husk 转发，同 gc3）到**目标区**
   （young：old 区；mixed：其余 old 区），改写槽位；目标不在 R → 跳过
3. 复制体的每个字段若指向**其他区** → 把**复制体的槽位**记入目标区 rset
   （见 §6，这不是优化而是正确性必需）
4. 完成后 R 置 FREE、清 rset；R 内垃圾零成本消失

**young GC**：疏散全部 eden 区。**普查**：gc1 式 worklist 从根标记，
按区统计 `live`，随后全堆清标记。**mixed GC**：普查 → 老区按
`live/REGION_SIZE` 升序（最脏优先）取前 `CSET_BUDGET` 个（跳过全满区）→
逐区疏散。

## 5. 触发时序（鸡生蛋的解法）

疏散需要 FREE 区做目标，而触发时可能一个 FREE 都没有。规则：

```
acquire eden region:
  FREE ≥ 3  → 直接取用
  FREE == 2 → youngGC（若存在 eden 区；用一个 FREE 当复制目标，全部 eden
              区随之释放，级联换目标）→ 回到顶重试
  FREE == 1 → census + mixed（用最后一个 FREE 当首个复制目标；每疏散完一个
              cset 区即释放一整区，级联供给后续目标）→ 回到顶重试
  FREE == 0 → check("Heap exhausted")（不可达：触发规则保证 mixed 先行）
```

关键不变式：**任何疏散发生时 FREE ≥ 1 恒成立**——young 在 FREE==2 触发、
mixed 在 FREE==1 触发，各自至少握有一个复制目标；疏散本身只会增加 FREE。
若 mixed 的 cset 全为满活区（无垃圾可释），堆真实耗尽，check 如实拦截。
确定性：单线程 + 顺序固定。

## 6. RSet 生命周期（本阶段最容易错的一处，先写清）

疏散 R 时复制对象 O 到 D 区：O 的字段原本指向 C 区对象的边，已登记在
**C.rset 里的源槽位**（`&O_old->head`）——源死了，该条目自然作废。但**复制体**
的槽位是新地址，若 C 以后被疏散而 rset 里没有 `&O_copy->head`，这条边就
隐形、C 的疏散会漏掉 O_copy 的引用。**因此复制时必须给复制体的跨区字段
登记 noteEdge**——旧条目作废、新条目生效，正是 gc4s"边存续"问题的槽位级
精确解。跨区环（A↔B 同停顿疏散）因此天然安全：先疏散者的边被登记进
后疏散者的 rset。

## 7. 测试

| 测试 | 断言 |
|---|---|
| t1–t4 | 核心场景改自 gc3；t4 为**跨区环**（≥REGION_SIZE 对象保证跨区） |
| t5 分水岭 | 连续分配跨满多个 eden 区**不触发任何 GC**（统计零），直到预算耗尽 |
| t6 脏区优先 | 构造密度已知的区 → mixed → 最脏区对象搬家、干净区对象地址不变 |
| t7 CSET 预算 | 足量脏区时单次 mixed 只疏散 ≤ CSET_BUDGET 区；连续触发逐批回收 |
| t8 RSet 杀手 | P 在 O1、x 仅经 P->tail 可达且在 O2（借 512 填充物精确布位）→ 疏散 O2 后 x 存活且 `P->tail->value` 正确；`gc7_bo`（-DRSET_OFF）经槽位复用按值检测丢失 |
| perfTest | 沿用负载 + 逐类统计行（young/mixed/搬移对象/疏散区数） |

布位确定性：所有复制按序打包，512 整除使区边界可算；t8 依赖"O2 首槽复用"
的 gc4s 同款技巧。

## 8. 决策记录

| 决策 | 选择 | 理由 |
|---|---|---|
| 派生源 | gc3_copying.c | 复制/转发机制最近源；区域机制是纯增量 |
| 普查 | STW（gc1 式） | 并发标记是 gc2/gc6 的课；本阶段课是区域与选择 |
| 晋升 | young 存活直接进 old 区 | 老化已在 gc4s 教过；组合留作读者练习（头注释点名） |
| RSet | 槽位级 + 精确去重 | gc4s 教训的正解；区域粒度正好承载 |
| 触发 | FREE≥2/==1/==0 三段规则 | 破解"疏散需目标区"的鸡生蛋 |
| 预期失败 | `gc7_bo`（-DRSET_OFF） | 跨区屏障可关；杀手 t8 与 gc4_bo/gc6 变体同构 |
| REGION_SIZE | 512 | 测试可用 1–2 区粒度布位；总数与 gc3/gc5 同内存 |

## 9. 风险与已知限制

- 本阶段机制面最大：§6 的 noteEdge 若遗漏，跨区引用在后续疏散中静默丢失
  ——t8 与 t6 联合锚定；§5 的触发层位若错位（如 mixed 推迟到 FREE==0），
  疏散无目标区而死锁——规则表已内建不变式
- `mixed` 只在 FREE==0 路径触发：老区垃圾若不顶着分配压力则长期不收
  （G1 由并发标记周期驱动；注释说明差异）
- 疏散目标区满时需级联取区：实现须处理"复制中途换区"，t3/t4 的大对象图
  覆盖
- 普查为 O(堆)；教学取舍，注释标注
