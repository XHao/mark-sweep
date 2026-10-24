# gc5 标记-压缩实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 追加第六阶段 `gc5_compact.c`——Lisp2 三趟滑动标记-压缩，与 gc3 同总内存下可用堆 100% vs 50%。

**Architecture:** 从 `gc3_copying.c` 派生：保留 arena+bump 分配器（改单空间 8192），删除 to-space/转发机制，加回 gc1 式迭代标记与三趟压缩（算新址 → 改指针 → 滑动）。Makefile wildcard 自动纳入，无 flag 构建。

**Tech Stack:** C（`-ggdb -std=gnu99 -Wall`）、POSIX make、macOS clang。零依赖。

**Spec:** `docs/2026-10-25-gc5-mark-compact-design.md`

## Global Constraints

- C99 零依赖；`$(CC) -ggdb -std=gnu99 -Wall` 无新警告
- 沿用既有代码风格（2 空格缩进、注释密度、printf 风格、`check` 命名）
- gc0–gc4 五个文件与 main.c 逐字节不动；Makefile 无需改动（wildcard `gc*.c`）
- `HEAP_MAX 8192`（spec §3，勿改 4096）；`STACK_MAX 256` 不变
- 每任务结束 `make test` 全绿（gc5 计入 BINS，无预期失败构建）
- 提交信息英文祈使句 + `Co-Authored-By: Claude Code <noreply@anthropic.com>`；分支 `gc5-mark-compact`，不动 master
- 测试编号决定（对 spec §5 的细化）：gc3 派生文件已有 test5（移动概念），故 gc5 编号为 t5=滑动下移、t6=容量、t7=碎片保序、t8=垃圾上深链——与 spec 内容对应；t8 深度由 spec 的 4000 调为 3500（叠加 999 尸体后 7998 ≤ 8192）

## Review Focus

（spec 未显式覆盖、最易坑人的五类；每条已锚定到所属任务测试）

1. **Pass 3 前向覆盖**（dest > src 的回归会静默毁数据）→ Task 1 `test5` 下滑断言（`after < before`）+ `test7`/`test8` 压缩后逐级内容遍历
2. **forward 残留跨轮**（下轮忘记重算新址）→ Task 1 `test7`：gc 后继续分配 20 个对象，`freeVM` 触发第二次 gc；两轮都断言存活数
3. **numObjects/freeCount 失步** → Task 1 `test6`/`test8` 同时断言两计数器
4. **marked 未清**（跨轮泄漏导致全活/全死）→ Task 1 `test7` 第二次 gc（freeVM）+ perfTest 两次 gc 输出非零回收数
5. **移动后 C 指针失效**（测试自身正确性）→ Task 1 `test1`/`test4`/`test5` 的"GC 后从栈重取"模式

---

### Task 1: gc5_compact.c — Lisp2 标记-压缩

**Files:**
- Create: `gc5_compact.c`

**Interfaces:**
- Consumes: `gc3_copying.c`（派生源）、gc1 的 Worklist 形态
- Produces: 自包含收集器；对外仅 `main()`（8 测试 + perfTest）；无新 Makefile 目标

- [ ] **Step 1: 复制起步**

```bash
cp gc3_copying.c gc5_compact.c
```

替换头部注释块（原 gc3 块整体）为：

```c
/* gc5 — Lisp2 mark-compact: three passes, sliding, in place.
 *
 * gc3 answered mark-sweep's fragmentation by copying — at the cost of
 * using only half the heap at any time. Compaction answers it by
 * sliding the survivors together in the SAME space: 100% of the heap
 * stays usable (8192 slots here = gc3's two 4096 semispaces), at the
 * price of more passes over the heap per collection.
 *
 * The classic Lisp2 algorithm, after marking:
 *   Pass 1 — walk the arena in address order; assign each survivor a
 *            destination slot (kept in a dedicated `forward` field),
 *            counting upward. Sliding: survivors keep their order.
 *   Pass 2 — rewrite every reference (stack roots, then every live
 *            pair's fields) to point at destinations. Objects have not
 *            moved yet, so old fields stay readable while we walk.
 *   Pass 3 — walk the arena again; move each survivor down to its
 *            destination. dest <= src always, so ascending order never
 *            overwrites an unread source.
 *
 * The `forward` field is dedicated, NOT a union member like gc3's
 * forwarding trick: pass 2 must read head/tail while the destination is
 * already stored. Real JVMs bit-pack forwarding pointers into the mark
 * word or use a Brooks forwarding pointer; we pay 8 bytes for clarity.
 *
 * Cost note: passes 1 and 3 scan the whole occupied region, not just
 * the live set — compaction costs O(heap used), which is exactly what
 * regional and generational heaps (gc4, and the collectors it imitates)
 * exist to amortize.
 */
```

