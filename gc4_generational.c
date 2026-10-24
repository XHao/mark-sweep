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
 * than slots — simpler, and the comment at promoteAll notes what real
 * collectors pay for that choice.
 */
#include <stdio.h>
#include <stdlib.h>

#define STACK_MAX 256
#define NURSERY_MAX 256
#define INIT_OLD_OBJ_NUM_MAX 8

typedef enum {
  OBJ_INT,
  OBJ_PAIR,
  OBJ_FORWARDED  /* Nursery husk: its union holds the promoted copy. */
} ObjectType;

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

void check(int condition, const char* message) {
  if (!condition) {
    printf("%s\n", message);
    exit(1);
  }
}

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


void push(VM* vm, Object* value) {
  check(vm->stackSize < STACK_MAX, "Stack overflow!");
  vm->stack[vm->stackSize++] = value;
}


Object* pop(VM* vm) {
  check(vm->stackSize > 0, "Stack underflow!");
  return vm->stack[--vm->stackSize];
}

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

void pushInt(VM* vm, int intValue) {
  Object* object = newObject(vm, OBJ_INT);
  object->value = intValue;

  push(vm, object);
}

Object* pushPair(VM* vm) {
  Object* object = newObject(vm, OBJ_PAIR);
  object->tail = pop(vm);
  object->head = pop(vm);

  push(vm, object);
  return object;
}

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

void objectPrint(Object* object) {
  switch (object->type) {
    case OBJ_INT:
      printf("%d", object->value);
      break;

    case OBJ_PAIR:
      printf("(");
      objectPrint(object->head);
      printf(", ");
      objectPrint(object->tail);
      printf(")");
      break;

    /* Unreachable in practice: only nursery husks are FORWARDED and
       they are never printed. Here so the switch is total. */
    case OBJ_FORWARDED:
      printf("<forwarded>");
      break;
  }
}

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
     copies' fields must point at promoted copies. */
  Object* outer = vm->stack[vm->stackSize - 1];
  check(outer->tail->type == OBJ_PAIR, "Inner pair should survive.");
  check(outer->tail->tail->value == 4, "Leaf values should survive.");
  freeVM(vm);
}

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

void perfTest() {
  printf("Performance Test.\n");
  VM* vm = newVM();

  for (int i = 0; i < 1000; i++) {
    for (int j = 0; j < 20; j++) {
      pushInt(vm, i);
    }

    for (int k = 0; k < 20; k++) {
      pop(vm);
    }
  }

  /* Honest numbers for this workload: minors far outnumber majors,
    but majors do fire regularly. Promote-on-first-survival (the
    simplification this file teaches) promotes every short-lived int
    that happens to sit on the stack when the nursery fills, so the
    old gen pollutes and a major is needed to sweep it. Real JVMs add
    age thresholds and survivor spaces for exactly this reason — see
    the header. A workload with a long-lived resident set and
    immediate-drop churn would show ~1 major in ~78 minors, but it
    would no longer be the same program gc3 runs, and comparing
    collection counts across different programs would be dishonest. */
  printf("%d minor GCs, %d major GCs, %d promotions.\n",
         vm->minorGcCount, vm->majorGcCount, vm->promotedCount);
  freeVM(vm);
}

int main(int argc, const char * argv[]) {
  test1();
  test2();
  test3();
  test4();
  test5();
  test6();
  test7();
  perfTest();

  return 0;
}
