# gc10 — 周期收集器（RC + tracing 混合）设计

日期：2026-10-31
状态：待用户审阅
分支：`gc10-cycles`
父 spec：`docs/2026-10-26-gc9-refcount-design.md`（RC 基座）；
算法族：Bacon 同步 trial-deletion（Python gc 同族）

## 1. 背景与目标

gc9 把范式缺陷钉成了断言：无根的环泄漏**整个可达闭包**。gc10 交付工业解法——
**周期收集器**：计数管日常即死即回收，周期性 tracing 兜底收环。这是两大范式的
合流，也是本系列终章：tracing 的三色词汇表（gc1/gc2）反过来救 counting。

核心算法（Bacon 同步三阶段）：

1. **候选根缓冲**：`release` 使计数下降但未死者入 root buffer（去重）——只有
   被释放过的对象才可能是环头（Bacon 洞察）；**混合的代价**即此缓冲 + scratch
   色，而非全堆注册表
2. **试探删除**（mark-gray，工作队列版）：从候选根出发，沿子边逐条
   `child.refCount--`（每对象恰一次，色守卫）——外部引用在试探后仍 > 0，
   环内互持归零
3. **扫描黑**（scan）：试探计数 > 0 者**外部存活**——置黑、沿边恢复计数
   （+1）、黑性传播；试探零者置白
4. **收集白**：仍白者即自持环，级联释放（直接 free + 递归收集，不走 release）

**正确性不变式**：任何未被试探删除的入边使计数 > 0 → scan-black 救活；
被试探删除的边的源头若外部存活，scan-black 传播恢复之。故误杀与漏收皆不可能。

成功标准：`make test` 全绿（**21/21**：12 PASS + 9 预期失败）；`make asan`
**12/12**（freeVM 释放根后自动跑一轮收集，清零离场——与 gc9 的泄漏式离场
对照）；gc9 的 t1–t6 **全部原样通过**（RC 语义零改动）。

## 2. 文件与集成

```
gc10_cycles.c     # 从 gc9_refcount.c 派生
Makefile          # 追加 gc10_bo 规则（-DNO_SCAN_BLACK，预期失败）
README.md         # 表格加行 + diff gc9_refcount.c gc10_cycles.c；计数 twelve-stage
docs/algorithms.md # gc10 节（合并后独立小任务，先例同前）
```

诚实注记（头注释）：Python 的 gc 即此族（差异：分代候选缓冲 + 阈值自动触发
vs 本实现的显式 `collectCycles`）；Swift/ObjC 不自动收环，把问题暴露给程序员
（weak/unowned）。

## 3. 机制

**Object** 增：`unsigned char gcColor`（WHITE/GRAY/BLACK scratch）与
`unsigned char buffered`。**VM** 增：`cycleRoots` 动态数组（候选缓冲）、
`cyclesCollected`/`cycleFreed` 统计。

**缓冲时机**（`release` 内）：计数下降且未死且未缓冲 → 入缓冲。**死亡退缓冲**
：常规 RC 死亡若对象在缓冲中，先从缓冲移除再 free（防悬挂候选）。

**`collectCycles(vm)`**（三阶段，全工作队列，无递归）：

```
candidates = 消费整个缓冲（清 buffered 标志）
mark-gray: 队列弹出 x；x 非 GRAY → 置 GRAY，逐子 child.refCount-- 且入队
scan:     逐候选 s：队列弹出 x；x 为 GRAY：refCount > 0 → scan_black(x)
           否则置 WHITE、子入队继续 scan
scan_black: x 置 BLACK，逐子 child.refCount++ 且递归黑化（队列版）
collect:  逐候选 r：队列弹出 x；x 为 WHITE → 置 BLACK（防重访）、子入队、
           free(x)、cycleFreed++（不走 release——计数已无意义）
若 cycleFreed > 0 → cyclesCollected++
```

**freeVM**：逐根 pop 后自动 `collectCycles`（清零离场）。

## 4. 测试

| 测试 | 内容 |
|---|---|
| t1–t6 | **gc9 原样继承**（RC 语义零改动；t5 仍是"无收集器时泄漏+手动断环"的基线课） |
| t7 对偶回收 | **gc9-t5 同一构造**：丢双根 → `collectCycles` → `liveObjects == 0` 自动清——两章互为注脚 |
| t8 外部引用救岛 | a↔b 环 + 额外根持 a → `collectCycles` → 岛**存活**（scan-black 恢复计数）；再丢根再收 → 清零。`gc10_bo`（`-DNO_SCAN_BLACK`）死于本测试前半（误杀外部存活岛） |
| t9 嵌套与悬链 | x↔y 环挂无环 z：无外部引用 → 三者全收；z 被外部根持有 → 仅环被收、z 独存 |
| perfTest | gc9 负载原样（断言不变）+ 环churn 段：100×（建 a↔b 岛丢根）+ 一次 `collectCycles` → `cycleFreed == 600`、终态 `liveObjects == 0` |

**gc10_bo 失败落点**：scan 阶段被编译为无条件走白支 → t8 的
"Should have preserved objects" 优雅失败（误杀即证据）。

## 5. 决策记录

| 决策 | 选择 | 理由 |
|---|---|---|
| 算法 | Bacon 同步 trial-deletion | 教科书标准；增量/并发版（Bacon-Rajan）三机制叠加，YAGNI |
| 候选范围 | release 触发的 root buffer | Bacon 洞察本身是一课；全堆扫描丢掉这课 |
| 注册表 | 无 | 缓冲算法不需要；混合代价=缓冲+scratch 色（叙事更准） |
| 触发 | 显式 `collectCycles` + freeVM 自动一轮 | 库内惯例（youngGC/minorGC 同款）；阈值自动触发 YAGNI |
| 收集期释放 | 直接 free 不走 release | 计数已无意义；防双重释放 |
| 预期失败 | `gc10_bo`（-DNO_SCAN_BLACK） | 误杀（外部引用岛被收）比漏收更险恶——杀手钉住它 |
| scratch 色 | 专设 gcColor 字段 | 与 refCount 语义分离；真身打包进 mark word，注释说明 |

## 6. 风险与已知限制

- **缓冲悬挂**：常规死亡未退缓冲 → collectCycles 访问已 free 内存——t6 千次
  覆写 + ASan 护航（每轮覆写都产生"下降未死"候选）
- 收集期间的 free 顺序：collect 先置 BLACK 再递归子（防环重访）——t9 嵌套环
  验证
- scratch 色跨轮残留：每次 collectCycles 开始时对候选全清（或依赖三阶段自身
  归一：WHITE/BLACK 终态无害，GRAY 残留不可能——scan 消费所有 GRAY）
- perfTest 的环 churn 段使 `cycleFreed == 600` 依赖精确构造——数字在实现期
  以实测校准后回填断言（同 gc4s 的 78→83 先例）
