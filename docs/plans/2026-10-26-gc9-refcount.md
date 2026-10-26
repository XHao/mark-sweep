# gc9 朴素引用计数实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 追加 `gc9_refcount.c`——朴素 RC（栈即强引用、级联即死即回收、存储三步曲），环泄漏钉住 + 手动断环。

**Architecture:** 从 `gc0_baseline.c` 派生：删 mark/sweep/gc 全套与阈值，`marked` 换 `refCount`，push/pop 与字段写全部走 retain/release；无 flag 构建。

**Tech Stack:** C（`-ggdb -std=gnu99 -Wall`）、POSIX make、macOS clang。零依赖。

**Spec:** `docs/2026-10-26-gc9-refcount-design.md`

## Global Constraints

- C99 零依赖；`-Wall` 无新警告；gc0–gc8 与 main.c 逐字节不动
- `STACK_MAX 256`；无阈值常量（无收集循环）
- 每任务结束 `make test` 全绿（最终 **11 PASS + 8 EXPECTED-FAIL = 19 passed**）；`make asan` **11/11**（零泄漏零 UB——t5 的手动断环是 ASan 清零的前提）
- 提交信息英文祈使句 + `Co-Authored-By: Claude Code <noreply@anthropic.com>`；分支 `gc9-refcount`
- heal 无关；统计语义：`countOps` 计 retain+release 各一次；`liveObjects` 计当前存活 malloc 数
- 测试编号 t1–t6 + perfTest（gc0 槽位改写）

## Review Focus

1. **pop 误释放转移值**（pushPair 未先 retain → 转移中途死亡）→ t2 级联断言 + ASan（顺序错即 UAF/泄漏）
2. **三步曲顺序**（未先换字段 → 断环级联 use-after-free）→ t5 断环三段（错误顺序被 ASan 抓）
3. **计数泄漏/重复释放** → t6 千次覆写恒定 + t1/t2 精确 `liveObjects`
4. **自赋值中途释放** → t4 结构与存活断言
5. **终态泄漏** → perfTest `liveObjects == 0` 断言

---

### Task 1: gc9_refcount.c

**Files:**
- Create: `gc9_refcount.c`

**Interfaces:**
- Consumes: `gc0_baseline.c`（派生：check/pushInt/objectPrint/perfTest 负载）
- Produces: 自包含收集器；公开 API `void retain(VM*, Object*)`、`void release(VM*, Object*)`、`objectSetHead/Tail`（三步曲）、`push/pop`（强引用语义）；VM 统计 `liveObjects/countOps`

- [ ] **Step 1: 复制起步 + 头注释**

```bash
cp gc0_baseline.c gc9_refcount.c
```

头注释块替换为：

```c
/* gc9 — Naive reference counting: the tracing family's dual.
 *
 * Every stage before this asked "what is reachable?" and swept the
 * rest in batches. Reference counting flips the question: each
 * object KNOWS how many references point at it, and the count
 * hitting zero means death — immediately, cascading through the
 * object's children, with no collector loop, no pause, and no
 * garbage left waiting. This is Swift/ObjC's ARC and Python's
 * everyday layer (their compilers/runtime insert the retain/release
 * calls that we write by hand here).
 *
 * The mechanics, and why each ordering choice exists:
 *
 *   - The stack holds strong references: push retains, pop releases.
 *   - Transfer (stack -> field, as pushPair does): retain FIRST, then
 *     pop — the pop releases, and without the retain the last
 *     reference could die mid-transfer. Net count change: zero.
 *   - Store (objectSetHead/Tail) runs a three-step idiom:
 *       retain(new);  old = field;  field = new;  release(old);
 *     Step 1 protects the new value (self-assignment, or the new
 *     value being the old one's only reference — test4). Step 3
 *     happens AFTER the field swap so that a cascading death reads
 *     the NEW state — breaking a cycle through objectSetTail(a, NULL)
 *     would otherwise release an already-freed object (test5).
 *
 * And the famous flaw, pinned as an assertion: a rootless a<->b cycle
 * keeps both counts at one and LEAKS (test5 checks liveObjects == 2,
 * then breaks the cycle by hand — the ARC programmer's daily fix —
 * and watches the cascade clear it). The automatic fix, a periodic
 * cycle-tracing pass, is gc10's whole chapter. Until then: this file
 * has no gc() at all — that absence is the paradigm.
 *
 * Stats: liveObjects counts live allocations; countOps counts
 * retain+release traffic — perfTest quantifies the paradigm's cost:
 * RC amortizes everything into writes, where tracing's mutator pays
 * nothing per store.
 *
 * diff gc0_baseline.c gc9_refcount.c is the whole paradigm swap.
 */
```

