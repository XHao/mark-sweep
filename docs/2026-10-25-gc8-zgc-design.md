# gc8 — ZGC 式染色指针 + 读屏障设计

日期：2026-10-25
状态：待用户审阅
分支：`gc8-zgc`
父 spec：`docs/2026-10-24-gc-teaching-playground-design.md`（约定与风格继承）

## 1. 背景与目标

gc3 的复制收集在停顿里**改写一切指针**；ZGC 的洞见恰恰相反：**对象先搬走，
指针以后再修**——读屏障在每次加载时惰性治愈，使搬迁与 mutator 并存成为可能。
本阶段教三个签名机制的最小版：

1. **指针染色**：堆字段中的引用在存储时带 **bit0 视图位**（epoch 校验位，
   每轮 relocate 翻转）——"颜色在指针里，屏障只需一次比较"
2. **读屏障 `loadRef(vm, slot)`**：颜色陈旧或目标是 husk → 经转发表就地治愈
   （改写槽位 + 刷成当前色）并返回干净指针；治愈后走快路径（一次比较）
3. **惰性搬迁**：relocate 停顿复制活对象、建 husk 转发表、**就地治愈栈根**
   （= ZGC 的 STW 根阶段），**堆字段一律不改写**——旧空间整个充当转发表
   **保留到下一轮停顿开头**，由治愈扫统一处理（ZGC 保留 relocation 页直至
   全部治愈的同构替代）

成功标准：`make test` 全绿（**18/18**：10 PASS + 8 个预期失败构建，含
t5 使染色与惰性治愈**直接可观测**（陈旧色 → loadRef → 翻新 + heal 计数）。
（修订 2026-10-26，终审后：计数 17/17 系笔误，实为 18/18。）

## 2. 文件与集成

```
gc8_zgc.c    # 从 gc3_copying.c 派生
Makefile     # 追加 gc8_bo 规则（-DLOAD_BARRIER_OFF，预期失败）
README.md    # 表格加行 + diff gc3_copying.c gc8_zgc.c；计数 ten-stage
docs/algorithms.md # gc8 节（合并后独立小任务，同 gc7 先例）
```

诚实注记（头注释）：真 ZGC 的颜色在 64 位指针高位（Marked0/1/Remapped），
治愈经全局转发表、无需类型检查；本玩具用 bit0 视图位 + husk 类型检查补位
——同一套惰性治愈纪律，编码从简。读屏障全量插桩（所有字段读取经
`loadRef`）对应真实系统中编译器对每次引用加载插桩。

## 3. 机制

**染色与屏障**（堆字段带色；栈是无色根，停顿中就地治愈）：

```c
Object* loadRef(VM* vm, Object** slot) {
  Object* pointer = *slot;
  Object* object = untag(pointer);
  /* Fast path: current view color AND target not relocated. */
  if (((size_t)pointer & 1) != (size_t)vm->color ||
      object->type == OBJ_FORWARDED) {
    if (object->type == OBJ_FORWARDED) object = object->forward;
    *slot = tagColor(vm->color, object);   /* heal in place */
    vm->healCount++;
  }
  return object;
}
```

写屏障（轻量）：`objectSetHead/Tail` 存入 `tagColor(vm->color, value)`。
**剃刀情况**（文档化并由 t5 断言）：停顿后 C 局部变量持有的**旧空间地址**被
存入字段时带**当前色**——颜色检查放行，husk 检查兜住——这正是屏障需要两条
检查而非一条的原因。

**relocate 停顿**（gc3 的骨架、两种语义反转）：

1. **治愈扫**：walk 当前空间所有 pair 字段过 `loadRef`——上一轮遗留的陈旧
   指针在此统一治愈（旧空间转发表仍完整，此刻是它最后的服役窗口）
2. **根阶段**：栈槽位经 `copyObject` 就地治愈（转发表即 husk，gc3 技巧）
3. **扫描**：沿 to-space 推进，但**只遍历不改写**——`copyObject(untag(f))`
   仅为复制与建 husk；字段继续指向旧空间（陈旧色，等读屏障来修）
4. **翻转**：交换空间、`color = 1 - color`——**颜色翻转使复制体里的全部旧
   字段在纯颜色比较下即刻可判陈旧**（这是染色机制的 payoff）

**对齐前提**：`Object` 为 24 字节、堆内地址 8 对齐，bit0 空闲可作色位；
注释声明该前提。

## 4. 测试

| 测试 | 内容 |
|---|---|
| t1–t4 | 核心场景（字段读取全部经 loadRef；t4 环经 husk 追踪闭合） |
| t5 惰性+染色杀手 | relocate 后断言字段**颜色陈旧**（染色可观测）→ `loadRef` 治愈 → 颜色翻新 + `healCount == 1` + 内容正确；随后存入 C 持有的旧地址（剃刀情况）→ loadRef 经 husk 检查治愈。`gc8_bo`：loadRef 退化为裸读 → 返回 husk → 内容断言失败（有效内存，零 UB） |
| t6 跨轮治愈扫 | 留陈旧指针不读 → 分配至下一次 relocate → 治愈扫统一处理（healCount 计入）→ 结构完整 |
| t7 快路径 | 治愈后重复 loadRef 不再计数；`healCount` 恒定 |
| perfTest | 沿用负载 + 统计行（relocate 次数 / heal 计数） |

**gc8_bo 的跳过模式**（gc6/gc7 先例）：OFF 构建下 t1/t3/t4/t6 均会在读屏障
缺失时更早失败，`main` 中以 `#ifndef LOAD_BARRIER_OFF` 跳过，仅留 t2 与杀手
t5，失败落点唯一。

## 5. 决策记录

| 决策 | 选择 | 理由 |
|---|---|---|
| 派生源 | gc3_copying.c | 复制/转发机制同源；语义反转（不重写字段）即本课 |
| 染色编码 | bit0 视图位 + epoch 翻转 | 高位染色不可移植；bit0 在 8 对齐下安全 |
| 搬迁粒度 | 单停顿整体搬迁 + 停顿后惰性治愈 | 完整并发管线两套机制，糊掉读屏障课（YAGNI） |
| 旧空间生命周期 | 保留至下一轮停顿的治愈扫 | ZGC 页保留的诚实替代；双空间下唯一可行 |
| 屏障双检查 | 颜色比较 OR husk 检查 | 剃刀情况（新色旧址）需要后者；t5 断言 |
| 预期失败 | `gc8_bo`（-DLOAD_BARRIER_OFF） | 读屏障可关；杀手返回 husk，type/内容断言优雅失败 |
| 根处理 | 停顿中就地治愈（不计 healCount） | ZGC 的 STW 根阶段同构；计数只反映读屏障治愈 |

## 6. 风险与已知限制

- 对齐前提依赖 `sizeof(Object)` 为 8 的倍数与 malloc 对齐——macOS clang
  成立；注释声明，换平台需重验
- C 局部变量跨 relocate 全部陈旧（gc3 同款契约），且**带当前色存入**时靠
  husk 检查兜底——t5 专测；**有效窗口仅一轮**：下一停顿后旧空间被复用，
  连 husk 检查都会读到新住户（无 UB 的静默别名）——头注释已明示
  "每停顿后重取指针"
- 治愈扫为 O(堆占用)；ZGC 按页惰性回收，教学取舍注释标注
- `healCount` 的精确断言依赖"停顿根阶段不计数"的约定——统计语义写进注释
