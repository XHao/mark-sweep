# 三色标记 + 屏障变体交互动画设计

日期：2026-10-31
状态：待用户审阅
分支：`tricolor-animation`
父 spec：`docs/2026-10-24-gc-teaching-playground-design.md`（约定继承）；
语义权威：`gc2_incremental.c` / `gc6_hybrid.c` 及其测试

## 1. 背景与目标

三色标记与写屏障的各版本（无/Yuasa 删除/Dijkstra 插入/混合）是全库最抽象的
一课。目标：单文件交互动画，把 **gc2/gc6 的三个杀手测试**变成可回放脚本——
同一场景在四档屏障下重放，结局矩阵直接可见。v1 只做脚本场景（确定性与可比性
是根基）；自由操作模式留作后续。

成功标准：`?selftest` 自检全绿（3 场景 × 4 屏障 = 12 条结局断言 + 关键中间态）；
每条结局与对应 C 二进制的实测输出一致（gc2_bo/gc6_dijkstra/gc6_yuasa 的死法、
gc2/gc6 默认构建的存活数）；README 与 algorithms.md 各加一行链接。

## 2. 文件与集成

```
docs/tricolor-animation.html   # 单文件，零依赖，vanilla JS + 内联 SVG，无构建
README.md                      # 一行链接
docs/algorithms.md             # gc2/gc6 节各加一行链接
```

## 3. 模型语义（gc2/gc6 的逐条转写——正确性核心）

对象：`{id, kind: INT|PAIR, color: WHITE|GRAY|BLACK|FREED, head/tail|value}`。
VM 状态：`vmStack`（根）、`grayStack`、`gcState: IDLE|MARKING`、`barrier` 四档。

| 操作 | 语义（与 C 一致处标注） |
|---|---|
| `alloc/push/pop` | 根栈常规；`pop` 在 MARKING 时 shade 弹出值（gc2 规则 2，含 MARKING 守卫） |
| `gcStart` | IDLE→MARKING；全部根 shade（gc2 的"起点预灰 = 栈快照"） |
| `shade(o)` | WHITE→GRAY 入灰栈；否则 no-op |
| `gcStep` | 灰栈空 → **sweep**（WHITE→FREED 幽灵、BLACK→WHITE、IDLE）；否则弹一个置 BLACK，PAIR 则 shade 两字段 |
| `gcDrive` | while MARKING: gcStep（最后一步即 sweep） |
| `write(pair, field, value)` | **屏障矩阵**（见下，全部套 `if MARKING` 守卫）→ 字段换值 → `if MARKING: gcStep`（写驱动一步，gc2/gc6） |
| `pushRoot(o)` | 入根栈**不 shade**（gc6：未屏障栈写） |

**屏障矩阵**（`write` 的 shade 选择）：

| barrier | shade 目标 |
|---|---|
| NONE | 无 |
| YUASA | 旧值（gc2 规则 1） |
| DIJKSTRA | 新值（gc6 变体） |
| HYBRID | 旧值 + `pair.color==BLACK` 时新值（gc6 默认） |

标记期分配（出生即黑，gc2 规则 3）：场景不用（三杀手都在 gcStart 后无分配），
模型仍实现并在叙述中可用。

## 4. 场景与结局矩阵（自检契约）

**S1 删除之痛**（gc2 test7）：`A=(2,42)`，victim=42 仅经 A.tail 可达 →
`gcStart; write(A.tail, A.head)`（删路径）`→ gcDrive → write(A.tail, victim)`
（复装）。

**S2 未屏障根写**（gc6 test8）：`g=(1,42)` → `gcStart; pushRoot(42int);
write(g.tail, g.head)`（删唯一字段路径）`→ gcDrive`。

**S3 插入之重**（gc6 test9）：`g=(1,42)`，`pushInt(99); pop`（无路径白对象 w）
→ `gcStart; gcStep`（g 显式置黑）`→ pushRoot(w); write(g.tail, w)` →
`gcDrive`。

| 场景 \ 屏障 | NONE | YUASA | DIJKSTRA | HYBRID |
|---|---|---|---|---|
| S1 | **LOST** | 存活(3) | **LOST**（sweep 后复装悬垂） | 存活(3) |
| S2 | **LOST** | 存活 | **LOST**（根写无屏障） | 存活 |
| S3 | **LOST** | **LOST**（纯删除无插入半边） | 存活 | 存活 |

每行恰好杀死缺特定半边的屏障——四档各有专属败局，这是选这三个场景的原因。
结局横幅同时给出对应 C 证据（二进制名 + 断言消息，如 `gc6_yuasa`:
"The insertion half must save the stored object."）。

## 5. UI

- **SVG 对象图**：节点圆角矩形，填充=三色（白底描边/灰/黑），FREED=虚线幽灵，
  LOST 高亮红框脉冲；PAIR 两字段边（head 实线/tail 虚线）+ 值标签
- **双栈并列**：VM 栈（根，左）与灰栈（右），元素即时进出
- **叙述行**：每步一句话（"mutator: A.tail = B —— 删除半边触发：shade(B)"；
  "collector: 弹出 A 置黑，shade(A.head)"；屏障未触发时明说"未触发"）
- **控件**：屏障四档选择（切换即重放至当前步）、场景三选、步进/回退/自动播放
  （600ms/步）/重置；回退 = 从头确定性重放到第 k 步
- **结局横幅**：SURVIVED（存活计数）/ LOST OBJECT!（丢失对象 + C 证据）
- **配色**：三色即语义（白/灰/黑），红仅用于丢失——语义色优先，无装饰色

## 6. 验证协议

- **自检模式**（`?selftest`）：页面加载即跑 12 条结局断言 + 中间态断言
  （如 S3 的 `gcStep 后 g.color==BLACK`），输出 PASS/FAIL 清单——此为该媒介
  的 TDD 载体：先写自检（对空实现全红）再实现模型
- 结局与 C 实测的一致性是验收锚：gc2_bo（S1/LOST, "2 remaining"）、
  gc6_dijkstra（S2/LOST）、gc6_yuasa（S3/LOST）、默认构建（三场景存活）
- 无浏览器自动化依赖：人工打开验证一次交互，自检承担回归

## 7. 决策记录

| 决策 | 选择 | 理由 |
|---|---|---|
| 形态 | 单文件 HTML | 三色是视觉状态；切换重放是对照的最强形态；零依赖符合库风 |
| v1 范围 | 仅脚本场景 | 确定性与可比性是根基；自由模式交互面翻倍，YAGNI |
| 场景 | 三杀手 | 每个恰好杀死缺特定半边的屏障——矩阵即课 |
| 布局 | 每场景手工定位 | 6-8 节点不值得自动布局 |
| 回退 | 重放到第 k 步 | 确定性模型使然，免去反向操作语义 |
| 测试 | 页内自检 | 媒介内 TDD；结局矩阵与 C 实测互为锚 |

## 8. 风险与已知限制

- 模型是转写不是共享：与 C 语义的漂移风险由结局矩阵锚定（每格都有 C 证据）
- `?selftest` 依赖人工打开浏览器执行——README 注明；不进 `make test`（无头
  环境假设）
- 叙述行文案中英混合以代码注释风格为准（术语英文、叙述中文）
