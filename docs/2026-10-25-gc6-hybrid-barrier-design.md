# gc6 — 混合写屏障教学阶段设计

日期：2026-10-25
状态：待用户审阅
分支：`gc6-hybrid`
父 spec：`docs/2026-10-24-gc-teaching-playground-design.md`（约定与风格全部继承）

## 1. 背景与目标，以及一个诚实的前提

gc2 用 Yuasa 删除屏障完成了增量标记。本阶段讲 Go 1.8+ 的**混合写屏障**
（hybrid barrier = Dijkstra 插入半边 + Yuasa 删除半边）——它存在的全部理由是
**goroutine 栈不插屏障**：纯插入屏障下，未屏障的栈写要求标记结束时 STW 重扫
全部栈；Go 选择"两半都上"，用少量浮动垃圾换掉重扫。

**诚实声明**：在本玩具的 mutator 模型里，hybrid 与纯 Yuasa 行为等价——gc2
的 `gcStart` 预灰全部根，已经等价于"起点栈快照"，因此本模型里从来不需要重
扫。本阶段真正**新增的可观测内容**是：`pushRoot` API 模拟未屏障栈写，配一个
"根写 + 路径删除"杀手测试——纯插入屏障在此丢失**已经入根**的对象，混合屏障
靠删除半边救回。这就是 Go 1.8 故事的微缩演示；Go 映射与取舍在注释中讲全。

成功标准：`make test` 全绿（gc6 计入 BINS，`gc6_dijkstra` 预期失败）；
`gc6_hybrid.c` 与 `gc2_incremental.c` 的 diff 聚焦于屏障语义与 pushRoot。

## 2. 文件与集成

```
gc6_hybrid.c      # 从 gc2_incremental.c 派生
Makefile          # 追加 gc6_dijkstra 规则（-DBARRIER_DIJKSTRA，预期失败）
README.md         # 表格加行 + diff gc2_incremental.c gc6_hybrid.c
```

## 3. 屏障语义

`objectSetHead/objectSetTail` 升级为混合（其余机制——pop-shade、出生即黑、
分配/写驱动单步——从 gc2 原样继承）：

```c
void objectSetTail(VM* vm, Object* pair, Object* value) {
  /* Both halves only matter while a cycle is marking — shading at
     idle would seed the gray stack and corrupt the next cycle (the
     gc2 lesson). */
  if (vm->gcState == GC_MARKING) {
#ifndef BARRIER_DIJKSTRA
    /* Deletion half (Yuasa): overwriting a field deletes a snapshot
       path; shade the OLD value. */
    shade(vm, pair->tail);
    /* Insertion half (Dijkstra): a black object gaining a white
       reference breaks tricolor; shade the NEW value. */
    if (pair->color == COLOR_BLACK) shade(vm, value);
#else
    /* Insertion-only variant: the pre-1.8 Go barrier. */
    shade(vm, value);
#endif
  }
  pair->tail = value;
  if (vm->gcState == GC_MARKING) gcStep(vm);
}
```

注释映射 Go `runtime/atomic_pointer.go` 的 `writePointer`：`shade(*slot)` +
`if current stack is black: shade(ptr)`，并说明取舍（重扫栈 vs 浮动垃圾）与本
玩具无需重扫的原因（预灰根 = 起点栈快照）。

## 4. pushRoot

```c
/* Simulate an UNBARRIERED stack write, like a goroutine stack store.
   Deliberately shades nothing and drives no GC step — that is the
   point: the barrier must tolerate this. */
void pushRoot(VM* vm, Object* object) {
  push(vm, object);
}
```

## 5. 测试

| 测试 | 内容 |
|---|---|
| t1–t7 | 继承 gc2 全部（混合屏障语义是 Yuasa 的超集，全过；t7 删除杀手靠删除半边） |
| t8 根写杀手 | 见下编排 |

**t8 编排**（无 UB：只断言计数，不 deref 可能悬垂的指针）：

```
pushInt(1); pushInt(42); g = pushPair  → g = (1, victim)，stack [g]
victim 仅经 g->tail 可达
gcStart                                  → roots [g] 置灰
pushRoot(vm, victim)                     → 未屏障根写，stack [g, victim]
objectSetTail(g, g->head)                → 删除唯一旧路径
gcDrive
check(numObjects == 3, "The barrier must save the newly-rooted object.")
```

- **hybrid**：删除半边 shade(victim) → victim 灰 → 存活 → 3 ✓
- **dijkstra 变体**：shade(新值 int1)，victim 始终白 → sweep 回收 → 2 ✗

**变体下的 t7 跳过**：t7（删除杀手）在 dijkstra 变体下必然失败——那是 Yuasa
的领地，失败方式与 t8 同类。为让 `gc6_dijkstra` 的失败**落在 t8 这个新演示上**，
其 main 中以 `#ifndef BARRIER_DIJKSTRA` 跳过 t7（注释说明跳过原因）。

## 6. 决策记录

| 决策 | 选择 | 理由 |
|---|---|---|
| 交错模型 | 沿用 gc2 lockstep，不引入线程 | 可复现可测试；屏障是 mutator 与收集器间唯一契约 |
| 变体数量 | 默认 hybrid + `-DBARRIER_DIJKSTRA` 一个变体 | 第三个变体无新失败演示，YAGNI |
| dijkstra 变体形态 | 无条件 shade 新值（教学简化形） | 经典"插入屏障"的简化表述，注释标注与 Go 精确形式的差异 |
| pushRoot 语义 | 不 shade、不驱动步 | 演示的就是"未屏障栈写"本身 |
| t7 变体跳过 | #ifndef 包裹 | 让预期失败落在 t8 的根写演示上 |
| 跨轮测试 | 不依赖 freeVM 充数（gc5 终审经验） | 显式 gcDrive 断言为主 |

## 7. 风险与已知限制

- t8 与 t7 的失败路径在 dijkstra 下同源（删除半边缺失）；spec 以 t7 跳过保证
  观测点唯一
- 混合屏障的插入半边（black owner 时 shade 新值）在本测试集内无独立失败演示
  ——它的价值在真实并发语境（未屏障栈 + 无重扫），注释如实说明，不过度宣称
- gcStart 的 `check(gcState == GC_IDLE)`：t8 直接调用 gcStart，前置状态由编排
  保证
