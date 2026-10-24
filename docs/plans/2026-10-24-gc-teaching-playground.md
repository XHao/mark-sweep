# GC 教学 Playground 实施计划（gc0–gc4）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在原版 mark-sweep 教学代码上构建五阶段 GC 教学 playground（迭代标记 → 三色增量+Yuasa 屏障 → Cheney 复制 → 分代），每阶段一个独立可编译运行的 `.c` 文件，相邻文件 diff 即算法差异。

**Architecture:** 每个 `gcN.c` 自包含（VM/Object/测试/main 全在一个文件），从上一阶段文件 `cp` 起步再做聚焦修改。原版 `main.c` 保持不动。Makefile 用 wildcard 自动纳入新阶段文件，每个任务追加自己的"预期失败"编译版规则。

**Tech Stack:** C（`-std=gnu99`）、POSIX make、macOS clang。零外部依赖。

**Spec:** `docs/2026-10-24-gc-teaching-playground-design.md`

## Global Constraints

- C99，零外部依赖，编译命令 `$(CC) -ggdb -std=gnu99 -Wall`（沿用原版无 -Wall？**用 -Wall**，新增代码不允许新警告）
- 沿用原版代码风格：2 空格缩进、函数定义前空行、注释密度、printf 输出风格
- `main.c` 保持逐字节不动；`gc0_baseline.c` 是它的逐字节拷贝 + 头部注释块
- 从 `gc1` 起自定义 `assert` 改名 `check`（gc0 保持原名以维持"逐字节拷贝"）
- 根集合 = VM 栈。测试中 C 局部变量持有的指针**不是根**
- 容量常量：`STACK_MAX 256`、`INIT_OBJ_NUM_MAX 8`（gc0/1/2）、`HEAP_MAX 4096`（gc3）、`NURSERY_MAX 256`（gc4）
- 每个任务结束：`make test` 必须全绿（含该任务的预期失败编译版）
- 提交信息英文祈使句（沿用仓库风格），结尾加 `Co-Authored-By: Claude Code <noreply@anthropic.com>`
- 分支：`gc-teaching-playground`（已存在），不要动 master

## Review Focus

（spec 未显式覆盖、最容易坑人的五类输入；每条已把锚定测试加进所属任务）

1. **gc2：MARKING 期间 pop 丢根** — pop 不 shade，则"pop 后移入已黑对象"会丢对象。→ Task 3 `test5` 断言 pop 后对象仍存活（13 个一个不丢）。
2. **gc2：MARKING 期间分配出生为白** — 会被本轮 sweep 误回收。→ Task 3 `test5` 断言标记期间 `pushInt` 的新对象存活。
3. **gc4：`isYoung(NULL)` 指针范围误判** — NULL 与 nursery 地址比较可能落进范围被当年轻对象。→ `isYoung` 显式判 NULL（Task 5 代码内）+ Task 5 `test7` 用 `objectSetTail(vm, pair, NULL)` 走一遍 minor GC 断言不崩。
4. **gc4：晋升副本的字段仍指向年轻对象** — 忘记递归晋升则内部指针悬垂。→ Task 5 `test3` 嵌套结构一次 minor 后按结构遍历断言内容完整。
5. **深结构回归**（worklist/复制/晋升都要扛住）：→ Task 2 `test5` 百万深链；Task 4 `test6` 深链复制后内容校验。

---

### Task 1: gc0 基线 + Makefile 骨架

**Files:**
- Create: `gc0_baseline.c`
- Modify: `Makefile`（整体重写）

**Interfaces:**
- Consumes: 原版 `main.c`
- Produces: `gc0_baseline.c`（原版逐字节拷贝 + 头部注释）；Makefile 目标 `all/test/clean/run/markandsweep`，wildcard `gc*.c` 自动纳入后续阶段

- [ ] **Step 1: 创建 gc0_baseline.c**

```bash
cp main.c gc0_baseline.c
```

然后在文件最顶部（`#include` 之前）插入：

```c
/* gc0 — Baseline: the original mark-sweep collector, verbatim.
 *
 * This file is a byte-for-byte copy of main.c (plus this comment) and
 * exists as the diff base for the later stages. Every gcN.c in this
 * directory derives from the previous stage, so:
 *
 *   diff gc0_baseline.c gc1_iterative.c     shows iterative marking
 *   diff gc1_iterative.c gc2_incremental.c  shows tricolor + barrier
 *   ...
 *
 * Companion to http://journal.stuffwithstuff.com/2013/12/08/babys-first-garbage-collector/
 */
```

- [ ] **Step 2: 重写 Makefile（v1，无预期失败版）**

整体替换 Makefile 内容为：

```make
CC      ?= cc
CFLAGS  = -ggdb -std=gnu99 -Wall
SRCS    = $(wildcard gc*.c)
BINS    = $(SRCS:.c=)

all: $(BINS)

markandsweep: main.c
	$(CC) $(CFLAGS) main.c -o $@

gc%: gc%.c
	$(CC) $(CFLAGS) $< -o $@

test: all
	@ok=0; fail=0; \
	for b in $(BINS); do \
	  if ./$$b > /dev/null 2>&1; then echo "PASS $$b"; ok=$$((ok+1)); \
	  else echo "FAIL $$b"; fail=$$((fail+1)); fi; \
	done; \
	echo "$$ok passed, $$fail failed"; \
	test $$fail -eq 0

run: markandsweep
	valgrind --leak-check=yes ./markandsweep

.PHONY: all test run clean
clean:
	rm -f $(BINS) markandsweep *~
```

注意：`test` 里 shell 变量展开需要 `$$`；pattern rule 的 recipe 行必须以 TAB 开头。

- [ ] **Step 3: 验证构建与测试**

Run: `make clean && make test`
Expected: `PASS gc0_baseline`、`1 passed, 0 failed`，无编译警告。单独 `./gc0_baseline` 输出应与 `./markandsweep` 一致。

- [ ] **Step 4: Commit**

```bash
git add gc0_baseline.c Makefile
git commit -m "$(cat <<'EOF'
Add gc0 baseline copy and multi-stage Makefile

gc0_baseline.c is main.c verbatim (plus header comment) and is the
diff base for the coming stages. The Makefile picks up any gc*.c
automatically and gains a test target.

Co-Authored-By: Claude Code <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: gc1 — 迭代标记（显式 worklist）

**Files:**
- Create: `gc1_iterative.c`

**Interfaces:**
- Consumes: `gc0_baseline.c`
- Produces: `check()`（原 `assert` 全部改名）；`Worklist` 结构 + `workPush`/`workPop`；重写的 `markAll`（吃掉原 `mark` 的职责）；`test5` 深链测试

- [ ] **Step 1: 复制起步**

```bash
cp gc0_baseline.c gc1_iterative.c
```

- [ ] **Step 2: 写失败测试**

在 `gc1_iterative.c` 中：先把 `assert` 全部改名 `check`（`grep -n assert gc1_iterative.c` 逐个替换，共 6 处：定义 + 5 处调用）。然后替换头部注释块为：

```c
/* gc1 — Iterative marking with an explicit worklist.
 *
 * The baseline's recursive mark() walks the object graph on the C call
 * stack: a deep chain of pairs needs one C stack frame per link and
 * overflows for large graphs. This stage replaces recursion with an
 * explicit, dynamically-grown worklist (a "gray stack" in GC terms).
 *
 * Behavior is identical to gc0 — same tests, same output. The only
 * diff-worthy change is mark()/markAll() plus the new test5.
 *
 * Modern relevance: every production collector marks iteratively;
 * recursion is a non-starter once the heap is a graph you don't control.
 */
