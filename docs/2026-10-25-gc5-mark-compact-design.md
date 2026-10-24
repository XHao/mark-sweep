# gc5 — 标记-压缩教学阶段设计

日期：2026-10-25
状态：待用户审阅
分支：`gc5-mark-compact`
父 spec：`docs/2026-10-24-gc-teaching-playground-design.md`（约定与风格全部继承）

## 1. 背景与目标

在既有五阶段 GC 教学框架上追加第六个阶段 `gc5_compact.c`：**Lisp2 三趟滑动
标记-压缩**。补齐经典三件套的最后一块——gc3 的头注释明确点名的代价
（"at most half the heap is usable at any time"）在本阶段兑现为反面：

| | gc3（双空间复制） | gc5（滑动压缩） |
|---|---|---|
| 同总内存下可用堆 | 50% | **100%** |
| 收集期堆遍历 | 2 次（均为活对象） | 4 次（标记活者 + Pass 1/3 扫全占用区间 + Pass 2 活者） |
| 对象顺序 | 广度优先重排 | **保序滑动** |
| 分配 | bump | bump（同） |

成功标准：`make test` 全绿（BINS wildcard 自动纳入 gc5）；gc5 与 gc3 的 diff
聚焦于"双空间复制 → 单空间压缩"；容量测试断言 gc3 物理不可能的存活规模。

## 2. 文件与集成

```
gc5_compact.c   # 从 gc3_copying.c 派生：删 to-space/转发机制，加回标记与三趟压缩
Makefile        # 无需改动（wildcard gc*.c 自动纳入；无 flag 构建）
README.md       # 表格加 gc5 行 + diff 指引 diff gc3_copying.c gc5_compact.c
```

无 `OBJ_FORWARDED`（压缩期对象必须保持可读，不能覆写 type）、无侵入式链表、
无 flag 构建（gc5 无屏障可关；卖点由容量/保序/内容测试直接断言）。

## 3. 数据结构

```c
#define HEAP_MAX 8192  /* 与 gc3 两块 4096 空间同总内存：可用堆 100% vs 50% */

typedef struct sObject {
  ObjectType type;
  unsigned char marked;     /* gc1 式标记位 */
  struct sObject* forward;  /* Pass 1 写入的压缩目标地址 */
  union { int value; struct { struct sObject* head; struct sObject* tail; }; };
} Object;

typedef struct {
  Object* stack[STACK_MAX];
  int stackSize;
  Object* heap;      /* 单块 arena */
  int freeCount;     /* bump 游标，亦即堆占用高水位 */
  int numObjects;    /* 存活数（== freeCount） */
} VM;
```

`forward` 为**专设字段而非 union 成员**：Pass 2 改指针期间存活 pair 的
head/tail 必须可读，不可复用其存储。注释说明真实 JVM 的做法（mark-word 位
打包转发指针 / Brooks forwarding pointer），教学版付 8 字节/对象买清晰。

## 4. 算法

**分配**：`&heap[freeCount++]`；满时 `gc(vm)`，仍满则 `check("Heap overflow!")`。

**gc：标记 + 三趟压缩**（Lisp2 的 "three passes" 指压缩三趟，标记在其前）：

1. **标记**：gc1 的迭代 worklist 从 VM 栈根出发
2. **Pass 1（算新址）**：`i` 升序扫 `[0, freeCount)`：存活 → `heap[i].forward
   = &heap[newCount]`，`newCount++`；死亡 → `forward = NULL`
3. **Pass 2（改指针）**：每个栈根槽位与每个存活 pair 的 head/tail 改写为
   `old->forward`（对象仍在原址；NULL 字段保持 NULL）
4. **Pass 3（滑动）**：`i` 升序，存活 → `heap[dest] = heap[i]`（`dest ≤ i`，
   无前向覆盖），清 `marked`；结束后 `freeCount = numObjects = newCount`

环与重复引用天然安全：Pass 1 给每个存活者唯一新址，Pass 2 幂等改指针。

**输出**：`Collected %d objects, %d remaining, compacted %d objects.\n`
（remaining == compacted，滑动的是全部存活者）。

## 5. 测试

| 测试 | 断言 |
|---|---|
| t1–t4（改自 gc3） | 核心场景；移动 GC——GC 后从栈重取指针；t4 环收集后内容校验 |
| t5 容量翻倍 | 存活链深度 3000（5999 对象）：> gc3 单空间上限 4096，≤ 8192；断言 numObjects 与链端值 |
| t6 碎片回收+保序 | 构建 8 条短链（各留 1 根，共 8 根 ≤ STACK_MAX），丢掉偶数位 4 条根 → gc → numObjects == 4 条链对象数、存活者地址严格升序（滑动保序指纹）、继续 pushInt 成功（空洞已合并） |
| t7 深链 | 深度 4000（7999 对象 ≤ 8192）压缩后逐级遍历，链长与端值完整 |
| perfTest | 沿用原负载 + 统计行（collected/remaining/compacted） |

## 6. 决策记录

| 决策 | 选择 | 理由 |
|---|---|---|
| 堆模型 | 单连续 arena（同总内存 8192） | 与 gc3 对比公平：100% vs 50% 可用堆可直接测试断言 |
| 转发地址 | 专设 forward 字段 | Pass 2 需同时读旧字段与新地址；union 复用不可行 |
| 派生来源 | cp gc3_copying.c | 分配器（arena+bump）同源；diff 焦点 = 双空间复制 → 标记+压缩 |
| 压缩形态 | 滑动保序而非任意重排 | Lisp2 经典；保序使 t6 的升序地址成为可断言指纹 |
| flag 构建 | 无 | 无屏障语义可关；容量/保序/内容测试已直接锚定卖点 |

## 7. 风险与已知限制

- Pass 3 的 `heap[dest] = heap[i]` 为结构体赋值；dest == i 时自赋值安全跳过
- 压缩成本 O(堆占用) 而非 O(存活)（Pass 1/3 扫全占用区间）——注释如实标注，
  这正是真实系统用区域化/分代来摊销的动因（呼应 gc4/gc6 谱系）
- 栈上限 STACK_MAX 256：容量测试须用链式构建（同 gc3 overflowTest 教训）