- [ ] **Step 2: 写失败测试（重写测试区）**

原 test5/test6/overflowTest 整体删除，test1–test4 保留（gc3 版已适配移动 GC：t1 内容校验、t4 环+重取，无需改动）。在 test4 之后插入四个新测试，并在其后保留原 `perfTest()` 不动：

```c
void test5() {
  printf("Test 5: Survivors slide down over garbage.\n");
  VM* vm = newVM();
  pushInt(vm, 1);            /* stays at the bottom */
  pushInt(vm, 2);            /* will die in place */
  pop(vm);
  pushInt(vm, 3);            /* allocated above the corpse of 2 */
  Object* before = vm->stack[1];

  gc(vm);
  check(vm->numObjects == 2, "The corpse should be compacted away.");
  Object* after = vm->stack[1];
  check(before != after, "The survivor should have moved.");
  check(after < before, "Sliding compaction moves objects DOWN.");
  check(after->value == 3, "Content survives the slide.");
  freeVM(vm);
}

void test6() {
  printf("Test 6: The whole heap is usable — more than gc3 could hold.\n");
  VM* vm = newVM();

  /* 5999 live objects: past gc3's 4096-slot semispace ceiling, well
     under this heap's 8192. The chain build keeps the stack at ~2. */
  const int depth = 3000;
  pushInt(vm, 0);
  for (int i = 1; i < depth; i++) {
    pushInt(vm, i);
    pushPair(vm);
  }

  gc(vm);
  check(vm->numObjects == 2 * depth - 1, "The whole chain should fit.");

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

void test7() {
  printf("Test 7: Holes are merged and survivors keep their order.\n");
  VM* vm = newVM();

  /* Eight chains, depth 3 (5 objects each), one root each. */
  const int chains = 8;
  const int depth = 3;
  for (int j = 0; j < chains; j++) {
    pushInt(vm, j);
    for (int i = 1; i < depth; i++) {
      pushInt(vm, j);
      pushPair(vm);
    }
  }
  check(vm->stackSize == chains, "One root per chain.");

  /* Pop everything, push back only the even-numbered chains. popping
     yields the top first, so store into roots[chains-1-j] to restore
     ascending order. Chains 1, 3, 5, 7 die interleaved in the heap. */
  Object* roots[chains];
  for (int j = 0; j < chains; j++) {
    roots[chains - 1 - j] = pop(vm);
  }
  for (int j = 0; j < chains; j += 2) {
    push(vm, roots[j]);
  }

  gc(vm);
  check(vm->numObjects == (chains / 2) * (2 * depth - 1),
        "Every other chain should be compacted away.");

  /* Order fingerprint: sliding compaction preserves allocation order,
     so the surviving roots' addresses must ascend with the stack. */
  Object* prev = NULL;
  for (int j = 0; j < vm->stackSize; j++) {
    Object* root = vm->stack[j];
    if (prev) check(prev < root, "Survivors should keep their order.");
    prev = root;
  }
  check(vm->stack[0]->head->value == 0, "Chain content survives.");

  /* The merged holes are reusable: allocate another 20 objects. */
  for (int i = 0; i < 4 * (2 * depth - 1); i++) {
    pushInt(vm, i);
  }
  check(vm->numObjects == 40, "Holes should have been reused.");
  freeVM(vm);
}

void test8() {
  printf("Test 8: A deep chain compacts intact over garbage.\n");
  VM* vm = newVM();

  /* 999 corpses at the bottom, then a 6999-object chain above them
     (3500-deep chain: 6999 + 999 = 7998 <= 8192; the spec's 4000-depth
     figure assumed no garbage — shrunk here to fit under it). */
  for (int i = 0; i < 999; i++) {
    pushInt(vm, -1);
    pop(vm);
  }
  const int depth = 3500;
  pushInt(vm, 0);
  for (int i = 1; i < depth; i++) {
    pushInt(vm, i);
    pushPair(vm);
  }

  gc(vm);
  check(vm->numObjects == 2 * depth - 1, "Chain kept, garbage gone.");
  check(vm->freeCount == 2 * depth - 1, "Heap is tight after compaction.");

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
  test7();
  test8();
  perfTest();

  return 0;
}
```