```

在 `test4()` 之后、`perfTest()` 之前插入：

```c
void test5() {
  printf("Test 5: A very deep chain does not overflow the C stack.\n");
  VM* vm = newVM();

  /* Build a chain of pairs nested 1,000,000 deep. This would blow the
     default 8 MB C stack with the baseline's recursive mark(). */
  pushInt(vm, 0);
  for (int i = 1; i < 1000000; i++) {
    pushInt(vm, i);
    pushPair(vm);
  }

  gc(vm);
  check(vm->numObjects == 2 * 1000000 - 1, "Should have kept the chain.");

  freeVM(vm);
}
```

在 `main()` 里 `test4();` 之后插入一行 `test5();`。

- [ ] **Step 3: 跑测试确认失败**

Run: `cc -ggdb -std=gnu99 -Wall gc1_iterative.c -o /tmp/gc1 && /tmp/gc1; echo exit=$?`
Expected: 编译通过；`Test 5` 之前 Test 1–4 输出正常，**随后大概率段错误（递归爆栈）**，`exit` 非 0（139 等）。若恰好没崩（栈余量大），确认 `Test 5` 输出后 `check` 失败也没关系——此步目的是暴露递归问题。

- [ ] **Step 4: 实现 worklist 迭代标记**

删除原 `mark()` 整个函数，在原 `markAll()` 位置替换为：

```c
typedef struct {
  Object** items;
  int count;
  int capacity;
} Worklist;

void workPush(Worklist* work, Object* object) {
  if (work->count == work->capacity) {
    work->capacity = work->capacity == 0 ? 64 : work->capacity * 2;
    work->items = realloc(work->items, work->capacity * sizeof(Object*));
    check(work->items != NULL, "Out of memory growing worklist.");
  }
  work->items[work->count++] = object;
}

Object* workPop(Worklist* work) {
  return work->items[--work->count];
}

void markAll(VM* vm) {
  Worklist work = {NULL, 0, 0};

  /* Seed the worklist with the roots. */
  for (int i = 0; i < vm->stackSize; i++) {
    workPush(&work, vm->stack[i]);
  }

  /* Drain it: pop, mark, push the children. Cycles terminate because
     we skip already-marked objects — same trick as the recursive
     version's early return. */
  while (work.count > 0) {
    Object* object = workPop(&work);
    if (object->marked) continue;

    object->marked = 1;

    if (object->type == OBJ_PAIR) {
      workPush(&work, object->head);
      workPush(&work, object->tail);
    }
  }

  free(work.items);
}
```

注意：`Worklist`/`workPush`/`workPop` 必须放在 `markAll` 之前（本文件无前向声明，自上而下依赖）。

- [ ] **Step 5: 跑测试确认通过**

Run: `make clean && make test`
Expected: `PASS gc0_baseline`、`PASS gc1_iterative`、`2 passed, 0 failed`。`./gc1_iterative` 的 Test 1–4 输出与 gc0 相同，Test 5 正常完成。

- [ ] **Step 6: Commit**

```bash
git add gc1_iterative.c
git commit -m "$(cat <<'EOF'
Add gc1: iterative marking with an explicit worklist

Replace the recursive mark() with a dynamically grown worklist so a
million-deep pair chain no longer overflows the C stack. Behavior is
identical to gc0; new test5 pins the deep-chain case.

Co-Authored-By: Claude Code <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: gc2 — 三色 + 增量标记 + Yuasa 写屏障

**Files:**
- Create: `gc2_incremental.c`
- Modify: `Makefile`（追加 gc2_bo 规则与 test 列表）

**Interfaces:**
- Consumes: `gc1_iterative.c` 的 `check`、worklist 思路（gc2 的灰栈长在 VM 里）
- Produces: `Color`（WHITE/GRAY/BLACK）、`GcState`（GC_IDLE/GC_MARKING）；VM 新字段 `gcState/grayStack/grayCount/grayCapacity`；公开 API `gcStart(vm)`、`gcStep(vm)`、`gcDrive(vm)`、`gc(vm)`（= 若 IDLE 则 start，再 drive 到完成）；受控写 `objectSetHead(vm, pair, value)`、`objectSetTail(vm, pair, value)`；`BARRIER_OFF` 编译版（预期 test6 失败）

- [ ] **Step 1: 复制起步**

```bash
cp gc1_iterative.c gc2_incremental.c
```

替换头部注释块为：

```c
/* gc2 — Tricolor abstraction + incremental marking + Yuasa write barrier.
 *
 * The baseline stops the world and marks the whole heap at once. Real
 * collectors (Go, JVM G1/ZGC/Shenandoah) interleave marking with the
 * mutator. This stage does exactly that, deterministically: after a GC
 * starts, every mutator operation (an allocation or a barriered field
 * write) drives ONE step of marking. Single-threaded interleaving is a
 * precise, replayable simulation of concurrent marking.
 *
 * Tricolor: WHITE = not reached this cycle, GRAY = reached but children
 * unscanned, BLACK = fully scanned. The gray stack is the gc1 worklist
 * living inside the VM.
 *
 * Yuasa snapshot-at-the-beginning barrier. Invariant, one sentence:
 * everything reachable when the GC starts ("the snapshot") survives
 * this cycle; objects that die mid-cycle may survive as floating
 * garbage until the next cycle. Three rules keep it:
 *   1. Overwriting a field shades the OLD value (deletion).
 *   2. Popping a root shades the popped object (root deletion).
 *   3. Objects allocated during marking are born BLACK (they float).
 *
 * Contrast: Dijkstra's insertion barrier shades the NEW value on
 * black->white stores and needs extra root care; Go's hybrid barrier
 * is both combined. Yuasa is chosen here because its proof is one line.
 *
 * Field writes must go through objectSetHead/objectSetTail — raw C
 * writes would bypass the barrier (in a real VM the compiler inserts
 * these calls; we insert them by hand).
 *
 * Sweep stays atomic and is the remaining pause — that is the problem
 * gc3/gc4 exist to attack.
 */
```

- [ ] **Step 2: 写失败测试（改 4 个既有测试 + 新增 test5/test6）**

**test4** 中两处裸写 `a->tail = b;` 和 `b->tail = a;` 改为：

```c
  /* Set up a cycle, and also make 2 and 4 unreachable and collectible.
     Field writes go through the barrier API from now on. */
  objectSetTail(vm, a, b);
  objectSetTail(vm, b, a);
```

在 `test4()` 之后、`test5()` 之前插入新 `test5`（交错正确性）：

```c
void test5() {
  printf("Test 5: Marking interleaves with the mutator, nothing is lost.\n");
  VM* vm = newVM();

  /* Push past the initial threshold (INIT_OBJ_NUM_MAX == 8): the 8th
     allocation starts a cycle instead of running one to completion. */
  for (int i = 0; i < 12; i++) {
    pushInt(vm, i);
  }
  check(vm->gcState == GC_MARKING, "Marking should be in progress.");

  /* Rule 2: popping a root during marking shades the popped object —
     a mutator may stash it in an already-black object. */
  Object* popped = pop(vm);
  check(popped->color == COLOR_GRAY, "Popped root should be shaded.");

  /* Rule 3: objects born during marking are born BLACK. */
  pushInt(vm, 99);
  Object* fresh = vm->stack[vm->stackSize - 1];
  check(fresh->color == COLOR_BLACK, "Fresh object should be born black.");

  gcDrive(vm);
  check(vm->gcState == GC_IDLE, "Marking should have finished.");
  check(vm->numObjects == 13, "No object should be lost: 12 + fresh.");

  freeVM(vm);
}
```

