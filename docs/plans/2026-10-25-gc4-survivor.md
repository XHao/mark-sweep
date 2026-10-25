# gc4_survivor（年龄阈值 + survivor 空间）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 追加 `gc4_survivor.c`——双空间 nursery + AGE_MAX 老化晋升，把 gc4 的 78/32/660 修成 **78/0/0** 并用确定性断言钉死。

**Architecture:** 从 `gc4_generational.c` 派生：slot worklist 复制点加"存活年轻 vs 晋升"分流，nursery 升级为互换双空间（gc3 空间互换技巧的年轻代内复用），其余机制（remset 屏障、majorGC、老年代链表）逐字沿用。

**Tech Stack:** C（`-ggdb -std=gnu99 -Wall`）、POSIX make、macOS clang。零依赖。

**Spec:** `docs/2026-10-25-gc4-survivor-design.md`

## Global Constraints

- C99 零依赖；`-Wall` 无新警告；gc0–gc6 与 main.c 逐字节不动
- `NURSERY_MAX 256`（每空间）、`AGE_MAX 2`、`INIT_OLD_OBJ_NUM_MAX 8`
- 每任务结束 `make test` 全绿（最终 **8 PASS + 5 EXPECTED-FAIL = 13 passed**；无新 flag 构建）
- 提交信息英文祈使句 + `Co-Authored-By: Claude Code <noreply@anthropic.com>`；分支 `gc4-survivor`
- 测试编号沿用 gc4 的 t1–t7 槽位（原地改写语义），perfTest 加头条断言；无新增编号
- 继承的 `#ifndef REMSET_OFF` ifdef 保留（不构建，机制同 gc4_bo 的课）

## Review Focus

1. **空间互换错位**（swap/游标恢复写错 → 跨轮丢对象或同槽双配）→ t1 双轮计数、t5 三轮跨空间追踪（x 出生→survivor→old 的三次地址变化）、t6 两轮 200 对象
2. **年龄边界差一**（`age+1 >= AGE_MAX` 写错 → 过早/过晚晋升）→ t1/t6 的精确 oldCount/promotedCount 断言
3. **晋升副本字段陈旧**（gc4 轮 minor#8 的教训：marked/remembered 未清）→ 复制点显式清 `age/remembered/marked` + t3 两轮后结构遍历
4. **头条断言脆弱** → 固定负载的确定性论证写进测试注释（千轮 churn，minor 时存活集 ≤20 且全为 age 0）
5. **survivor 溢出不可测路径** → check 分支存在且注释指向 HotSpot 的差异处理；测试最大年轻存活集 200 < 256（余量 56）

---

### Task 1: gc4_survivor.c

**Files:**
- Create: `gc4_survivor.c`

**Interfaces:**
- Consumes: `gc4_generational.c`（派生源；`Slotlist`/`remsetAdd`/`objectSetHead/Tail`/`majorGC` 骨架逐字沿用）
- Produces: 自包含收集器 `gc4_survivor`；关键新形态 `void copyYoung(VM* vm, int forceOld)`（取代 `promoteAll`）；VM 字段 `nursery[2]/nurserySpace/nurseryCount/survivorCount`

- [ ] **Step 1: 复制起步**

```bash
cp gc4_generational.c gc4_survivor.c
```

替换头部注释块为：

```c
/* gc4_survivor — Age thresholds + survivor space: fixing gc4's promotion bug.
 *
 * gc4 promoted every young object on FIRST survival. On a churn
 * workload that is exactly backwards: the short-lived objects that
 * happened to sit on the stack at a nursery fill got tenured, the old
 * gen polluted, and majors fired almost as often as minors (gc4's
 * perfTest: 78 minors, 32 majors, 636 promotions).
 *
 * The fix is what HotSpot ships: a two-space nursery where eden and
 * survivor swap roles each collection. Survivors are copied into the
 * other space and AGE; only after AGE_MAX survivals do they promote
 * to the old gen. Short-lived objects now die young before the
 * threshold — in scratch space, at zero cost. The same churn workload
 * drops to 78 minors, 0 majors, 0 promotions (pinned by perfTest).
 *
 * Mechanically this is gc4's slot worklist with one decision added at
 * the copy point (survive-young vs promote), plus the gc3
 * space-swapping trick applied inside the nursery. HotSpot differs in
 * the details — it has two survivor spaces and promotes early on
 * survivor overflow where we simply check-fail — and packs the age
 * into the mark word rather than a dedicated byte.
 *
 * diff gc4_generational.c gc4_survivor.c is the whole aging delta.
 */
```