- [ ] **Step 2: 写失败测试**

测试区整体替换（t1–t6）：

```c
void test1() {
  printf("Test 1: Death is immediate — no collector ran.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);
  check(vm->liveObjects == 2, "Two objects should be live.");

  pop(vm);
  pop(vm);
  check(vm->liveObjects == 0, "Both should be freed instantly.");
  freeVM(vm);
}

void test2() {
  printf("Test 2: Death cascades through the tree.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);
  pushPair(vm);
  pushInt(vm, 3);
  pushInt(vm, 4);
  pushPair(vm);
  pushPair(vm);
  check(vm->liveObjects == 7, "The whole tree should be live.");

  pop(vm);   /* dropping the root releases the entire tree at once */
  check(vm->liveObjects == 0, "The cascade should free all seven.");
  freeVM(vm);
}

void test3() {
  printf("Test 3: Shared references keep counting.\n");
  VM* vm = newVM();
  pushInt(vm, 42);
  Object* shared = vm->stack[vm->stackSize - 1];

  push(vm, shared);   /* a second strong reference */
  pop(vm);            /* first drop: count 2 -> 1, still alive */
  check(vm->liveObjects == 1, "One reference should keep it alive.");

  pop(vm);            /* second drop: 1 -> 0, dead */
  check(vm->liveObjects == 0, "The last reference should free it.");
  freeVM(vm);
}

void test4() {
  printf("Test 4: Self-assignment survives the three-step idiom.\n");
  VM* vm = newVM();
  pushInt(vm, 2);
  pushInt(vm, 3);
  Object* p = pushPair(vm);     /* p = (2, 3) */

  objectSetTail(vm, p, p->tail);   /* retain-first protects the value */
  check(vm->liveObjects == 3, "Nothing should have died mid-store.");
  check(p->tail->value == 3, "Structure should be intact.");
  freeVM(vm);
}

void test5() {
  printf("Test 5: Cycles leak — pinned, then broken by hand.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);
  Object* a = pushPair(vm);
  pushInt(vm, 3);
  pushInt(vm, 4);
  Object* b = pushPair(vm);

  objectSetTail(vm, a, b);
  objectSetTail(vm, b, a);      /* the cycle: counts 2 and 2 */

  pop(vm);                      /* drop both roots */
  pop(vm);

  /* Each object's count is held by the other's tail: BOTH LEAK.
     This is naive RC's fatal flaw, and it is the spec of this test —
     the automatic fix (periodic cycle tracing) is gc10's chapter. */
  check(vm->liveObjects == 2, "Cycles leak under naive RC.");

  /* The ARC programmer's manual fix: break the cycle through the
     public idiom. The three-step ordering matters here — the field
     must be swapped BEFORE the old value's cascade runs, or a's own
     death would release the already-freed b. */
  objectSetTail(vm, a, NULL);
  check(vm->liveObjects == 0, "Breaking the cycle clears the leak.");
  freeVM(vm);
}

void test6() {
  printf("Test 6: A thousand overwrites keep the counts honest.\n");
  VM* vm = newVM();
  pushInt(vm, 2);
  pushInt(vm, 3);
  Object* p = pushPair(vm);     /* 3 objects live */

  for (int i = 0; i < 1000; i++) {
    pushInt(vm, i);
    /* Transfer idiom: retain before pop, then store (which releases
       the previous tail). Live count must stay flat at 3. */
    Object* v = vm->stack[vm->stackSize - 1];
    retain(vm, v);
    pop(vm);
    objectSetTail(vm, p, v);
    check(vm->liveObjects == 3, "Overwrites must not leak or over-free.");
  }

  check(p->tail->value == 999, "The final overwrite should stick.");
  freeVM(vm);
}
```

