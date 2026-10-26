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
 * drops to 83 minors, 0 majors, 0 promotions — only the zeros are
 * pinned by perfTest's checks; the minor count drifts up (78 -> 83)
 * because survivors occupy eden's tail between collections.
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
#include <stdio.h>
#include <stdlib.h>

#define STACK_MAX 256
#define NURSERY_MAX 256
#define AGE_MAX 2
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
  unsigned char age;        /* Minors survived; AGE_MAX promotes. */

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

  /* Young generation: two contiguous spaces swapping roles each
     minor. Between minors only eden (nursery[nurserySpace]) is live;
     the other space is scratch until the next collection copies
     into it. */
  Object* nursery[2];
  int nurserySpace;   /* 0 or 1: which space is eden right now. */
  int nurseryCount;   /* Bump cursor in eden. */
  int survivorCount;  /* During a minor: bump cursor in survivor space. */

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
  vm->nursery[0] = malloc(NURSERY_MAX * sizeof(Object));
  vm->nursery[1] = malloc(NURSERY_MAX * sizeof(Object));
  vm->nurserySpace = 0;
  vm->nurseryCount = 0;
  vm->survivorCount = 0;
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
  if (object == NULL) return 0;
  return (object >= vm->nursery[0] && object < vm->nursery[0] + NURSERY_MAX)
      || (object >= vm->nursery[1] && object < vm->nursery[1] + NURSERY_MAX);
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

  /* Keep remembered-set entries whose old object still points into
     the nursery: under aging an old->young edge can outlive many
     minors, and clearing it here would silently lose the young object
     at the next one. (gc4 could clear unconditionally — immediate
     promotion meant no edge ever survived a minor.) */
  int kept = 0;
  for (int i = 0; i < vm->remsetCount; i++) {
    Object* old = vm->remset[i];
    /* Pair guard mirrors gc4's scan loop: only pairs can be recorded,
       but misuse must not alias an int's union storage. */
    if (old->type == OBJ_PAIR &&
        (isYoung(vm, old->head) || isYoung(vm, old->tail))) {
      vm->remset[kept++] = old;
    } else {
      old->remembered = 0;
    }
  }
  vm->remsetCount = kept;

  /* The survivor space is the new eden; the old eden is scratch. */
  vm->nurserySpace = 1 - vm->nurserySpace;
  vm->nurseryCount = vm->survivorCount;
  vm->survivorCount = 0;
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

  /* Promote every live young object first: the sweep below only
     walks the old list, so everything live must be on it. */
  copyYoung(vm, 1);

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
  copyYoung(vm, 0);
  printf("Minor GC: old gen at %d objects.\n", vm->oldCount);

  if (vm->oldCount >= vm->oldMax) majorGC(vm);
}

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

  /* gc4_generational.c tells the honest story of promote-on-first-
     survival on this same workload: 32 majors out of 78 collections,
     because every short-lived int on the stack at a nursery fill got
     tenured. Aging fixes exactly that; the checks below pin it. */
  printf("%d minor GCs, %d major GCs, %d promotions.\n",
         vm->minorGcCount, vm->majorGcCount, vm->promotedCount);

  /* The headline this stage exists for: on this churn workload every
     object dies before AGE_MAX, so nothing is ever promoted and the
     old gen never crosses oldMax — zero majors, where first-survival
     promotion ran 32 majors out of 78 collections (gc4's perfTest).
     Deterministic: minors fire at nursery fills, when the live set is
     at most one round's 20 fresh age-0 ints. */
  check(vm->majorGcCount == 0, "Aging should eliminate majors on churn.");
  check(vm->promotedCount == 0, "Churn should never be promoted.");
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