在 `test5()` 之后插入 `test6`（杀手测试）：

```c
void test6() {
  printf("Test 6: The barrier saves an object deleted then re-added.\n");
  VM* vm = newVM();

  /* victim is reachable ONLY through q->tail — not via a root. */
  pushInt(vm, 42);              /* victim */
  pushInt(vm, 2);
  Object* q = pushPair(vm);     /* q = (2, victim) */
  Object* victim = q->tail;

  gcStart(vm);                  /* snapshot taken: victim is live */
  objectSetTail(vm, q, q->head);/* delete the only field reference */
  gcDrive(vm);                  /* marking completes, sweep runs */

  /* Put victim back. With the barrier it survived (shaded on delete),
     so this write is safe and the count includes it. */
  objectSetTail(vm, q, victim);
  check(vm->numObjects == 3, "Snapshot object should have survived.");

  freeVM(vm);
}
```

在 `main()` 的 `test5();` 后追加 `test6();` 一行即可（BARRIER_OFF 构建版会在 test6 的 check 处 exit(1)，`make test` 正是靠非零退出码识别"预期失败"；无需额外打印）。

**test6 关键点（执行者须知）**：victim 的 C 指针在 OFF 构建中会指向已 free 的内存，但测试只把指针值**赋值**进 `q->tail`（写裸指针不 deref，C 语言定义安全），断言只读 `vm->numObjects`——失败路径无未定义行为。

- [ ] **Step 3: 跑测试确认失败**

Run: `cc -ggdb -std=gnu99 -Wall gc2_incremental.c -o /tmp/gc2 && /tmp/gc2; echo exit=$?`
Expected: **编译失败**——`gcState`、`Color`、`gcStart`、`objectSetTail` 等未定义。这就是 C 单文件测试的"红灯"。

- [ ] **Step 4: 实现三色 + 状态机 + 屏障**

**4a.** `ObjectType` 枚举后新增两个枚举；`sObject` 的 `unsigned char marked;` 替换为 `Color color;`：

```c
typedef enum {
  COLOR_WHITE,  /* Not reached this cycle. */
  COLOR_GRAY,   /* Reached, children not yet scanned. */
  COLOR_BLACK   /* Reached and fully scanned. */
} Color;

typedef enum {
  GC_IDLE,      /* No collection in progress. */
  GC_MARKING    /* Incremental marking in progress. */
} GcState;
```

`sObject` 改为：

```c
typedef struct sObject {
  ObjectType type;
  Color color;

  /* The next object in the linked list of heap allocated objects. */
  struct sObject* next;

  union {
    /* OBJ_INT */
    int value;

    /* OBJ_PAIR */
    struct {
      struct sObject* head;
      struct sObject* tail;
    };
  };
} Object;
```

**4b.** `VM` 结构在 `maxObjects` 后追加：

```c
  /* Incremental GC state. */
  GcState gcState;
  Object** grayStack;
  int grayCount;
  int grayCapacity;
```

**4c.** `newVM()` 中 `vm->maxObjects = INIT_OBJ_NUM_MAX;` 后追加：

```c
  vm->gcState = GC_IDLE;
  vm->grayStack = NULL;
  vm->grayCount = 0;
  vm->grayCapacity = 0;
```

**4d.** 原 `mark`/`markAll`/`Worklist`/`workPush`/`workPop` 整体删除，在原位置放灰栈与状态机（`pop` 也要改，见 4e）：

```c
/* Shade: make a white object gray and reachable-by-the-marking-loop. */
void shade(VM* vm, Object* object) {
  if (object == NULL) return;
  if (object->color != COLOR_WHITE) return;

  object->color = COLOR_GRAY;

  if (vm->grayCount == vm->grayCapacity) {
    vm->grayCapacity = vm->grayCapacity == 0 ? 64 : vm->grayCapacity * 2;
    vm->grayStack = realloc(vm->grayStack,
                            vm->grayCapacity * sizeof(Object*));
    check(vm->grayStack != NULL, "Out of memory growing gray stack.");
  }
  vm->grayStack[vm->grayCount++] = object;
}

/* Start an incremental cycle: take the snapshot by graying the roots. */
void gcStart(VM* vm) {
  check(vm->gcState == GC_IDLE, "GC already in progress.");
  vm->gcState = GC_MARKING;
  for (int i = 0; i < vm->stackSize; i++) {
    shade(vm, vm->stack[i]);
  }
}

/* One step: blacken one gray object, gray its children. */
void gcStep(VM* vm) {
  if (vm->grayCount == 0) {
    /* Marking complete: sweep atomically. This sweep — not the marking
       — is the remaining stop-the-world pause of mark-sweep. */
    int numObjects = vm->numObjects;

    Object** object = &vm->firstObject;
    while (*object) {
      if ((*object)->color == COLOR_WHITE) {
        Object* unreached = *object;
        *object = unreached->next;
        free(unreached);
        vm->numObjects--;
      } else {
        /* Survived; reset for the next cycle. */
        (*object)->color = COLOR_WHITE;
        object = &(*object)->next;
      }
    }

    vm->gcState = GC_IDLE;
    vm->maxObjects = vm->numObjects == 0
        ? INIT_OBJ_NUM_MAX : vm->numObjects * 2;

    printf("Collected %d objects, %d remaining.\n",
           numObjects - vm->numObjects, vm->numObjects);
    return;
  }

  Object* object = vm->grayStack[--vm->grayCount];
  object->color = COLOR_BLACK;

  if (object->type == OBJ_PAIR) {
    shade(vm, object->head);
    shade(vm, object->tail);
  }
}

/* Drive the cycle to completion (stop-the-world batching). */
void gcDrive(VM* vm) {
  while (vm->gcState == GC_MARKING) {
    gcStep(vm);
  }
}

/* Explicit collection: start if idle, then finish. */
void gc(VM* vm) {
  if (vm->gcState == GC_IDLE) gcStart(vm);
  gcDrive(vm);
}
```

**4e.** `pop()` 替换为（规则 2）：

```c
Object* pop(VM* vm) {
  check(vm->stackSize > 0, "Stack underflow!");

  Object* popped = vm->stack[--vm->stackSize];

  /* Dropping a root deletes a snapshot path: shade the popped object so
     whatever the mutator does with it stays covered by the snapshot. */
  shade(vm, popped);

  return popped;
}
```

注意：`pop` 在 `shade` 之前定义会编译失败——把 `pop` 挪到 `shade`/`gcStart`/`gcStep`/`gcDrive`/`gc` 之后、`newObject` 之前。

**4f.** `newObject()` 替换为：

