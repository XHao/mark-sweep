# 三色标记 + 屏障变体动画实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付 `docs/tricolor-animation.html`——单文件交互动画：三杀手场景 × 四档屏障重放，结局矩阵可视，`?selftest` 自检锚定与 C 实测的一致性。

**Architecture:** 纯 JS 模型（无 DOM 引用、可单独抽取）确定性预计算每场景每屏障的**全帧序列**（每步 = 状态快照 + 叙述）；UI 只做"渲染第 k 帧"；屏障/场景切换 = 重算帧序列后重放到当前步。自检 = 断言 12 格结局矩阵 + 4 条中间态。

**Tech Stack:** 单文件 HTML + vanilla JS + 内联 SVG/CSS，零依赖零构建。

**Spec:** `docs/2026-10-31-tricolor-animation-design.md`

## Global Constraints

- 单文件 `docs/tricolor-animation.html`，不引入任何外部资源（字体/CDN 全不用）
- 模型语义以 `gc2_incremental.c`/`gc6_hybrid.c` 为权威；结局矩阵每格的 C 证据照抄 spec §4
- 模型代码块内**不得出现任何 DOM 引用**（可被 sed 抽取后用 node 独立运行）
- 每任务结束 `make test` 仍 **19/19**、`make asan` 仍 **11/11**（本工作不触碰 C，回归必须为零）
- 提交信息英文祈使句 + `Co-Authored-By: Claude Code <noreply@anthropic.com>`；分支 `tricolor-animation`
- 中文叙述 + 英文术语（与库内注释风格一致）

## Review Focus

1. **结局矩阵漂移**（模型与 C 语义的任何偏差）→ 自检 12 格断言（每格两属性：kind + survivors）
2. **写驱动步进的次序**（shade 在换字段前、gcStep 在换字段后）→ 中间断言：S1-YUASA write1 后 v 为 GRAY；S1-DIJKSTRA write1 后 n2 为 GRAY
3. **sweep 语义**（WHITE→FREED、BLACK→WHITE 复位、灰栈空才触发）→ 中间断言：S3-NONE 终态 w FREED 且仍在 vmStack
4. **屏障守卫**（IDLE 期写不 shade 不步进）→ S1 复装写在 sweep 后不产生任何 collector 帧
5. **UI 状态错位**（回退/切换后帧渲染与索引不一致）→ 手动验证脚本（Step 5）覆盖 回退×屏障切换×场景切换 三操作

---

### Task 1: docs/tricolor-animation.html

**Files:**
- Create: `docs/tricolor-animation.html`

**Interfaces:**
- Consumes: spec §3 模型语义表、§4 场景与矩阵（唯一权威）
- Produces: 全局 `runScenario(scnId, barrierId)` → `{frames, outcome}`（frames[i] = {desc, gcState, objects, vmStack, grayStack, outcome|null}）；`SCENARIOS` 元数据；`SELFTEST` 断言表

- [ ] **Step 1: 骨架 + 自检先行（RED）**

写入文件：HTML 骨架（head/CSS 略占位为最小可用、控件区、图区、双栈区、叙述区、横幅、自检面板 `<pre id="selftest">`）+ 模型脚本块（`<script id="model">`，内容为**空实现**）+ 自检块 + UI 块。模型空实现：

```html
<script id="model">
// Standalone-extractable: NO DOM references below this line.
// Semantics authority: gc2_incremental.c / gc6_hybrid.c.

function runScenario(scnId, barrierId) {
  // TODO(model): implement per docs/2026-10-31-tricolor-animation-design.md §3.
  return { frames: [], outcome: { kind: 'BROKEN', survivors: -1 } };
}

const SCENARIOS = { /* TODO(model): S1..S3 metadata */ };
</script>
```

自检块（`<script>`，页面加载时若 `location.search` 含 `selftest` 则运行，输出到 `<pre>`；同一段代码附加在抽取文件的尾部即为 headless runner）：