`main()` 替换为：

```c
int main(int argc, const char * argv[]) {
  test1();
  test2();
  test3();
  test4();
  test5();
  test6();
  perfTest();

  return 0;
}
```

perfTest 的 `freeVM(vm);` 前插入统计：

```c
  printf("%d objects live, %d refcount ops.\n",
         vm->liveObjects, vm->countOps);
  check(vm->liveObjects == 0, "Churn should leave nothing live.");
```

- [ ] **Step 3: 跑测试确认失败**

Run: `cc -ggdb -std=gnu99 -Wall gc9_refcount.c -o /tmp/gc9 2>&1 | grep -m1 error; echo done`
Expected: 编译失败——`vm->liveObjects`、`retain`、`release`、`objectSetTail` 等未定义。

- [ ] **Step 4: 实现 RC**

**4a.** 宏区：删除 `#define INIT_OBJ_NUM_MAX 8`。

**4b.** `sObject`：`unsigned char marked;` 替换为 `int refCount;`，删除 `next`
指针及其注释（无链表）。

**4c.** `VM` 整体替换：

```c
typedef struct {
  Object* stack[STACK_MAX];
  int stackSize;

  /* Statistics: the paradigm's two observable faces — instant
     reclamation and per-write cost. */
  int liveObjects;
  int countOps;
} VM;
```

**4d.** `newVM()` 替换：

```c
VM* newVM() {
  VM* vm = malloc(sizeof(VM));
  vm->stackSize = 0;
  vm->liveObjects = 0;
  vm->countOps = 0;
  return vm;
}
```

**4e.** `push`/`pop` 替换（`check` 定义之后、原 push 位置起），并在 `push`
之前插入原语：

```c
void retain(VM* vm, Object* object) {
  if (object == NULL) return;
  vm->countOps++;
  object->refCount++;
}

void release(VM* vm, Object* object) {
  if (object == NULL) return;
  vm->countOps++;
  if (--object->refCount == 0) {
    /* Death cascades through the children — immediately, no
       collector, no pause. This absence IS the paradigm. */
    if (object->type == OBJ_PAIR) {
      release(vm, object->head);
      release(vm, object->tail);
    }
    free(object);
    vm->liveObjects--;
  }
}

void push(VM* vm, Object* value) {
  check(vm->stackSize < STACK_MAX, "Stack overflow!");
  vm->stack[vm->stackSize++] = value;
  retain(vm, value);   /* a stack slot is a strong reference */
}

Object* pop(VM* vm) {
  check(vm->stackSize > 0, "Stack underflow!");
  Object* popped = vm->stack[--vm->stackSize];
  release(vm, popped);
  return popped;
}
```

**4f.** 删除 `mark`/`markAll`/`sweep`/`gc` 四个函数，原位置放 `newObject`：

```c
Object* newObject(VM* vm, ObjectType type) {
  Object* object = malloc(sizeof(Object));
  object->type = type;
  object->refCount = 0;   /* the caller's push/store retains */
  vm->liveObjects++;
  return object;
}
```

**4g.** `pushInt` 保持不变；其后插入字段写 API 与改造后的 `pushPair`：

```c
/* The store idiom — a three-step dance whose every ordering choice
   is load-bearing (test4 and test5 are the proofs):
     1. retain the NEW value: self-assignment, or the new value
        being the old one's only reference, must not die mid-store.
     2. swap the field BEFORE releasing: a cascading death must read
        the NEW state — otherwise breaking a cycle releases freed
        memory.
     3. release the old value last. */
void objectSetHead(VM* vm, Object* pair, Object* value) {
  retain(vm, value);
  Object* old = pair->head;
  pair->head = value;
  release(vm, old);
}

void objectSetTail(VM* vm, Object* pair, Object* value) {
  retain(vm, value);
  Object* old = pair->tail;
  pair->tail = value;
  release(vm, old);
}

Object* pushPair(VM* vm) {
  /* Transfer: retain BEFORE pop — the pop releases, and without the
     retain the last reference could die mid-transfer. Net count
     change per value: zero. */
  Object* tail = vm->stack[vm->stackSize - 1];
  retain(vm, tail);
  Object* head = vm->stack[vm->stackSize - 2];
  retain(vm, head);
  pop(vm);
  pop(vm);

  Object* object = newObject(vm, OBJ_PAIR);
  object->head = head;   /* counts already transferred */
  object->tail = tail;

  push(vm, object);
  return object;
}
```

