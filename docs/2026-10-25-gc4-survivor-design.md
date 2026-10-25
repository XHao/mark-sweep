# gc4_survivor — 年龄阈值 + survivor 空间设计

日期：2026-10-25
状态：待用户审阅
分支：`gc4-survivor`
父 spec：`docs/2026-10-24-gc-teaching-playground-design.md`（约定与风格继承）；
前置：`docs/2026-10-24-gc-teaching-playground-design.md` §3 gc4 的修订记录
（"年龄阈值/survivor 空间列为后续可选扩展"）——本文件兑现该挂账。

## 1. 背景与目标

gc4 的"首次存活即晋升"是被诚实记录的缺陷：每次 nursery 填满时恰在栈上的
短命对象全体晋升，污染老年代（实测 78 minor / 32 major / 636 晋升）。本阶段
引入 **age threshold + survivor space**（HotSpot 同构）修正之：gc4 原文件
**原地保留**（"最简分代 + 已知缺陷"本身是一课），新文件并列展示修正，
`diff gc4_generational.c gc4_survivor.c` 即老化机制的精确增量。

**可量化验收**：同一 perfTest 负载下，gc4_survivor 应得 **78 minor /
0 major / 0 晋升**——churn 在年龄门槛前死亡，老年代零污染；spec 原始验收
信号"major 远少于 gc3 的 5 次收集"首次成立。该结果由 perfTest 内的
确定性断言钉死。

## 2. 文件与集成

```
gc4_survivor.c    # 从 gc4_generational.c 派生
Makefile          # 无需改动（wildcard 自动纳入；无 flag 构建）
README.md         # gc4 行后插行 + diff gc4_generational.c gc4_survivor.c；计数 eight-stage
docs/algorithms.md # 不改（gc4 节已引用本扩展为挂账，落地后由 gc4_survivor 自述）
```

## 3. 机制

**Object** 增一字段：`unsigned char age;`（出生为 0）。

**nursery 升级为双空间**（各 `NURSERY_MAX 256`）：

- 平时只有 eden 活着：分配 bump 进 `nursery[nurserySpace]`（同 gc4 单空间）
- minor GC 期间存活者**复制进另一块**（survivor）、`age+1`，随后**角色互换**
  ——Cheney 双空间技巧在年轻代内部的复用；两次范围判断并入 `isYoung`

**晋升判定**（进既有 slot worklist 的复制点）：

```c
if (object->age + 1 >= AGE_MAX || forceOld) → malloc 晋升老年代（age 归 0）
else                                        → 复制进 survivor（age+1，仍年轻）
```

`#define AGE_MAX 2`（存活两个 minor 后晋升）。husk 转发、槽位重写、
remset 清理、`remembered`/`marked` 处理逐字沿用 gc4。**survivor 溢出**以
`check("Survivor overflow!")` 如实拦截（HotSpot 用"溢出即晋升"处理，注释
说明差异）。

**majorGC**：先 `copyYoung(vm, /*forceOld=*/1)`（存活者全员强制晋升——老
年代标记清扫只碰老对象），其余不变。触发条件不变（minor 后
`oldCount >= oldMax`）。

**写屏障 / remset / `objectSetHead/Tail`**：逐字不动。

## 4. 测试

| 测试 | 内容 |
|---|---|
| t1 老化演示 | 两个 int：一次 minor 后仍年轻（`nurseryCount == 2 && oldCount == 0`）；二次 minor 后晋升（`oldCount == 2 && promotedCount == 2`）——年龄语义的直接断言 |
| t2 | 不可达回收：双计数器归零 |
| t3 | 嵌套结构：两次 minor 全员晋升后 `oldCount == 7`，结构遍历完整 |
| t4 | 环：两次 minor 后 `oldCount == 4`，环闭合 |
| t5 remset 杀手 | 机制不变；存活判定改为 x 以 survivor 身份存活再（第二次 minor）晋升。OFF 检测利用空间互换：x 死亡的 eden 在下一次 minor 后重新成为分配区，`pushInt(7)` 覆写其槽位 → `p->tail->value != 42`，零 UB |
| t6 | 200 存活：一次 minor 全为 survivor（`oldCount == 0`，**无 major**）；二次 minor 全员晋升 → `oldCount 200 ≥ oldMax 8` → major 触发。pop 全部 + minor → 老年代计数纹丝不动 |
| t7 | NULL 安全（p 经两次 minor 变老后写 NULL） |
| perfTest 头条 | 继承原负载 + **`check(majorGcCount == 0 && promotedCount == 0)`**——本阶段的全部卖点钉进一条确定性断言 |

## 5. 决策记录

| 决策 | 选择 | 理由 |
|---|---|---|
| 形态 | 新文件并列，不原地改 gc4 | 保住 v1 的教学叙事（含诚实缺陷）；diff 即增量 |
| AGE_MAX | 2 | 最小可演示阈值：一次存活→年轻、两次→晋升，t1 两行断言讲完 |
| survivor 溢出 | check 失败 | HotSpot 的"溢出即晋升"是额外机制，教学从简并注释 |
| majorGC | 先强制全员晋升再清扫 | 沿用 gc4 的前置不变式，标记清扫只碰老对象 |
| flag 构建 | 无 | 老化无屏障可关；行为由 t1/t6/perfTest 断言直接锚定 |

## 6. 风险与已知限制

- survivor 容量 256：测试的最大一次年轻存活集为 t6 的 200，余量 56
- 空间互换使 OFF 检测多一步 minor（t5 编排已计入）；ON 路径不受影响
- `age` 占 1 字节/对象——真实系统把年龄打包进 mark word，注释说明