- [ ] **Step 3: 跑测试确认失败**

Run: `cc -ggdb -std=gnu99 -Wall gc5_compact.c -o /tmp/gc5 && /tmp/gc5 > /tmp/gc5.out 2>&1; echo exit=$?; tail -2 /tmp/gc5.out`
Expected: **编译通过**（测试只用 gc3 已有的标识符），运行时在 Test 6 处死于 `Heap overflow!`，`exit=1`。原因：实现仍是 gc3 残余（HEAP_MAX=4096 双空间），5999 个存活对象超出其单空间容量——这正是 gc5 要解决的 50% 浪费，容量测试在此刻失败即本步的 RED。（Test 1–5 在 gc3 残余上恰好也能过：滑动语义与双空间复制在无垃圾紧排布下地址行为一致。）

- [ ] **Step 4: 实现单空间 + 三趟压缩**

**4a.** `#define HEAP_MAX 4096` 替换为：

```c
#define HEAP_MAX 8192  /* Same total memory as gc3's two 4096 semispaces;
                          with compaction, 100% of it is usable. */
```

**4b.** `ObjectType` 枚举删去 `OBJ_FORWARDED`（恢复两值）：

```c
typedef enum {
  OBJ_INT,
  OBJ_PAIR
} ObjectType;
```

**4c.** `sObject` 整体替换（加回标记位与专设 forward）：

```c
typedef struct sObject {
  ObjectType type;
  unsigned char marked;

  /* Where this object moves during compaction (pass 1 writes it).
     Dedicated field, not a union member: pass 2 rewrites pointers
     while the old head/tail must stay readable. Real JVMs bit-pack a
     forwarding pointer into the mark word or use a Brooks pointer;
     we pay 8 bytes for clarity. */
  struct sObject* forward;

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

**4d.** `VM` 整体替换：

```c
typedef struct {
  Object* stack[STACK_MAX];
  int stackSize;

  /* One contiguous arena. Allocation bumps `freeCount`. */
  Object* heap;
  int freeCount;

  /* The total number of live objects (== freeCount). */
  int numObjects;
} VM;
```

**4e.** `newVM()` 替换：

```c
VM* newVM() {
  VM* vm = malloc(sizeof(VM));
  vm->stackSize = 0;
  vm->heap = malloc(HEAP_MAX * sizeof(Object));
  vm->freeCount = 0;
  vm->numObjects = 0;
  return vm;
}
```

**4f.** 删除 `copyObject` 与 gc3 版 `gc()`，原位置放：

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

  for (int i = 0; i < vm->stackSize; i++) {
    workPush(&work, vm->stack[i]);
  }

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

void gc(VM* vm) {
  int numObjects = vm->numObjects;

  markAll(vm);

  /* Pass 1: assign destinations in address order (sliding). */
  int newCount = 0;
  for (int i = 0; i < vm->freeCount; i++) {
    Object* object = &vm->heap[i];
    object->forward = object->marked ? &vm->heap[newCount++] : NULL;
  }

  /* Pass 2: rewrite the references. Roots first, then every live
     pair's fields — objects have not moved yet, so the old fields are
     readable while we rewrite them. */
  for (int i = 0; i < vm->stackSize; i++) {
    vm->stack[i] = vm->stack[i]->forward;
  }
  for (int i = 0; i < vm->freeCount; i++) {
    Object* object = &vm->heap[i];
    if (object->marked && object->type == OBJ_PAIR) {
      object->head = object->head ? object->head->forward : NULL;
      object->tail = object->tail ? object->tail->forward : NULL;
    }
  }

  /* Pass 3: slide the survivors down. dest <= i always, so ascending
     order never overwrites an unread source. Clearing `marked` here
     leaves the compacted heap clean for the next cycle. */
  for (int i = 0; i < vm->freeCount; i++) {
    Object* object = &vm->heap[i];
    if (!object->marked) continue;

    Object* dest = object->forward;
    object->marked = 0;
    if (dest != object) *dest = *object;
  }

  vm->freeCount = newCount;
  vm->numObjects = newCount;

  printf("Collected %d objects, %d remaining, compacted %d objects.\n",
         numObjects - newCount, newCount, newCount);
}
```