- [ ] **Step 2: 写失败测试（改写 t1–t7 + perfTest 断言）**

test1 替换为（老化演示）：

```c
void test1() {
  printf("Test 1: Objects age in the nursery before promotion.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);

  minorGC(vm);
  check(vm->nurseryCount == 2, "First survival should keep objects young.");
  check(vm->oldCount == 0, "Nothing should be promoted yet.");
  check(vm->promotedCount == 0, "No promotions should have happened.");

  minorGC(vm);
  check(vm->oldCount == 2, "AGE_MAX survival should promote both.");
  check(vm->promotedCount == 2, "Both promotions counted.");
  freeVM(vm);
}
```

test2 替换为：

```c
void test2() {
  printf("Test 2: Unreached objects are collected.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);
  pop(vm);
  pop(vm);

  minorGC(vm);
  check(vm->nurseryCount == 0, "Nothing should survive.");
  check(vm->oldCount == 0, "Nothing should be promoted.");
  freeVM(vm);
}
```

test3 替换为：

```c
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
  check(vm->nurseryCount == 7, "The tree should survive young.");
  minorGC(vm);
  check(vm->oldCount == 7, "Second survival should promote the tree.");

  /* The nested structure must survive two copies intact: copies'
     fields must point at copies (the gc4 lesson, twice over). */
  Object* outer = vm->stack[vm->stackSize - 1];
  check(outer->tail->type == OBJ_PAIR, "Inner pair should survive.");
  check(outer->tail->tail->value == 4, "Leaf values should survive.");
  freeVM(vm);
}
```