```c
Object* newObject(VM* vm, ObjectType type) {
  /* Cross the threshold: start a cycle instead of running one whole. */
  if (vm->gcState == GC_IDLE && vm->numObjects >= vm->maxObjects) {
    gcStart(vm);
  }

  Object* object = malloc(sizeof(Object));
  object->type = type;
  object->next = vm->firstObject;
  vm->firstObject = object;

  /* Rule 3: objects born during marking are born BLACK. They float
     through this cycle as garbage rather than being lost. Their fields
     are filled in by the caller right after — black objects are never
     rescanned, so uninitialized fields are never traced. */
  object->color = vm->gcState == GC_MARKING ? COLOR_BLACK : COLOR_WHITE;

  vm->numObjects++;

  /* This allocation is one mutator step: pay one GC step for it. */
  if (vm->gcState == GC_MARKING) gcStep(vm);

  return object;
}
```

**4g.** `pushPair()` 之后插入受控写 API（规则 1）：

```c
/* Barriered field writes — the only sanctioned way to mutate a pair's
   fields. (A real VM's compiler inserts these; we do it by hand.)
   A field write is a mutator step: like an allocation, it drives one
   step of marking. Order matters: shade the old value BEFORE the
   assignment, step AFTER it. */
void objectSetHead(VM* vm, Object* pair, Object* value) {
  /* Rule 1 (Yuasa deletion): overwriting a field deletes a path from
     the snapshot; shade the OLD value so the snapshot stays intact. */
#ifndef BARRIER_OFF
  shade(vm, pair->head);
#else
  (void)vm;
#endif
  pair->head = value;
  if (vm->gcState == GC_MARKING) gcStep(vm);
}

void objectSetTail(VM* vm, Object* pair, Object* value) {
#ifndef BARRIER_OFF
  shade(vm, pair->tail);
#else
  (void)vm;
#endif
  pair->tail = value;
  if (vm->gcState == GC_MARKING) gcStep(vm);
}
```

`pushPair` 内部对刚建对象的两次赋值保持裸写并加一行注释：

```c
  /* Writes into a just-born (white or black) object are safe without
     the barrier: the popped roots were already shaded by pop(). */
```

**4h.** `freeVM()` 替换为：

```c
void freeVM(VM *vm) {
  vm->stackSize = 0;
  gc(vm);
  free(vm->grayStack);
  free(vm);
}
```

- [ ] **Step 5: 跑测试确认通过**

Run: `cc -ggdb -std=gnu99 -Wall gc2_incremental.c -o /tmp/gc2 && /tmp/gc2; echo exit=$?`
Expected: Test 1–6 全部输出，`exit=0`。

- [ ] **Step 6: Makefile 追加预期失败版**

Makefile 末尾（`clean:` 之前）追加：

```make
gc2_bo: gc2_incremental.c
	$(CC) $(CFLAGS) -DBARRIER_OFF gc2_incremental.c -o $@
```

`test:` 目标 `all` 后改为 `test: all gc2_bo`，并在 BINS 循环后、echo 汇总前插入：

```make
	for b in gc2_bo; do \
	  if ./$$b > /dev/null 2>&1; then echo "UNEXPECTED-PASS $$b"; fail=$$((fail+1)); \
	  else echo "EXPECTED-FAIL $$b"; ok=$$((ok+1)); fi; \
	done; \
```

clean 的 rm 列表追加 `gc2_bo`。

- [ ] **Step 7: 验证与提交**

Run: `make clean && make test`
Expected: `PASS gc0_baseline`、`PASS gc1_iterative`、`PASS gc2_incremental`、`EXPECTED-FAIL gc2_bo`、`4 passed, 0 failed`。

```bash
git add gc2_incremental.c Makefile
git commit -m "$(cat <<'EOF'
Add gc2: tricolor incremental marking with Yuasa write barrier

Marking interleaves with the mutator one step at a time. Three
snapshot rules keep the cycle safe: shade on field overwrite, shade
on root pop, black allocation. Field writes go through
objectSetHead/objectSetTail; BARRIER_OFF builds the expected-fail
binary that test6 catches.

Co-Authored-By: Claude Code <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: gc3 — Cheney 半空间复制 + bump 分配

**Files:**
- Create: `gc3_copying.c`
- Modify: `Makefile`（追加 gc3_overflow）

**Interfaces:**
- Consumes: `gc0_baseline.c`（gc3 从 gc0 起步：mark/sweep 整体删除，不是从 gc1 改）
- Produces: `OBJ_FORWARDED` 枚举值；union 成员 `struct sObject* forward`；VM 堆字段 `heap[2]/space/freeCount/toCount`；`copyObject(vm, object)`；`gc(vm)`（= 交换空间+复制根+Cheney 扫描）；`OVERFLOW_TEST` 编译版（预期失败）

- [ ] **Step 1: 复制起步**

```bash
cp gc0_baseline.c gc3_copying.c
```

然后把 `assert` 全部改名 `check`（`grep -n assert gc3_copying.c`，定义 + 6 处调用；gc0 基线用的是原名，从这里起统一用 `check`）。替换头部注释块为：

```c
/* gc3 — Cheney semispace copying collection + bump-pointer allocation.
 *
 * Mark-sweep leaves the heap fragmented and pays a full-heap sweep.
 * Copying collection flips that: allocation is a pointer bump into a
 * contiguous from-space (O(1), no malloc, no free list — the reason
 * young generations everywhere are bump-allocated), and collection
 * copies the survivors into to-space, squeezing out the garbage and
 * the fragmentation in one motion. Cost: live data is copied, and at
 * most half the heap is usable at any time.
 *
 * Cheney's algorithm: copy the roots into to-space, leaving a
 * forwarding pointer in the dead from-space copy; then walk to-space
 * with a scan pointer, forwarding each field's target on first visit.
 * The scan pointer chasing the free pointer is a breadth-first
 * traversal — no mark bits at all. Cycles work for free: the second
 * visit of any object hits its forwarding pointer.
 *
 * Moving the heap means roots must be rewritten: the VM stack slots
 * are updated in place during collection. A C variable holding an
 * Object* across a GC is stale afterwards — tests re-fetch pointers
 * from the stack. Real runtimes solve this with stack maps or handles.
 */