```js
function runSelfTest(out) {
  const M = [
    // [scenario, barrier, kind, survivors]  — spec §4's matrix, C-anchored.
    ['S1', 'NONE', 'LOST', 2],      ['S1', 'YUASA', 'SURVIVED', 3],
    ['S1', 'DIJKSTRA', 'LOST', 2],  ['S1', 'HYBRID', 'SURVIVED', 3],
    ['S2', 'NONE', 'LOST', 2],      ['S2', 'YUASA', 'SURVIVED', 3],
    ['S2', 'DIJKSTRA', 'LOST', 2],  ['S2', 'HYBRID', 'SURVIVED', 3],
    ['S3', 'NONE', 'LOST', 3],      ['S3', 'YUASA', 'LOST', 3],
    ['S3', 'DIJKSTRA', 'SURVIVED', 4], ['S3', 'HYBRID', 'SURVIVED', 4],
  ];
  let pass = 0, fail = 0, lines = [];
  for (const [s, b, kind, n] of M) {
    const r = runScenario(s, b);
    const ok = r.outcome.kind === kind && r.outcome.survivors === n;
    ok ? pass++ : fail++;
    lines.push((ok ? 'PASS' : 'FAIL') + ' ' + s + '/' + b +
               ' 期望 ' + kind + '(' + n + ') 实得 ' +
               r.outcome.kind + '(' + r.outcome.survivors + ')');
  }
  // Intermediate-state assertions (Review Focus 2/3).
  const t = (cond, msg) => { cond ? pass++ : fail++;
    lines.push((cond ? 'PASS' : 'FAIL') + ' ' + msg); };

  /* Offset 0 — the WRITE frame itself: shading happens BEFORE the
     field swap, and the write's driving gcStep (offset 1) would
     already blacken the just-shaded top-of-stack object. */
  let r = runScenario('S1', 'YUASA');
  t(frameAt(r, 'A.tail := n2', 0).objects.v.color === 'GRAY',
    'S1/YUASA 写帧内 victim 已为 GRAY（删除半边先于换字段）');
  r = runScenario('S1', 'DIJKSTRA');
  t(frameAt(r, 'A.tail := n2', 0).objects.n2.color === 'GRAY',
    'S1/DIJKSTRA 写帧内 n2 已为 GRAY（新值被 shade）');
  r = runScenario('S3', 'NONE');
  const fin = r.frames[r.frames.length - 1];
  t(fin.objects.w.color === 'FREED' && fin.vmStack.indexOf('w') >= 0,
    'S3/NONE w 被回收却仍在根栈（无屏障之祸）');
  r = runScenario('S1', 'DIJKSTRA');
  t(r.frames[r.frames.length - 1].objects.A.tail === 'v' &&
    r.frames[r.frames.length - 1].objects.v.color === 'FREED',
    'S1/DIJKSTRA 复装写指向 FREED（sweep 后悬垂）');

  lines.push('== ' + pass + ' passed, ' + fail + ' failed ==');
  if (out) out(lines.join('\n'));
  return { pass, fail, lines };
}
function frameAt(run, descPart, offset) {
  const i = run.frames.findIndex(f => f.desc.indexOf(descPart) >= 0);
  return run.frames[Math.min(i + offset, run.frames.length - 1)];
}
```

headless 抽取命令（RED 与 GREEN 共用）：

```bash
sed -n '/<script id="model">/,/<\/script>/p' docs/tricolor-animation.html \
  | sed '1d;$d' > /tmp/anim_model.js
cat >> /tmp/anim_model.js <<'EOF'
const out = s => console.log(s);
const r = runSelfTest(out);
process.exit(r.fail === 0 ? 0 : 1);
EOF
node /tmp/anim_model.js
```

- [ ] **Step 2: 跑自检确认失败（RED）**

Run: 上面的抽取命令（`command -v node` 先确认；无 node 则改用浏览器打开 `?selftest` 人工读数，并在 ledger 记录媒介降级）
Expected: **16 条 FAIL**（12 格 BROKEN + 4 中间断言异常），退出码 1。