test4 替换为：

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

  /* Young-to-young writes need no barrier. */
  a->tail = b;
  b->tail = a;

  minorGC(vm);
  check(vm->nurseryCount == 4, "The cycle should survive young.");
  minorGC(vm);
  check(vm->oldCount == 4, "Cycle should survive promotion intact.");

  /* Re-fetch: a and b were young pointers, now promoted. */
  Object* pair = vm->stack[vm->stackSize - 1];
  check(pair->tail->head != NULL, "Cycle should still close.");
  freeVM(vm);
}
```

test5 替换为：

```c
void test5() {
  printf("Test 5: The remembered set saves an old->young reference.\n");
  VM* vm = newVM();

  pushInt(vm, 2);
  pushInt(vm, 3);
  pushPair(vm);
  minorGC(vm);                /* survivors: still young */
  minorGC(vm);                /* AGE_MAX: promoted; nursery empty */

  /* Re-fetch the old pair (the young pointer is a stale husk). */
  Object* p = vm->stack[vm->stackSize - 1];
  check(vm->oldCount == 3, "Pair should be old now.");

  /* X is young and reachable ONLY through old p's tail. */
  pushInt(vm, 42);
  Object* x = vm->stack[vm->stackSize - 1];
  objectSetTail(vm, p, x);    /* barrier fires: old p gets remembered */
  pop(vm);                    /* drop the root; p->tail is all X has */

  minorGC(vm);                /* X survives YOUNG (age 1) via remset */
  minorGC(vm);                /* X reaches AGE_MAX: promoted */

  /* After two swaps the allocator is back in the space where X was
     born; pushInt(7) overwrites that slot. A saved X lives in the
     old gen, untouched — a lost X would read the overwrite. The
     nursery memory is never freed mid-run, so this is observable by
     value with no UB. */
  pushInt(vm, 7);

  check(p->tail->value == 42, "Young object should survive via remset.");
  check(vm->oldCount == 4, "X should have been promoted.");
  check(vm->promotedCount == 4, "Promotions counted.");
  freeVM(vm);
}
```

test6 替换为：

```c
void test6() {
  printf("Test 6: Aging holds majors back until real tenure.\n");
  VM* vm = newVM();

  /* 200 live ints (stay under STACK_MAX == 256 and NURSERY_MAX). */
  for (int i = 0; i < 200; i++) {
    pushInt(vm, i);
  }

  minorGC(vm);
  check(vm->nurseryCount == 200, "All survivors stay young after one minor.");
  check(vm->majorGcCount == 0, "Young survivors must not fire a major.");
  check(vm->oldCount == 0, "Nothing promoted yet.");

  minorGC(vm);
  check(vm->oldCount == 200, "Second survival promotes all 200.");
  check(vm->majorGcCount == 1, "Old gen growth should run a major.");
  check(vm->promotedCount == 200, "Promotions counted.");

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
```

test7 替换为：

```c
void test7() {
  printf("Test 7: NULL field values are safe.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);
  pushPair(vm);
  minorGC(vm);
  minorGC(vm);

  /* Re-fetch the old pair (the young pointer is a stale husk). */
  Object* p = vm->stack[vm->stackSize - 1];

  /* Storing NULL must not be range-checked into "is young". */
  objectSetTail(vm, p, NULL);
  minorGC(vm);
  check(p->tail == NULL, "NULL should stay NULL.");
  freeVM(vm);
}
```

perfTest 的统计 printf 之后、`freeVM(vm);` 之前插入头条断言：

```c
  /* The headline this stage exists for: on this churn workload every
     object dies before AGE_MAX, so nothing is ever promoted and the
     old gen never crosses oldMax — zero majors, where first-survival
     promotion ran 32 majors out of 78 collections (gc4's perfTest).
     Deterministic: minors fire at nursery fills, when the live set is
     at most one round's 20 fresh age-0 ints. */
  check(vm->majorGcCount == 0, "Aging should eliminate majors on churn.");
  check(vm->promotedCount == 0, "Churn should never be promoted.");
```

`main()` 不变（t1–t7 + perfTest 槽位沿用）。

- [ ] **Step 3: 跑测试确认失败**

Run: `cc -ggdb -std=gnu99 -Wall gc4_survivor.c -o /tmp/gc4s && /tmp/gc4s > /tmp/gc4s.out 2>&1; echo exit=$?; tail -1 /tmp/gc4s.out`
Expected: 编译通过（新测试只用 gc4 已有标识符），运行时死于 **Test 1** 的 `First survival should keep objects young.`——gc4 残余的 promoteAll 把两个 int 直接晋升，`nurseryCount` 为 0 ≠ 2。这正是本阶段要修的行为，确定性 RED。

- [ ] **Step 4: 实现老化与双空间**

**4a.** 宏替换：

```c
#define NURSERY_MAX 256
#define INIT_OLD_OBJ_NUM_MAX 8
```
→
```c
#define NURSERY_MAX 256
#define AGE_MAX 2
#define INIT_OLD_OBJ_NUM_MAX 8
```

**4b.** `sObject` 的 `unsigned char remembered;` 行后加：

```c
  unsigned char age;        /* Minors survived; AGE_MAX promotes. */
```

**4c.** VM 的 nursery 字段替换：

```c
  /* Young generation: malloc'd objects on a linked list. */
```
（上一行为 old gen 注释——VM 中原 young 段）
原：
```c
  /* Young generation: one contiguous, bump-allocated arena. */
  Object* nursery;
  int nurseryCount;
```
替换为：
```c
  /* Young generation: two contiguous spaces swapping roles each
     minor. Between minors only eden (nursery[nurserySpace]) is live;
     the other space is scratch until the next collection copies
     into it. */
  Object* nursery[2];
  int nurserySpace;   /* 0 or 1: which space is eden right now. */
  int nurseryCount;   /* Bump cursor in eden. */
  int survivorCount;  /* During a minor: bump cursor in survivor space. */
```

**4d.** `newVM()` 的 nursery 初始化替换：

```c
  vm->nursery = malloc(NURSERY_MAX * sizeof(Object));
  vm->nurseryCount = 0;
```
→
```c
  vm->nursery[0] = malloc(NURSERY_MAX * sizeof(Object));
  vm->nursery[1] = malloc(NURSERY_MAX * sizeof(Object));
  vm->nurserySpace = 0;
  vm->nurseryCount = 0;
  vm->survivorCount = 0;
```

**4e.** `isYoung` 整体替换：

```c
/* Which generation does this object live in? NULL is neither — the
   explicit check keeps NULL out of the pointer range compares. */
int isYoung(VM* vm, Object* object) {
  if (object == NULL) return 0;
  return (object >= vm->nursery[0] && object < vm->nursery[0] + NURSERY_MAX)
      || (object >= vm->nursery[1] && object < vm->nursery[1] + NURSERY_MAX);
}
```

**4f.** `promoteAll` 整体替换为 `copyYoung`：

```c
/* Copy every young object reachable from the roots and the remembered
   set, then swap the nursery spaces. Survivors below AGE_MAX stay
   young (copied into the other space, age + 1); the rest promote to
   the old gen. forceOld (majorGC's pre-pass) promotes everyone.
   Young garbage simply vanishes — copying a young generation is free
   precisely because nothing is swept. */
void copyYoung(VM* vm, int forceOld) {
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

    Object* copy;
    if (object->age + 1 >= AGE_MAX || forceOld) {
      /* Old enough: promote. */
      copy = malloc(sizeof(Object));
      *copy = *object;
      copy->age = 0;
      copy->remembered = 0;
      copy->marked = 0;
      copy->next = vm->oldFirst;
      vm->oldFirst = copy;
      vm->oldCount++;
      vm->promotedCount++;
    } else {
      /* Not yet: age in the survivor space. (HotSpot promotes early
         on survivor overflow instead of failing; one mechanism
         fewer here.) */
      check(vm->survivorCount < NURSERY_MAX, "Survivor overflow!");
      copy = &vm->nursery[1 - vm->nurserySpace][vm->survivorCount++];
      *copy = *object;
      copy->age = object->age + 1;
      copy->remembered = 0;
    }

    object->type = OBJ_FORWARDED;
    object->forward = copy;
    *slot = copy;

    if (copy->type == OBJ_PAIR) {
      slotPush(&slots, &copy->head);
      slotPush(&slots, &copy->tail);
    }
  }

  free(slots.items);

  /* The survivor space is the new eden; the old eden is scratch. */
  vm->nurserySpace = 1 - vm->nurserySpace;
  vm->nurseryCount = vm->survivorCount;
  vm->survivorCount = 0;
  vm->remsetCount = 0;
}
```

**4g.** `majorGC` 开头（`vm->majorGcCount++;` 之后）插入强制晋升。注意 gc4 的 majorGC 原本不调 promoteAll——晋升由触发路径（minorGC 先跑 copyYoung）隐式保证；gc4_survivor 改为**显式**前置，t6 的 major 断言依赖它：

```c
void majorGC(VM* vm) {
  vm->majorGcCount++;

  /* Promote every live young object first: the sweep below only
     walks the old list, so everything live must be on it. */
  copyYoung(vm, 1);
```
（其余标记清扫逐字不变。）

**4h.** `minorGC` 内 `promoteAll(vm);` → `copyYoung(vm, 0);`。

**4i.** `newObject` 替换：

```c
Object* newObject(VM* vm, ObjectType type) {
  /* Eden full: collect it. After the swap the cursor equals the
     survivor count, so there is room unless survivors filled the
     space (then the check fires honestly). */
  if (vm->nurseryCount == NURSERY_MAX) minorGC(vm);
  check(vm->nurseryCount < NURSERY_MAX, "Nursery overflow!");

  Object* object = &vm->nursery[vm->nurserySpace][vm->nurseryCount++];
  object->type = type;
  object->marked = 0;
  object->remembered = 0;
  object->age = 0;

  return object;
}
```

**4j.** `freeVM` 替换（promoteAll → copyYoung、双空间释放）：

```c
void freeVM(VM *vm) {
  vm->stackSize = 0;
  copyYoung(vm, 1);  /* young objects still referenced via the remset
                        get promoted, so teardown is uniform */

  Object* object = vm->oldFirst;
  while (object) {
    Object* next = object->next;
    free(object);
    object = next;
  }

  free(vm->nursery[0]);
  free(vm->nursery[1]);
  free(vm->remset);
  free(vm);
}
```

- [ ] **Step 5: 跑测试确认通过**

Run: `cc -ggdb -std=gnu99 -Wall gc4_survivor.c -o /tmp/gc4s 2>/tmp/gc4s.warn && /tmp/gc4s > /tmp/gc4s.out 2>&1; echo exit=$?; grep -cE "^Test" /tmp/gc4s.out; wc -c < /tmp/gc4s.warn; grep "minor GCs" /tmp/gc4s.out | tail -1`
Expected: `exit=0`；`7`（t1–t7 头行）；0 字节警告；统计行形如 `78 minor GCs, 0 major GCs, 0 promotions.`

- [ ] **Step 6: 全量验证并提交**

Run: `make clean >/dev/null 2>&1; make test 2>&1 | grep -v "^cc "`
Expected: 8 个 PASS（含 `gc4_survivor`）+ 5 个 EXPECTED-FAIL + `13 passed, 0 failed`。

```bash
git add gc4_survivor.c
git commit -m "$(cat <<'EOF'
Add gc4_survivor: age thresholds and survivor spaces

Two nursery spaces swap roles each minor; survivors age in place
and only promote after AGE_MAX survivals. Churn that poisoned gc4's
old gen (32 majors out of 78 collections) now dies young at zero
cost — perfTest pins 78/0/0 with deterministic assertions. gc4
stays as the honest first-cut lesson; the diff between the files is
the aging delta.

Co-Authored-By: Claude Code <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: README 行 + 收尾验证

**Files:**
- Modify: `README.md`

- [ ] **Step 1: README 更新**

表格 gc4 行后插：

```markdown
| `gc4_survivor.c` | age thresholds, eden/survivor space swap, promote on AGE_MAX — gc4's promotion bug fixed | HotSpot young gen (survivor spaces + tenuring) |
```

diff 命令块 gc4 行后插：

```markdown
    diff gc4_generational.c gc4_survivor.c  # aging: survivor spaces + AGE_MAX
```

首段 `seven-stage tour` → `eight-stage tour`。

- [ ] **Step 2: 最终全量验证**

Run: `make clean >/dev/null 2>&1; make test 2>&1 | tail -2 && ./gc4_survivor | tail -1`
Expected: `13 passed, 0 failed`；尾行 `Collected ... / 0 promotions` 相关输出或统计行。

- [ ] **Step 3: 提交**

```bash
git add README.md
git commit -m "$(cat <<'EOF'
Add gc4_survivor row and diff command to the README tour map

Co-Authored-By: Claude Code <noreply@anthropic.com>
EOF
)"
```

---

## 执行说明（对 worker）

- 两个任务严格线性；RED = Step 3 运行时 t1 失败（gc4 残余直接晋升），GREEN = Step 5，收口 = Step 6 `13 passed`
- 4g 处注意：gc4 的 `majorGC` 原本不调 promoteAll——是 minorGC 触发的路径保证了"先晋升"；gc4_survivor 的 majorGC 改为**开头显式** `copyYoung(vm, 1)`，两者语义等价但显式更稳（t6 的 major 断言依赖它）
- 不要动 gc0–gc6、main.c、master 分支
- 实际输出与 Expected 不符时停下按 systematic-debugging 排查