**4h.** `objectPrint` 不动（gc0 版无 FORWARDED case，恰为本阶段所需）。

**4i.** `freeVM` 替换：

```c
void freeVM(VM *vm) {
  /* No collector to call — releasing the roots is the whole
     teardown. Cycles would leak here; tests break theirs by hand. */
  while (vm->stackSize > 0) pop(vm);
  free(vm);
}
```

- [ ] **Step 5: 跑测试确认通过**

Run: `cc -ggdb -std=gnu99 -Wall gc9_refcount.c -o /tmp/gc9 2>/tmp/gc9.warn && /tmp/gc9 > /tmp/gc9.out 2>&1; echo exit=$?; grep -cE "^Test" /tmp/gc9.out; wc -c < /tmp/gc9.warn; grep "refcount ops" /tmp/gc9.out`
Expected: `exit=0`；`6`；0 字节；统计行 `0 objects live, N refcount ops.`（N 约 4 万+）。

- [ ] **Step 6: 全量验证并提交**

Run: `make clean >/dev/null 2>&1; make test 2>&1 | grep -v "^cc " | tail -3 && make asan 2>&1 | tail -2`
Expected: `PASS gc9_refcount` 在列、`19 passed, 0 failed`；`ASAN PASS gc9_refcount` 在列、`11 passed, 0 failed`。

```bash
git add gc9_refcount.c
git commit -m "$(cat <<'EOF'
Add gc9: naive reference counting — the tracing family's dual

Objects know their counts; death cascades immediately out of
release, with no collector loop at all. The stack holds strong
references, transfers retain before pop, and stores run the
three-step idiom (retain-new, swap-field, release-old) whose
orderings are pinned by tests: self-assignment (test4) and the
cycle break that would be a use-after-free in the naive order
(test5). The famous flaw is asserted, not hidden: a rootless
cycle leaks (liveObjects == 2), then a manual break clears it —
ARC's daily fix, and gc10's motivation. countOps quantifies the
paradigm's write-amortized cost against tracing.

Co-Authored-By: Claude Code <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: README 行 + 收尾验证

**Files:**
- Modify: `README.md`

- [ ] **Step 1: README 更新**

表格 gc8 行后插：

```markdown
| `gc9_refcount.c` | reference counting: strong-reference stack, cascading immediate death, the three-step store idiom, cycles leak (pinned) | Swift/ObjC ARC, Python's everyday layer |
```

diff 命令块追加：

```markdown
    diff gc0_baseline.c  gc9_refcount.c   # the paradigm swap: counting vs tracing
```

首段 `ten-stage tour` → `eleven-stage tour`（EF 计数不变：仍 eight）。

- [ ] **Step 2: 最终全量验证**

Run: `make clean >/dev/null 2>&1; make test 2>&1 | tail -2 && make asan 2>&1 | tail -1 && ./gc9_refcount | tail -1`
Expected: `19 passed, 0 failed`；`11 passed, 0 failed`；gc9 统计行。

- [ ] **Step 3: 提交**

```bash
git add README.md
git commit -m "$(cat <<'EOF'
Add gc9 row and diff command to the README tour map

Co-Authored-By: Claude Code <noreply@anthropic.com>
EOF
)"
```

---

## 执行说明（对 worker）

- 两个任务严格线性；RED = Step 3 编译缺符号，GREEN = Step 5，收口 = Step 6 的 19/19 + asan 11/11
- ASan 是本阶段的重要裁判：三步曲顺序、转移惯用语、千次覆写的任何错误都会以
  UAF/泄漏形式被 `make asan` 抓住——若 test 通过而 asan 失败，顺序错了
- 不要动 gc0–gc8、main.c、master 分支
- 实际输出与 Expected 不符时停下按 systematic-debugging 排查，勿硬闯