- [ ] **Step 3: 实现模型与场景（完整代码）**

`<script id="model">` 块整体替换为：

```js
// Standalone-extractable: NO DOM references below this line.
// Semantics authority: gc2_incremental.c / gc6_hybrid.c.

const BARRIERS = {
  NONE:     { name: '无屏障' },
  YUASA:    { name: 'Yuasa 删除半边' },
  DIJKSTRA: { name: 'Dijkstra 插入半边' },
  HYBRID:   { name: '混合屏障（Go 1.8+）' },
};

const SCENARIOS = {
  S1: {
    name: 'S1 删除之痛', victim: 'v',
    blurb: 'victim 仅经 A.tail 可达；GC 中删掉唯一字段路径，sweep 后再复装。',
    saved: 'gc2 默认构建：test7 通过（3 remaining）',
    lost: {
      NONE:     'gc2_bo（-DBARRIER_OFF）: Collected 1 objects, 2 remaining —— "Snapshot object should have survived."',
      DIJKSTRA: 'gc6_dijkstra 构建跳过 t7 的原因正在于此：插入单屏障救不了删除场景',
      '*':      '屏障缺失，victim 被回收',
    },
  },
  S2: {
    name: 'S2 未屏障根写', victim: 'v',
    blurb: 'GC 中 pushRoot(victim)（未屏障栈写），再删其唯一字段路径。',
    saved: 'gc6 默认构建：test8 通过',
    lost: {
      NONE:     '未屏障栈写 + 无屏障删除：victim 双重无保护',
      DIJKSTRA: 'gc6_dijkstra: "The barrier must save the newly-rooted object."（2 remaining）',
      '*':      'victim 被回收',
    },
  },
  S3: {
    name: 'S3 插入之重', victim: 'w',
    blurb: 'gcStep 先把 g 置黑，再把无路径白对象 w 存入黑 g——只有插入半边能救。',
    saved: 'gc6 默认构建：test9 通过',
    lost: {
      NONE:  '无屏障：w 被回收',
      YUASA: 'gc6_yuasa: "The insertion half must save the stored object."（3 remaining）',
      '*':   'w 被回收',
    },
  },
};

function runScenario(scnId, barrierId) {
  const scn = SCENARIOS[scnId];
  const B = BARRIERS[barrierId];
  const S = { objects: {}, vmStack: [], grayStack: [], gcState: 'IDLE' };
  const frames = [];

  function snap(desc, outcome) {
    frames.push({
      desc: desc, gcState: S.gcState, outcome: outcome || null,
      objects: JSON.parse(JSON.stringify(S.objects)),
      vmStack: S.vmStack.slice(), grayStack: S.grayStack.slice(),
    });
  }

  function shade(id, notes) {
    if (!id) return;
    const o = S.objects[id];
    if (o && o.color === 'WHITE') {
      o.color = 'GRAY';
      S.grayStack.push(id);
      notes.push('shade(' + id + ')');
    }
  }

  /* One collector step; sweep when the gray stack is empty (gcStep). */
  function gcStep(notes) {
    if (S.gcState !== 'MARKING') return;
    if (S.grayStack.length === 0) {
      const freed = [];
      for (const id of Object.keys(S.objects)) {
        const o = S.objects[id];
        if (o.color === 'WHITE') { o.color = 'FREED'; freed.push(id); }
        else if (o.color === 'BLACK') o.color = 'WHITE';  /* reset for next cycle */
      }
      S.gcState = 'IDLE';
      notes.push('sweep：回收白色 [' + (freed.join(' ') || '无') + ']，黑复位为白');
      return;
    }
    const id = S.grayStack.pop();
    const o = S.objects[id];
    o.color = 'BLACK';
    notes.push('弹出 ' + id + ' 置黑');
    if (o.kind === 'PAIR') { shade(o.head, notes); shade(o.tail, notes); }
  }

  /* Mutator ops (built at IDLE in all three scenarios). */
  function allocInt(id, value) {
    S.objects[id] = { id: id, kind: 'INT', value: value,
                      color: S.gcState === 'MARKING' ? 'BLACK' : 'WHITE' };
    snap('mutator: alloc int ' + id + '（值 ' + value + '）' +
         (S.gcState === 'MARKING' ? ' —— 标记期出生即黑（gc2 规则 3）' : ''));
  }
  function pushRoot(id) {
    S.vmStack.push(id);
    snap('mutator: pushRoot(' + id + ') —— 未屏障栈写：不 shade（goroutine 栈同款）');
  }
  function allocPair(id, headId, tailId) {
    S.objects[id] = { id: id, kind: 'PAIR', head: headId, tail: tailId,
                      color: S.gcState === 'MARKING' ? 'BLACK' : 'WHITE' };
    pushRoot(id);
  }
  function pop() {
    const id = S.vmStack.pop();
    const notes = [];
    if (S.gcState === 'MARKING') shade(id, notes);  /* gc2 rule 2 */
    snap('mutator: pop(' + id + ')' +
         (notes.length ? ' —— 根删除半边：' + notes.join(', ') : ''));
  }

  function write(pairId, field, valueId) {
    const p = S.objects[pairId];
    const notes = [];
    if (S.gcState === 'MARKING') {
      const old = p[field];
      if (barrierId === 'YUASA' || barrierId === 'HYBRID') shade(old, notes);
      if (barrierId === 'DIJKSTRA' ||
          (barrierId === 'HYBRID' && p.color === 'BLACK')) shade(valueId, notes);
    }
    p[field] = valueId;
    snap('mutator: ' + pairId + '.' + field + ' := ' + (valueId || 'null') +
         (S.gcState !== 'MARKING' ? ' ——（IDLE：屏障与步进均不需要）'
          : notes.length ? ' —— ' + B.name + ' 触发：' + notes.join('，')
          : ' —— 屏障未触发'));
    if (S.gcState === 'MARKING') {          /* writes drive one step (gc2/gc6) */
      const n2 = [];
      gcStep(n2);
      snap('collector: gcStep —— ' + n2.join('；'));
    }
  }

  function gcStart() {
    S.gcState = 'MARKING';
    const notes = [];
    for (const id of S.vmStack) shade(id, notes);
    snap('collector: gcStart —— 根全部预灰（起点栈快照）：' +
         (notes.join(', ') || '无根'));
  }
  function stepOnce() {
    const notes = [];
    gcStep(notes);
    if (notes.length) snap('collector: gcStep —— ' + notes.join('；'));
  }
  function gcDrive() {
    while (S.gcState === 'MARKING') stepOnce();
  }

  /* ---- Scenario scripts (the killer tests, op for op) ---- */
  if (scnId === 'S1') {
    allocInt('n2', 2); allocInt('v', 42);
    allocPair('A', 'n2', 'v');
    gcStart();
    write('A', 'tail', 'n2');   /* delete the victim's only field path */
    gcDrive();
    write('A', 'tail', 'v');    /* restore — post-sweep under weak barriers */
  } else if (scnId === 'S2') {
    allocInt('n1', 1); allocInt('v', 42);
    allocPair('g', 'n1', 'v');
    gcStart();
    pushRoot('v');
    write('g', 'tail', 'n1');
    gcDrive();
  } else if (scnId === 'S3') {
    allocInt('n1', 1); allocInt('v', 42);
    allocPair('g', 'n1', 'v');
    allocInt('w', 99);
    pop();                      /* w: popped at IDLE — pathless, C-held */
    gcStart();
    stepOnce();                 /* g -> BLACK, explicitly */
    pushRoot('w');
    write('g', 'tail', 'w');
    gcDrive();
  }

  /* ---- Outcome ---- */
  const victim = S.objects[scn.victim];
  const survivors = Object.keys(S.objects)
    .filter(id => S.objects[id].color !== 'FREED').length;
  const lost = victim.color === 'FREED';
  const outcome = {
    kind: lost ? 'LOST' : 'SURVIVED', survivors: survivors,
    evidence: lost ? (scn.lost[barrierId] || scn.lost['*']) : scn.saved,
  };
  snap(scn.name + ' 结局 —— victim ' + (lost ? '被回收' : '存活') +
       '，' + survivors + ' 个对象存活', outcome);

  return { frames: frames, outcome: outcome };
}
```