**4g.** `newObject()` 替换：

```c
Object* newObject(VM* vm, ObjectType type) {
  /* Heap full: collect, then insist on room. */
  if (vm->freeCount == HEAP_MAX) gc(vm);
  check(vm->freeCount < HEAP_MAX, "Heap overflow!");

  Object* object = &vm->heap[vm->freeCount++];
  object->type = type;
  object->marked = 0;

  vm->numObjects++;

  return object;
}
```

**4h.** `objectPrint` 删去 `OBJ_FORWARDED` case（枚举值已不存在；连同其注释共 5 行）。

**4i.** `freeVM()` 替换：

```c
void freeVM(VM *vm) {
  vm->stackSize = 0;
  gc(vm);
  free(vm->heap);
  free(vm);
}
```

- [ ] **Step 5: 跑测试确认通过**

Run: `cc -ggdb -std=gnu99 -Wall gc5_compact.c -o /tmp/gc5 && /tmp/gc5 > /tmp/gc5.out 2>&1; echo exit=$?; grep -cE "^Test" /tmp/gc5.out`
Expected: `exit=0`；`8`（Test 1–8 头行）；`Performance Test.` 后输出含 `compacted` 的收集行（约 2 次）。无 -Wall 警告。

- [ ] **Step 6: 全量验证并提交**

Run: `make clean >/dev/null 2>&1; make test`
Expected: 6 个 PASS（gc0–gc5）+ 3 个 EXPECTED-FAIL + `9 passed, 0 failed`。

```bash
git add gc5_compact.c
git commit -m "$(cat <<'EOF'
Add gc5: Lisp2 mark-compact with sliding three passes

One 8192-slot arena (gc3's total memory, 100% usable), iterative
marking, dedicated forward field, three compaction passes: compute
destinations in address order, rewrite roots and fields, slide
survivors down in place. Order-preserving; holes merge; capacity
test holds 5999 live objects past gc3's semispace ceiling.

Co-Authored-By: Claude Code <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: README 行 + 收尾验证

**Files:**
- Modify: `README.md`

**Interfaces:**
- Consumes: Task 1 的 gc5_compact.c
- Produces: 最终 README（六阶段路线图）

- [ ] **Step 1: README 表格加行与 diff 指引**

表格在 gc4 行后追加一行：

```markdown
| `gc5_compact.c` | Lisp2 mark-compact: three passes, sliding compaction, dedicated forward word, 100% heap usable | JVM full-GC compaction, .NET's compacting GC |
```

diff 命令块（gc4 行之后）追加一行：

```markdown
    diff gc3_copying.c   gc5_compact.c      # mark-compact (single space, in place)
```

- [ ] **Step 2: 最终全量验证**

Run: `make clean >/dev/null 2>&1; make test && ./gc5_compact | tail -3`
Expected: `9 passed, 0 failed`；gc5 尾部输出 `Collected … compacted …` 两行。

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "$(cat <<'EOF'
Add gc5 row and diff command to the README tour map

Co-Authored-By: Claude Code <noreply@anthropic.com>
EOF
)"
```

---

## 执行说明（对 worker）

- 两个任务严格线性（Task 2 依赖 Task 1 的文件存在）
- C 单文件 TDD：RED = 编译失败（Step 3），GREEN = Step 5 `exit=0`，收口 = Step 6 `make test` 全绿
- 不要动 gc0–gc4、main.c、Makefile、master 分支
- 实际输出与 Expected 不符时停下按 systematic-debugging 排查，勿硬闯