```

- [ ] **Step 2: 写失败测试（改造 4 个既有 + 新增 3 个）**

**test1** 替换为：

```c
void test1() {
  printf("Test 1: Objects on stack are preserved.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);

  gc(vm);
  check(vm->numObjects == 2, "Should have preserved objects.");
  check(vm->stack[0]->value == 1, "Content should survive the copy.");
  freeVM(vm);
}
```

**test2 / test3** 除 `assert`→`check` 外**逐字不动**（它们不跨 GC 持有 C 指针，无需适配），确认如下：

```c
void test2() {
  printf("Test 2: Unreached objects are collected.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);
  pop(vm);
  pop(vm);

  gc(vm);
  check(vm->numObjects == 0, "Should have collected objects.");
  freeVM(vm);
}

void test3() {
  printf("Test 3: Reach nested objects.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);
  pushPair(vm);
  pushInt(vm, 3);
  pushInt(vm, 4);
  pushPair(vm);
  pushPair(vm);

  gc(vm);
  check(vm->numObjects == 7, "Should have reached objects.");
  freeVM(vm);
}
```

**test4**（环）替换为：

```c
void test4() {
  printf("Test 4: Handle cycles.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);
  Object* a = pushPair(vm);
  pushInt(vm, 3);
  pushInt(vm, 4);
  Object* b = pushPair(vm);

  /* Set up a cycle, and also make 2 and 4 unreachable and collectible. */
  a->tail = b;
  b->tail = a;

  gc(vm);
  check(vm->numObjects == 4, "Should have collected objects.");
  /* Re-fetch after the move: a and b are stale C pointers now. */
  Object* pair = vm->stack[vm->stackSize - 1];
  check(pair->tail->head != NULL, "Cycle should still close.");
  freeVM(vm);
}
```

在 `test4()` 之后插入三个新测试：

```c
void test5() {
  printf("Test 5: Objects move to to-space.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);
  pushPair(vm);
  Object* before = vm->stack[vm->stackSize - 1];

  gc(vm);
  Object* after = vm->stack[vm->stackSize - 1];
  check(before != after, "Objects should have moved.");
  check(after->type == OBJ_PAIR, "Pair should still be a pair.");
  freeVM(vm);
}

void test6() {
  printf("Test 6: A deep chain copies intact.\n");
  VM* vm = newVM();
  const int depth = 10000;

  pushInt(vm, 0);
  for (int i = 1; i < depth; i++) {
    pushInt(vm, i);
    pushPair(vm);
  }

  gc(vm);
  check(vm->numObjects == 2 * depth - 1, "Chain should be complete.");

  /* Walk it: every pair nests one level down, ending at the 0 int. */
  Object* object = vm->stack[vm->stackSize - 1];
  int seen = 0;
  while (object->type == OBJ_PAIR) {
    object = object->head;
    seen++;
  }
  check(seen == depth - 1, "Chain links should be intact.");
  check(object->value == 0, "Chain end should be intact.");
  freeVM(vm);
}

void overflowTest() {
  printf("Overflow Test: heap capacity is enforced.\n");
  VM* vm = newVM();
  /* Exactly HEAP_MAX live objects fit; the next one must check-fail.
     Only compiled into the OVERFLOW_TEST build. */
  for (int i = 0; i < HEAP_MAX + 1; i++) {
    pushInt(vm, i);
  }
  printf("ERROR: overflow was not caught.\n");
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

#ifdef OVERFLOW_TEST
  overflowTest();
#endif

  return 0;
}
```

（原 test1–test3/test4 中 `freeVM(vm)` 前的断言全部保留；test2/test3 的 `gc(vm)` 与 `numObjects` 断言不变——test3 的 7 对象断言原样成立。）

- [ ] **Step 3: 跑测试确认失败**

Run: `cc -ggdb -std=gnu99 -Wall gc3_copying.c -o /tmp/gc3 && /tmp/gc3; echo exit=$?`
Expected: 编译失败——`HEAP_MAX`、`OBJ_FORWARDED`、`before != after` 所需的堆字段等未定义。

- [ ] **Step 4: 实现半空间堆**

**4a.** `#define INIT_OBJ_NUM_MAX 8` 后追加 `#define HEAP_MAX 4096`。

**4b.** `ObjectType` 加一个值：

```c
typedef enum {
  OBJ_INT,
  OBJ_PAIR,
  OBJ_FORWARDED  /* From-space husk: its union holds the new address. */
} ObjectType;
```

**4c.** `sObject`：删掉 `unsigned char marked;` 与 `next` 指针（对象链表没了），union 加 `forward`：

```c
typedef struct sObject {
  ObjectType type;

  union {
    /* OBJ_INT */
    int value;

    /* OBJ_PAIR */
    struct {
      struct sObject* head;
      struct sObject* tail;
    };

    /* OBJ_FORWARDED: where the live copy went. */
    struct sObject* forward;
  };
} Object;
```

**4d.** `VM` 整体替换：

```c
typedef struct {
  Object* stack[STACK_MAX];
  int stackSize;

  /* Two contiguous semispaces. Allocation bumps `freeCount` in the
     from-space heap[space]. */
  Object* heap[2];
  int space;      /* 0 or 1: index of the space we allocate into. */
  int freeCount;  /* Bump cursor: objects allocated in from-space. */
  int toCount;    /* During GC: objects copied into to-space so far. */

  /* The total number of live objects (== freeCount outside a GC). */
  int numObjects;
} VM;
```

**4e.** `newVM()` 替换：

```c
VM* newVM() {
  VM* vm = malloc(sizeof(VM));
  vm->stackSize = 0;
  vm->heap[0] = malloc(HEAP_MAX * sizeof(Object));
  vm->heap[1] = malloc(HEAP_MAX * sizeof(Object));
  vm->space = 0;
  vm->freeCount = 0;
  vm->numObjects = 0;
  return vm;
}
```

**4f.** 删除 `mark`、`markAll`、`sweep`、`gc`，原位置放：

```c
/* Copy an object into to-space (once), and return its new address. */
Object* copyObject(VM* vm, Object* object) {
  if (object == NULL) return NULL;

  /* Already copied? Follow the forwarding address. */
  if (object->type == OBJ_FORWARDED) return object->forward;

  /* First visit: copy to the to-space bump cursor. */
  Object* copy = &vm->heap[1 - vm->space][vm->toCount++];
  *copy = *object;

  /* The from-space copy is dead; overwrite it with the forwarding
     pointer. No side table needed — the corpse IS the table. */
  object->type = OBJ_FORWARDED;
  object->forward = copy;

  return copy;
}

void gc(VM* vm) {
  int numObjects = vm->numObjects;
  vm->toCount = 0;

  /* Cheney: copy the roots, rewriting the stack slots in place. */
  for (int i = 0; i < vm->stackSize; i++) {
    vm->stack[i] = copyObject(vm, vm->stack[i]);
  }

  /* Scan to-space from the bottom: forward each pair's fields. The
     scan pointer chases the free pointer; when they meet, every live
     object has been copied exactly once (cycles included). */
  int scan = 0;
  while (scan < vm->toCount) {
    Object* object = &vm->heap[1 - vm->space][scan++];
    if (object->type == OBJ_PAIR) {
      object->head = copyObject(vm, object->head);
      object->tail = copyObject(vm, object->tail);
    }
  }

  vm->space = 1 - vm->space;
  vm->freeCount = vm->toCount;
  vm->numObjects = vm->toCount;

  printf("Collected %d objects, %d remaining.\n",
         numObjects - vm->numObjects, vm->numObjects);
}
```

**4g.** `newObject()` 替换：

```c
Object* newObject(VM* vm, ObjectType type) {
  /* From-space full: collect, then insist on room. */
  if (vm->freeCount == HEAP_MAX) gc(vm);
  check(vm->freeCount < HEAP_MAX, "Heap overflow!");

  Object* object = &vm->heap[vm->space][vm->freeCount++];
  object->type = type;

  vm->numObjects++;

  return object;
}
```

**4h.** `freeVM()` 替换：

```c
void freeVM(VM *vm) {
  vm->stackSize = 0;
  gc(vm);
  free(vm->heap[0]);
  free(vm->heap[1]);
  free(vm);
}
```

（`pushInt`/`pushPair`/`objectPrint`/`perfTest` 原样保留，无需改动。）

- [ ] **Step 5: 跑测试确认通过**

Run: `cc -ggdb -std=gnu99 -Wall gc3_copying.c -o /tmp/gc3 && /tmp/gc3; echo exit=$?`
Expected: Test 1–6 + Performance Test 全部输出，`exit=0`。

- [ ] **Step 6: Makefile 追加预期失败版**

Task 3 的 gc2_bo 规则后追加：

```make
gc3_overflow: gc3_copying.c
	$(CC) $(CFLAGS) -DOVERFLOW_TEST gc3_copying.c -o $@
```

`test:` 依赖追加 `gc3_overflow`；预期失败循环改为 `for b in gc2_bo gc3_overflow; do`；clean 追加 `gc3_overflow`。

- [ ] **Step 7: 验证与提交**

Run: `make clean && make test`
Expected: 三个 PASS + `EXPECTED-FAIL gc2_bo` + `EXPECTED-FAIL gc3_overflow` + `5 passed, 0 failed`。

```bash
git add gc3_copying.c Makefile
git commit -m "$(cat <<'EOF'
Add gc3: Cheney semispace copying with bump-pointer allocation

Replace per-object malloc and the mark/sweep pair with two fixed
arenas and Cheney copy: roots are copied and rewritten in place, a
scan pointer forwards fields breadth-first, forwarding addresses
live in the dead from-space husks. No mark bits; cycles come free.
OVERFLOW_TEST builds the expected-fail capacity check.

Co-Authored-By: Claude Code <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: gc4 — 分代收集

**Files:**
- Create: `gc4_generational.c`
- Modify: `Makefile`（追加 gc4_bo）

**Interfaces:**
- Consumes: `gc0_baseline.c`（old gen 形态）、gc3 的转发技巧（nursery 晋升）
- Produces: `isYoung(vm, object)`（NULL 安全）；`objectSetHead/objectSetTail`（代际屏障版）；`minorGC(vm)`、`majorGC(vm)`；VM 统计字段 `minorGcCount/majorGcCount/promotedCount`、`oldCount/oldMax`、`remset/remsetCount/remsetCapacity`；`REMSET_OFF` 编译版（预期失败）

- [ ] **Step 1: 复制起步**

```bash
cp gc0_baseline.c gc4_generational.c
```

然后把 `assert` 全部改名 `check`（`grep -n assert gc4_generational.c`，定义 + 6 处调用）。替换头部注释块为：

```c
/* gc4 — Generational collection.
 *
 * The weak generational hypothesis: most objects die young. So give
 * the young their own small space and collect it constantly — cheaply
 * — and only touch the old generation rarely.
 *
 * gc4 is literally gc3's nursery + gc0's old gen, glued by a write
 * barrier:
 *   - nursery: one contiguous arena, bump-allocated (gc3, halved).
 *   - old gen: malloc'd objects on an intrusive linked list, collected
 *     by iterative mark-sweep (gc0/gc1, verbatim).
 *   - The glue: a remembered set. An old object gaining a reference to
 *     a young one is "remembered"; minor GCs re-scan remembered
 *     objects as extra roots.
 *
 * Minor GC promotes every surviving young object to the old gen on
 * first survival (real JVMs use age thresholds and survivor spaces)
 * and then just resets the nursery cursor — dead young objects cost
 * nothing at all. That free ride is the whole point of the design.
 * Promotion uses gc3's forwarding trick: the nursery husk records
 * where the object went, and a slot worklist rewrites every reference
 * (stack slots, old fields, promoted copies' fields).
 *
 * Major GC: promote everything live, then plain mark-sweep the old
 * list. Triggered by the old gen crossing oldMax (the gc0 x2
 * heuristic, reused).
 *
 * Remset entries store whole objects (re-scanned both fields) rather
 * than slots — simpler, and the comment at minorGC notes what real
 * collectors pay for that choice.
 */
```

- [ ] **Step 2: 写失败测试（改造既有 + 新增 3 个）**

**test1** 替换为：

```c
void test1() {
  printf("Test 1: Objects on stack are preserved.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);

  minorGC(vm);
  check(vm->oldCount == 2, "Should have promoted both objects.");
  check(vm->nurseryCount == 0, "Nursery should be empty.");
  check(vm->minorGcCount == 1, "Should have run one minor GC.");
  freeVM(vm);
}
```

**test2 / test3** 替换为：

```c
void test2() {
  printf("Test 2: Unreached objects are collected.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);
  pop(vm);
  pop(vm);

  minorGC(vm);
  check(vm->oldCount == 0, "Should have collected objects.");
  freeVM(vm);
}

void test3() {
  printf("Test 3: Reach nested objects.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);
  pushPair(vm);
  pushInt(vm, 3);
  pushInt(vm, 4);
  pushPair(vm);
  pushPair(vm);

  minorGC(vm);
  check(vm->oldCount == 7, "Should have promoted the whole tree.");
  /* The nested structure must survive promotion intact: promoted
     copies' fields must point at promoted copies (review-focus #4). */
  Object* outer = vm->stack[vm->stackSize - 1];
  check(outer->tail->type == OBJ_PAIR, "Inner pair should survive.");
  check(outer->tail->tail->value == 4, "Leaf values should survive.");
  freeVM(vm);
}
```

**test4**（环）替换为：

```c
void test4() {
  printf("Test 4: Handle cycles.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);
  Object* a = pushPair(vm);
  pushInt(vm, 3);
  pushInt(vm, 4);
  Object* b = pushPair(vm);

  /* Set up a cycle, and also make 2 and 4 unreachable and collectible.
     Young-to-young writes need no barrier. */
  a->tail = b;
  b->tail = a;

  minorGC(vm);
  check(vm->oldCount == 4, "Cycle should survive promotion intact.");

  /* Re-fetch: a and b were young pointers, now promoted. */
  Object* pair = vm->stack[vm->stackSize - 1];
  check(pair->tail->head != NULL, "Cycle should still close.");
  freeVM(vm);
}
```

在 `test4()` 之后插入三个新测试：

```c
void test5() {
  printf("Test 5: The remembered set saves an old->young reference.\n");
  VM* vm = newVM();

  pushInt(vm, 2);
  pushInt(vm, 3);
  pushPair(vm);               /* young pair on the stack */
  minorGC(vm);                /* promoted; nursery empty again */

  /* The pre-GC pair pointer is a stale nursery husk — re-fetch the
     live, now-old pair from the stack before writing to it. */
  Object* p = vm->stack[vm->stackSize - 1];
  check(vm->oldCount == 3, "Pair should be old now.");

  /* X is young and reachable ONLY through old p's tail. */
  pushInt(vm, 42);
  Object* x = vm->stack[vm->stackSize - 1];
  objectSetTail(vm, p, x);    /* barrier fires: old p gets remembered */
  pop(vm);                    /* drop the root; p->tail is all X has */

  minorGC(vm);

  /* Reuse the freed nursery slot so a lost X is observable by value.
     The arena is never freed mid-run, so even the REMSET_OFF build
     reads valid memory — the failure is deterministic, no UB. */
  pushInt(vm, 7);

  check(p->tail->value == 42, "Young object should survive via remset.");
  check(vm->oldCount == 4, "X should have been promoted.");
  freeVM(vm);
}

void test6() {
  printf("Test 6: Minor/major split and old-gen independence.\n");
  VM* vm = newVM();

  /* 200 live ints (stay under STACK_MAX == 256): one minor promotes
     them all, and oldCount (200) crosses oldMax (8), firing a major. */
  for (int i = 0; i < 200; i++) {
    pushInt(vm, i);
  }
  minorGC(vm);

  check(vm->minorGcCount == 1, "Explicit minor should have run.");
  check(vm->majorGcCount == 1, "Old gen growth should run a major.");
  check(vm->oldCount == 200, "Everything is live and promoted.");

  /* Old-gen garbage is invisible to minor GCs — only a major sweeps
     the old list. Drop every root and run another minor: the count of
     now-dead old objects must not move. */
  for (int i = 0; i < 200; i++) {
    pop(vm);
  }
  minorGC(vm);
  check(vm->oldCount == 200, "Minor GC must not collect the old gen.");
  freeVM(vm);
}

void test7() {
  printf("Test 7: NULL field values are safe.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);
  pushPair(vm);
  minorGC(vm);

  /* Re-fetch the promoted pair (the young pointer is a stale husk). */
  Object* p = vm->stack[vm->stackSize - 1];

  /* Storing NULL must not be range-checked into "is young". */
  objectSetTail(vm, p, NULL);
  minorGC(vm);
  check(p->tail == NULL, "NULL should stay NULL.");
  freeVM(vm);
}
```

`main()` 的测试列表在 `test4();` 后追加 `test5(); test6(); test7();`。`perfTest()` 末尾 `freeVM(vm);` 前追加：

```c
  printf("%d minor GCs, %d major GCs, %d promotions.\n",
         vm->minorGcCount, vm->majorGcCount, vm->promotedCount);
```

- [ ] **Step 3: 跑测试确认失败**

Run: `cc -ggdb -std=gnu99 -Wall gc4_generational.c -o /tmp/gc4 && /tmp/gc4; echo exit=$?`
Expected: 编译失败——`minorGC`、`objectSetTail`、`oldCount` 等未定义。

- [ ] **Step 4: 实现分代**

**4a.** `#define INIT_OBJ_NUM_MAX 8` 替换为：

```c
#define NURSERY_MAX 256
#define INIT_OLD_OBJ_NUM_MAX 8
```

**4b.** `sObject` 替换（`marked` 给老年代标记用，`remembered` 给 remset 用）：

```c
typedef struct sObject {
  ObjectType type;
  unsigned char marked;      /* Old-gen mark bit (gc0 verbatim). */
  unsigned char remembered;  /* Already in the remembered set. */

  /* Old-gen intrusive list. */
  struct sObject* next;

  union {
    /* OBJ_INT */
    int value;

    /* OBJ_PAIR */
    struct {
      struct sObject* head;
      struct sObject* tail;
    };

    /* OBJ_FORWARDED: nursery husk pointing at the promoted copy. */
    struct sObject* forward;
  };
} Object;
```

`ObjectType` 加 `OBJ_FORWARDED`（同 Task 4）。

**4c.** `VM` 整体替换：

```c
typedef struct {
  Object* stack[STACK_MAX];
  int stackSize;

  /* Young generation: one contiguous, bump-allocated arena. */
  Object* nursery;
  int nurseryCount;

  /* Old generation: malloc'd objects on a linked list. */
  Object* oldFirst;
  int oldCount;
  int oldMax;

  /* Remembered set: old objects with young references. */
  Object** remset;
  int remsetCount;
  int remsetCapacity;

  /* Stats, printed by perfTest and asserted by tests. */
  int minorGcCount;
  int majorGcCount;
  int promotedCount;
} VM;
```

**4d.** `newVM()` 替换：

```c
VM* newVM() {
  VM* vm = malloc(sizeof(VM));
  vm->stackSize = 0;
  vm->nursery = malloc(NURSERY_MAX * sizeof(Object));
  vm->nurseryCount = 0;
  vm->oldFirst = NULL;
  vm->oldCount = 0;
  vm->oldMax = INIT_OLD_OBJ_NUM_MAX;
  vm->remset = NULL;
  vm->remsetCount = 0;
  vm->remsetCapacity = 0;
  vm->minorGcCount = 0;
  vm->majorGcCount = 0;
  vm->promotedCount = 0;
  return vm;
}
```

**4e.** 删除 `mark`、`markAll`、`sweep`、`gc`，原位置放（注意顺序：isYoung → remsetAdd → slotPush/slotPop → promoteAll → majorGC → minorGC）：

```c
/* Which generation does this object live in? NULL is neither — the
   explicit check keeps NULL out of the pointer range compare. */
int isYoung(VM* vm, Object* object) {
  return object != NULL
      && object >= vm->nursery
      && object < vm->nursery + NURSERY_MAX;
}

void remsetAdd(VM* vm, Object* object) {
  if (vm->remsetCount == vm->remsetCapacity) {
    vm->remsetCapacity = vm->remsetCapacity == 0 ? 16 : vm->remsetCapacity * 2;
    vm->remset = realloc(vm->remset, vm->remsetCapacity * sizeof(Object*));
    check(vm->remset != NULL, "Out of memory growing remset.");
  }
  vm->remset[vm->remsetCount++] = object;
}

/* Slot worklist: references to update, not objects to visit. */
typedef struct {
  Object*** items;
  int count;
  int capacity;
} Slotlist;

void slotPush(Slotlist* slots, Object** slot) {
  if (slots->count == slots->capacity) {
    slots->capacity = slots->capacity == 0 ? 64 : slots->capacity * 2;
    slots->items = realloc(slots->items, slots->capacity * sizeof(Object**));
    check(slots->items != NULL, "Out of memory growing slot list.");
  }
  slots->items[slots->count++] = slot;
}

/* Promote every young object reachable from the roots and the
   remembered set, rewriting every reference along the way, then reset
   the nursery cursor. Young garbage simply vanishes — copying a young
   generation is free precisely because nothing is swept. */
void promoteAll(VM* vm) {
  Slotlist slots = {NULL, 0, 0};

  for (int i = 0; i < vm->stackSize; i++) {
    slotPush(&slots, &vm->stack[i]);
  }

  /* Real collectors record SLOTS in the remset to re-scan exactly the
     fields that can hold young pointers; we store whole objects and
     re-scan both fields — simpler, but pays a little per minor GC. */
  for (int i = 0; i < vm->remsetCount; i++) {
    Object* old = vm->remset[i];
    old->remembered = 0;
    if (old->type == OBJ_PAIR) {
      slotPush(&slots, &old->head);
      slotPush(&slots, &old->tail);
    }
  }

  while (slots.count > 0) {
    Object** slot = slots.items[--slots.count];
    Object* object = *slot;

    /* Old objects never move, and any young pointer they hold is
       covered by the remset above — no need to descend through them. */
    if (!isYoung(vm, object)) continue;

    /* Second visit through another slot: follow the husk. */
    if (object->type == OBJ_FORWARDED) {
      *slot = object->forward;
      continue;
    }

    /* First visit: promote. */
    Object* copy = malloc(sizeof(Object));
    *copy = *object;
    copy->remembered = 0;
    copy->next = vm->oldFirst;
    vm->oldFirst = copy;
    vm->oldCount++;
    vm->promotedCount++;

    object->type = OBJ_FORWARDED;
    object->forward = copy;
    *slot = copy;

    if (copy->type == OBJ_PAIR) {
      slotPush(&slots, &copy->head);
      slotPush(&slots, &copy->tail);
    }
  }

  free(slots.items);

  vm->nurseryCount = 0;
  vm->remsetCount = 0;
}

/* Old-gen collection: iterative mark-sweep over the old list (the gc1
   worklist over the gc0 heap). Runs only after promotion drained the
   nursery, so every live object is old and every young pointer in a
   live object is dead by construction. */
void majorGC(VM* vm) {
  vm->majorGcCount++;

  Worklist work = {NULL, 0, 0};
  for (int i = 0; i < vm->stackSize; i++) {
    workPush(&work, vm->stack[i]);
  }
  while (work.count > 0) {
    Object* object = workPop(&work);
    /* NULL guards both empty fields and defensive traversal. */
    if (object == NULL || object->marked) continue;

    object->marked = 1;

    if (object->type == OBJ_PAIR) {
      workPush(&work, object->head);
      workPush(&work, object->tail);
    }
  }
  free(work.items);

  Object** object = &vm->oldFirst;
  while (*object) {
    if (!(*object)->marked) {
      Object* unreached = *object;
      *object = unreached->next;
      free(unreached);
      vm->oldCount--;
    } else {
      (*object)->marked = 0;
      object = &(*object)->next;
    }
  }

  vm->oldMax = vm->oldCount == 0
      ? INIT_OLD_OBJ_NUM_MAX : vm->oldCount * 2;

  printf("Major GC: %d old objects remaining.\n", vm->oldCount);
}

void minorGC(VM* vm) {
  vm->minorGcCount++;
  promoteAll(vm);
  printf("Minor GC: old gen at %d objects.\n", vm->oldCount);

  if (vm->oldCount >= vm->oldMax) majorGC(vm);
}
```

注意：`Worklist`/`workPush`/`workPop` 必须放在 `majorGC` 之前定义（本文件无前向声明）。完整代码（gc1 的 worklist，`marked` 字段版）：

```c
typedef struct {
  Object** items;
  int count;
  int capacity;
} Worklist;

void workPush(Worklist* work, Object* object) {
  if (work->count == work->capacity) {
    work->capacity = work->capacity == 0 ? 64 : work->capacity * 2;
    work->items = realloc(work->items, work->capacity * sizeof(Object*));
    check(work->items != NULL, "Out of memory growing worklist.");
  }
  work->items[work->count++] = object;
}

Object* workPop(Worklist* work) {
  return work->items[--work->count];
}
```

**4f.** `newObject()` 替换：

```c
Object* newObject(VM* vm, ObjectType type) {
  /* Nursery full: collect it. After promotion the cursor is 0, so
     there is always room afterwards. */
  if (vm->nurseryCount == NURSERY_MAX) minorGC(vm);

  Object* object = &vm->nursery[vm->nurseryCount++];
  object->type = type;
  object->marked = 0;
  object->remembered = 0;

  return object;
}
```

**4g.** `pushPair()` 之后插入代际屏障：

```c
/* Barriered field writes — remember old objects that gain young refs. */
void objectSetHead(VM* vm, Object* pair, Object* value) {
#ifndef REMSET_OFF
  if (!isYoung(vm, pair) && isYoung(vm, value) && !pair->remembered) {
    pair->remembered = 1;
    remsetAdd(vm, pair);
  }
#else
  (void)vm;
#endif
  pair->head = value;
}

void objectSetTail(VM* vm, Object* pair, Object* value) {
#ifndef REMSET_OFF
  if (!isYoung(vm, pair) && isYoung(vm, value) && !pair->remembered) {
    pair->remembered = 1;
    remsetAdd(vm, pair);
  }
#else
  (void)vm;
#endif
  pair->tail = value;
}
```

**4h.** `freeVM()` 替换：

```c
void freeVM(VM *vm) {
  vm->stackSize = 0;
  promoteAll(vm);  /* no-op count-wise; keeps invariants simple */

  Object* object = vm->oldFirst;
  while (object) {
    Object* next = object->next;
    free(object);
    object = next;
  }

  free(vm->nursery);
  free(vm->remset);
  free(vm);
}
```

- [ ] **Step 5: 跑测试确认通过**

Run: `cc -ggdb -std=gnu99 -Wall gc4_generational.c -o /tmp/gc4 && /tmp/gc4; echo exit=$?`
Expected: Test 1–7 + Performance Test（带统计行）全部输出，`exit=0`。性能测试应显示 minor 次数多、major 次数少或为零——分代假设的直接证据。

- [ ] **Step 6: Makefile 追加预期失败版**

```make
gc4_bo: gc4_generational.c
	$(CC) $(CFLAGS) -DREMSET_OFF gc4_generational.c -o $@
```

`test:` 依赖追加 `gc4_bo`；预期失败循环改为 `for b in gc2_bo gc3_overflow gc4_bo; do`；clean 追加 `gc4_bo`。

- [ ] **Step 7: 验证与提交**

Run: `make clean && make test`
Expected: `PASS`×5、`EXPECTED-FAIL`×3、`8 passed, 0 failed`。

```bash
git add gc4_generational.c Makefile
git commit -m "$(cat <<'EOF'
Add gc4: generational collection with remembered set

Nursery (bump arena, gc3-style forwarding on promotion) + old gen
(malloc list, gc0-style iterative mark-sweep), glued by a write
barrier feeding a remembered set. Minor GCs promote survivors and
reset the cursor — young garbage costs nothing. REMSET_OFF builds
the expected-fail binary that test5 catches.

Co-Authored-By: Claude Code <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: README 路线图 + 收尾验证

**Files:**
- Modify: `README.md`

**Interfaces:**
- Consumes: 全部五个阶段文件
- Produces: 最终 README

- [ ] **Step 1: 重写 README.md**

整体替换为：

```markdown
# mark-sweep — a GC teaching playground

Sample code for [Baby's First Garbage Collector][article], grown into a
five-stage tour of how real collectors work. Each `gcN.c` is one
self-contained, runnable collector; read them in order and diff each
against its predecessor — the diff *is* the lesson.

[article]: http://journal.stuffwithstuff.com/2013/12/08/babys-first-garbage-collector/

| Stage | Concept introduced | Where it shows up in production |
|---|---|---|
| `main.c` / `gc0_baseline.c` | mark-sweep, root set, adaptive threshold | the classic baseline |
| `gc1_iterative.c` | explicit worklist instead of recursion | every production collector |
| `gc2_incremental.c` | tricolor abstraction, incremental marking, Yuasa write barrier, mutator interleaving | Go's GC, JVM concurrent markers |
| `gc3_copying.c` | Cheney semispace copy, bump-pointer allocation, forwarding pointers, root rewriting | JVM young gen, every moving GC |
| `gc4_generational.c` | weak generational hypothesis, promotion, remembered set, minor/major split | JVM G1, Go 1.19+, Mimalloc |

Build and test everything (three expected-fail binaries prove the
barriers actually matter — compile them out and the killer tests
catch the loss):

    make test

Suggested reading order: each file's header comment, then the diff
against the previous stage, then the tests at the bottom — the killer
tests (`test6` in gc2, `test5` in gc4) are the ones whose failure the
barriers exist to prevent.
```

- [ ] **Step 2: 最终全量验证**

Run: `make clean && make test && make clean && make markandsweep && ./markandsweep | head -5`
Expected: `8 passed, 0 failed`；原版二进制照常运行（输出 Test 1 头几行）。

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "$(cat <<'EOF'
Rewrite README as the five-stage GC tour map

Co-Authored-By: Claude Code <noreply@anthropic.com>
EOF
)"
```

---

## 执行说明（对 worker）

- 每个任务独立可交付、独立可评审；任务内严格按步骤顺序（C 单文件的"红灯"= 编译失败或段错误，"绿灯" = `exit=0` + `make test` 全绿）
- 不要动 `main.c`；不要动 master 分支
- 若某步的实际输出与 Expected 不符，停下来按 systematic-debugging 排查，不要硬闯