- [ ] **Step 4: 跑自检确认通过（GREEN）**

Run: 同 Step 2 抽取命令。
Expected: **16 条 PASS**，`== 16 passed, 0 failed ==`，退出码 0。

- [ ] **Step 5: 实现 UI（完整行为契约 + 关键代码）**

CSS（内联 `<style>`，语义色）：节点三色 `WHITE:#fff/边框#555`、`GRAY:#98a1ab`、
`BLACK:#272b30/文字#fff`、`FREED:虚线+opacity .35`、LOST 高亮 `#c0392b` 描边；
布局为上下三段：控件条 / 主区（左 SVG 图 + 右双栈与状态）/ 底部叙述与横幅。

UI 块核心（完整实现，无省略）：

```js
// ---- UI (all DOM references live below this line) ----
const LAYOUT = {
  S1: { A: [40, 90], n2: [260, 40], v: [260, 150] },
  S2: { g: [40, 90], n1: [260, 40], v: [260, 150] },
  S3: { g: [40, 90], n1: [260, 30], v: [260, 100], w: [260, 180] },
};
let cur = { scn: 'S1', barrier: 'YUASA', run: null, idx: 0, timer: null };

function rebuild() {
  cur.run = runScenario(cur.scn, cur.barrier);
  cur.idx = Math.min(cur.idx, cur.run.frames.length - 1);
  render();
}
function render() {
  const f = cur.run.frames[cur.idx];
  drawGraph(f);
  drawStacks(f);
  document.getElementById('narr').textContent =
    (cur.idx + 1) + '/' + cur.run.frames.length + '  ' + f.desc;
  const b = document.getElementById('banner');
  if (f.outcome) {
    b.style.display = 'block';
    b.className = f.outcome.kind === 'LOST' ? 'lost' : 'saved';
    b.textContent = (f.outcome.kind === 'LOST' ? '✘ LOST OBJECT! ' : '✔ SURVIVED ') +
      '—— ' + f.outcome.survivors + ' 个对象存活 —— C 证据：' + f.outcome.evidence;
  } else b.style.display = 'none';
  syncButtons();
}
function drawGraph(f) {
  const svg = document.getElementById('graph');
  const L = LAYOUT[cur.scn];
  let h = '';
  for (const id of Object.keys(f.objects)) {
    const o = f.objects[id], [x, y] = L[id];
    const fill = { WHITE: '#fff', GRAY: '#98a1ab', BLACK: '#272b30', FREED: '#fff' }[o.color];
    const txt = o.color === 'BLACK' ? '#fff' : '#222';
    const ghost = o.color === 'FREED';
    const isVictim = id === SCENARIOS[cur.scn].victim;
    h += '<g opacity="' + (ghost ? 0.35 : 1) + '">' +
      '<rect x="' + x + '" y="' + y + '" width="92" height="46" rx="8" ' +
      'fill="' + fill + '" stroke="' + (isVictim && ghost ? '#c0392b' : '#555') + '" ' +
      'stroke-width="' + (isVictim ? 3 : 1.5) + '" ' +
      (ghost ? 'stroke-dasharray="6 4"' : '') + '/>' +
      '<text x="' + (x + 46) + '" y="' + (y + 28) + '" text-anchor="middle" ' +
      'fill="' + txt + '" font-size="15">' + id +
      (o.kind === 'INT' ? '=' + o.value : '') + '</text></g>';
    if (o.kind === 'PAIR') {
      for (const [fld, dx] of [['head', 20], ['tail', 72]]) {
        const t = o[fld];
        if (t && f.objects[t]) {
          const [tx, ty] = L[t];
          h += '<line x1="' + (x + dx) + '" y1="' + (y + 46) + '" ' +
            'x2="' + (tx + 46) + '" y2="' + ty + '" stroke="' +
            (f.objects[t].color === 'FREED' ? '#c0392b' : '#888') + '" ' +
            (fld === 'tail' ? 'stroke-dasharray="4 3"' : '') + '/>' +
            '<text x="' + ((x + dx + tx + 46) / 2) + '" y="' + ((y + 46 + ty) / 2 - 4) +
            '" font-size="11" fill="#666">' + fld + '</text>';
        }
      }
    }
  }
  svg.innerHTML = h;
}
function drawStacks(f) {
  const mk = (ids) => ids.length
    ? ids.map(id => '<div class="cell c' + f.objects[id].color + '">' + id + '</div>').join('')
    : '<div class="empty">（空）</div>';
  document.getElementById('vmstack').innerHTML = mk(f.vmStack);
  document.getElementById('graystack').innerHTML = mk(f.grayStack);
  document.getElementById('state').textContent = 'GC: ' + f.gcState;
  document.getElementById('state').className =
    'state ' + (f.gcState === 'MARKING' ? 'marking' : 'idle');
}
function syncButtons() {
  for (const id of ['S1', 'S2', 'S3'])
    document.getElementById('scn-' + id).classList.toggle('on', cur.scn === id);
  for (const id of Object.keys(BARRIERS))
    document.getElementById('bar-' + id).classList.toggle('on', cur.barrier === id);
  document.getElementById('btn-step').disabled = cur.idx >= cur.run.frames.length - 1;
  document.getElementById('btn-back').disabled = cur.idx <= 0;
}
function play() {
  if (cur.timer) { clearInterval(cur.timer); cur.timer = null; return; }
  cur.timer = setInterval(() => {
    if (cur.idx >= cur.run.frames.length - 1) { clearInterval(cur.timer); cur.timer = null; return; }
    cur.idx++; render();
  }, 700);
}
// Wire controls: scn-*/bar-* call rebuild; step/back/reset/play; state text.
document.querySelectorAll('[id^=scn-]').forEach(b =>
  b.onclick = () => { cur.scn = b.id.slice(4); cur.idx = 0; rebuild(); });
document.querySelectorAll('[id^=bar-]').forEach(b =>
  b.onclick = () => { cur.barrier = b.id.slice(4); rebuild(); });
document.getElementById('btn-step').onclick = () =>
  { cur.idx = Math.min(cur.idx + 1, cur.run.frames.length - 1); render(); };
document.getElementById('btn-back').onclick = () => { cur.idx--; render(); };
document.getElementById('btn-reset').onclick = () => { cur.idx = 0; render(); };
document.getElementById('btn-play').onclick = play;

if (location.search.indexOf('selftest') >= 0) {
  runSelfTest(s => { document.getElementById('selftest').textContent = s; });
  document.getElementById('selftest').style.display = 'block';
} else {
  rebuild();
}
```

HTML 主体（控件与面板骨架）：

```html
<body>
<h1>三色标记 × 写屏障 —— 四版本对照动画</h1>
<p id="blurb"></p>
<div class="controls">
  <span>场景：</span><button id="scn-S1">S1 删除之痛</button>
  <button id="scn-S2">S2 未屏障根写</button><button id="scn-S3">S3 插入之重</button>
  <span class="gap"></span><span>屏障：</span>
  <button id="bar-NONE">无</button><button id="bar-YUASA">Yuasa</button>
  <button id="bar-DIJKSTRA">Dijkstra</button><button id="bar-HYBRID">混合</button>
  <span class="gap"></span>
  <button id="btn-reset">⏮</button><button id="btn-back">◀</button>
  <button id="btn-step">步进 ▶</button><button id="btn-play">自动 ▶▶</button>
</div>
<div id="banner"></div>
<div class="main">
  <svg id="graph" width="420" height="260" viewBox="0 0 420 260"></svg>
  <div class="side">
    <div id="state" class="state idle">GC: IDLE</div>
    <h3>VM 栈（根）</h3><div id="vmstack"></div>
    <h3>灰栈</h3><div id="graystack"></div>
  </div>
</div>
<div id="narr"></div>
<pre id="selftest" style="display:none"></pre>
</body>
```

场景切换时更新 `#blurb` 为 `SCENARIOS[cur.scn].blurb`（rebuild 内）。

- [ ] **Step 6: 交互手动验证（Review Focus #5）**

Run: `open docs/tricolor-animation.html`
人工脚本：①S1/Yuasa 步进到结局（SURVIVED 3）→ 回退 3 步 → 切屏障到 无 → 帧序列与结局随之变为 LOST 2 → 切 S3 → 自动播放至 LOST。②地址栏加 `?selftest` 重载 → 16 PASS。
Expected: 三操作后帧/叙述/横幅与索引一致无错位；自检 16 PASS。

- [ ] **Step 7: 回归与提交**

Run: `make clean >/dev/null 2>&1; make test 2>&1 | tail -1 && make asan 2>&1 | tail -1`
Expected: `19 passed, 0 failed`；`11 passed, 0 failed`。

```bash
git add docs/tricolor-animation.html
git commit -m "$(cat <<'EOF'
Add tricolor + write-barrier variant animation

A single self-contained HTML page replaying the gc2/gc6 killer
tests: three scripted scenarios under four barrier settings, each
scenario killing exactly the barriers missing a specific half.
The embedded model is a faithful transcription of the gc2/gc6
lockstep semantics (pre-grayed roots, step-driving writes with the
shade-before-swap order, pop-shade, unbarriered pushRoot,
born-black allocation, sweep-on-empty-gray); frames are
precomputed deterministically, so stepping back and switching
barriers is pure replay. A ?selftest mode asserts the full outcome
matrix plus four intermediate states — sixteen checks the model
had to pass before the UI was written. C evidence (binary name +
assertion message) rides every outcome banner.

Co-Authored-By: Claude Code <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: README 与 algorithms.md 链接 + 收尾验证

**Files:**
- Modify: `README.md`、`docs/algorithms.md`

- [ ] **Step 1: 链接**

README（`make test` 段后）追加一行：

```markdown
For the tricolor and write-barrier variants there is also an
[interactive animation](docs/tricolor-animation.html) — the gc2/gc6
killer tests replayed under all four barrier settings, with each
outcome citing its C binary.
```

algorithms.md 的 gc2 节末尾追加：

```markdown
**动画**：`docs/tricolor-animation.html` 重放本节与 gc6 的三个杀手场景，
四档屏障切换对照，结局横幅附 C 二进制证据。
```

gc6 节的屏障矩阵表后追加同样一行（措辞可略异）。

- [ ] **Step 2: 最终验证**

Run: `make clean >/dev/null 2>&1; make test 2>&1 | tail -1 && make asan 2>&1 | tail -1 && node /tmp/anim_model.js 2>&1 | tail -1`
Expected: `19 passed, 0 failed`；`11 passed, 0 failed`；`== 16 passed, 0 failed ==`。

- [ ] **Step 3: 提交**

```bash
git add README.md docs/algorithms.md
git commit -m "$(cat <<'EOF'
Link the tricolor animation from README and algorithms.md

Co-Authored-By: Claude Code <noreply@anthropic.com>
EOF
)"
```

---

## 执行说明（对 worker）

- 两个任务严格线性；本工作的 RED/GREEN 载体是 headless 自检（Step 2/4），
  C 套件仅作零回归证明（Step 7）
- 模型帧预计算是关键结构：任何"边播边算"的冲动都应拒绝——回退与切换重放
  依赖确定性
- 若 node 不存在：Step 2/4 改为浏览器 `?selftest` 读数并在 ledger 记录；
  Step 6 的人工验证无论如何都要做
- 不要动任何 gcN.c、main.c、master 分支
