/* gc7 — Region-based collection: the G1 ideas.
 *
 * Every stage so far had one big lever; G1's insight is composition
 * and scheduling: cut the heap into equal-sized regions and collect
 * REGION BY REGION, picking the ones that pay best.
 *
 *   1. The heap is a region table. Allocation bumps inside the eden
 *      region and just GRABS A NEW REGION when full — no collection.
 *      GC fires only when free regions run low (the ladder below),
 *      where gc3/gc4 collected on every fill.
 *   2. Evacuating a region copies its live objects into other
 *      regions and rewrites every incoming edge. Roots are rescanned
 *      each pause; non-root incoming edges come from the region's
 *      remembered set — SLOT-granular this time (the gc4s lesson's
 *      exact form): the write barrier records the slot address of any
 *      cross-region pointer store.
 *   3. Garbage-First: a stop-the-world census (gc1's worklist) counts
 *      live per region; a mixed pause then evacuates the DIRTIEST old
 *      regions first, at most CSET_BUDGET of them — bounded work per
 *      pause, and objects in clean regions simply don't move.
 *
 * The subtle invariant (see evacuateRegion): when a copy lands in
 * region D, its cross-region fields must be recorded in the TARGET
 * regions' rssets — the old object's rset entries die with the old
 * slots. Miss this and a later evacuation silently drops the edge.
 * Cross-region cycles are safe by construction: the first-evacuated
 * side's edges are noted into the not-yet-evacuated side's rset.
 *
 * What real G1 has that we skip (see gc2/gc6/gc4s for those lessons):
 * concurrent marking, aged promotion, humongous objects, and a
 * soft-real-time scheduler. Note too that real G1's young collections
 * don't consult rssets at all — they evacuate the whole young
 * generation as one closure; per-region evacuation with rset seeding
 * is this toy's uniform mechanism, and precisely where the
 * floating-garbage cascade came from. Our ladder: keep one free region in hand
 * at all times; collect young when free < 2; when that is not enough,
 * census + mixed until it is.
 *
 * diff gc3_copying.c gc7_regions.c is the whole regional delta.
 */
#include <stdio.h>
#include <stdlib.h>

#define STACK_MAX 256
#define INIT_OBJ_NUM_MAX 8
#define HEAP_MAX 8192
#define REGION_COUNT 16
#define REGION_SIZE 512   /* 16 x 512 = 8192: same total as gc3+gc5. */
#define CSET_BUDGET 2     /* Max regions evacuated per mixed pause. */

typedef enum {
  OBJ_INT,
  OBJ_PAIR,
  OBJ_FORWARDED  /* From-space husk: its union holds the new address. */
} ObjectType;

typedef struct sObject {
  ObjectType type;
  unsigned char marked;

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

typedef enum { REGION_FREE, REGION_EDEN, REGION_OLD } RegionType;

typedef struct {
  RegionType type;
  int used;   /* Bump cursor within this region. */
  int live;   /* Census result: live objects (mixed selection). */
  Object*** rset;         /* Slots pointing INTO this region. */
  int rsetCount, rsetCapacity;
} Region;

typedef struct {
  Object* stack[STACK_MAX];
  int stackSize;

  Object* heap;            /* One arena, carved into regions. */
  Region regions[REGION_COUNT];
  int edenIndex;           /* Current eden region; -1 if none. */
  int oldTarget;           /* Evacuation copy target; -1 if none. */

  int youngGcCount, mixedGcCount;
  int regionsEvacuated, objectsMoved;
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
  vm->heap = malloc(HEAP_MAX * sizeof(Object));
  for (int i = 0; i < REGION_COUNT; i++) {
    vm->regions[i].type = REGION_FREE;
    vm->regions[i].used = 0;
    vm->regions[i].live = 0;
    vm->regions[i].rset = NULL;
    vm->regions[i].rsetCount = 0;
    vm->regions[i].rsetCapacity = 0;
  }
  vm->edenIndex = -1;
  vm->oldTarget = -1;
  vm->youngGcCount = 0;
  vm->mixedGcCount = 0;
  vm->regionsEvacuated = 0;
  vm->objectsMoved = 0;
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

int regionOf(VM* vm, Object* object) {
  return (int)((object - vm->heap) / REGION_SIZE);
}

Object* regionBase(VM* vm, int r) {
  return vm->heap + (size_t)r * REGION_SIZE;
}

int freeRegions(VM* vm) {
  int free = 0;
  for (int i = 0; i < REGION_COUNT; i++) {
    if (vm->regions[i].type == REGION_FREE) free++;
  }
  return free;
}

int acquireRegion(VM* vm, RegionType type) {
  for (int i = 0; i < REGION_COUNT; i++) {
    if (vm->regions[i].type == REGION_FREE) {
      vm->regions[i].type = type;
      vm->regions[i].used = 0;
      vm->regions[i].live = 0;
      return i;
    }
  }
  return -1;
}

void rsetAdd(Region* region, Object** slot) {
  for (int i = 0; i < region->rsetCount; i++) {
    if (region->rset[i] == slot) return;   /* exact-slot dedup */
  }
  if (region->rsetCount == region->rsetCapacity) {
    region->rsetCapacity = region->rsetCapacity == 0 ? 8 : region->rsetCapacity * 2;
    region->rset = realloc(region->rset, region->rsetCapacity * sizeof(Object**));
    check(region->rset != NULL, "Out of memory growing rset.");
  }
  region->rset[region->rsetCount++] = slot;
}

/* Slot worklist: references to process, not objects. */
typedef struct {
  Object*** items;
  int count, capacity;
} Slotlist;

void slotPush(Slotlist* slots, Object** slot) {
  if (slots->count == slots->capacity) {
    slots->capacity = slots->capacity == 0 ? 64 : slots->capacity * 2;
    slots->items = realloc(slots->items, slots->capacity * sizeof(Object**));
    check(slots->items != NULL, "Out of memory growing slot list.");
  }
  slots->items[slots->count++] = slot;
}

/* Marking worklist (gc1's). */
typedef struct {
  Object** items;
  int count, capacity;
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

/* Census: mark from the roots, tally live per region, clear. */
void census(VM* vm) {
  Worklist work = {NULL, 0, 0};
  for (int i = 0; i < vm->stackSize; i++) {
    workPush(&work, vm->stack[i]);
  }
  while (work.count > 0) {
    Object* object = workPop(&work);
    if (object == NULL || object->marked) continue;
    object->marked = 1;
    if (object->type == OBJ_PAIR) {
      workPush(&work, object->head);
      workPush(&work, object->tail);
    }
  }
  free(work.items);

  /* Marks STAY SET: the evacuation below consumes them (an unmarked
     object is garbage — rset seeds owned by dead objects point at
     unmarked values and must be dropped, not followed). young/mixed
     clear the marks when the pause ends. */
  for (int r = 0; r < REGION_COUNT; r++) {
    vm->regions[r].live = 0;
    for (int j = 0; j < vm->regions[r].used; j++) {
      if (regionBase(vm, r)[j].marked) {
        vm->regions[r].live++;
      }
    }
  }
}

/* Copy target for evacuation: an old region with room, cascading to a
   fresh one. The real guarantee is the caller's up-front target budget
   (youngGC/mixedGC); this cascade is only the slot-by-slot supply
   within it. */
Object* copyIntoOld(VM* vm, Object* object) {
  if (vm->oldTarget < 0 ||
      vm->regions[vm->oldTarget].used == REGION_SIZE) {
    vm->oldTarget = acquireRegion(vm, REGION_OLD);
    check(vm->oldTarget >= 0, "Heap exhausted during evacuation.");
  }
  Object* copy = &regionBase(vm, vm->oldTarget)
                 [vm->regions[vm->oldTarget].used++];
  *copy = *object;
  copy->marked = 0;
  return copy;
}

/* Evacuate the whole collection set as ONE closure: seed from every
   stack root and every rset entry whose OWNER is not itself being
   evacuated (the cset filter — a dead owner's edge dies with it; a
   live owner inside the cset is copied here and its fields re-route
   below). Copy each cset object once, on first visit; fields pointing
   into the cset feed the worklist, fields leaving it land in the
   target region's rset. A unit closure is REQUIRED: per-region
   evacuation would lose objects reachable only from a region visited
   later in the same pause. */
int evacuateCset(VM* vm, const int* cset) {
  Slotlist slots = {NULL, 0, 0};

  for (int i = 0; i < vm->stackSize; i++) {
    if (vm->stack[i] != NULL && cset[regionOf(vm, vm->stack[i])]) {
      slotPush(&slots, &vm->stack[i]);
    }
  }
  for (int r = 0; r < REGION_COUNT; r++) {
    if (!cset[r]) continue;
    for (int i = 0; i < vm->regions[r].rsetCount; i++) {
      slotPush(&slots, vm->regions[r].rset[i]);
    }
  }

  int moved = 0;
  while (slots.count > 0) {
    Object** slot = slots.items[--slots.count];
    Object* object = *slot;
    if (object == NULL) continue;
    if (!cset[regionOf(vm, object)]) continue;   /* outside: not ours */
    if (!object->marked) continue;  /* garbage: dead owners' stale rset
                                       entries point here — drop them */

    if (object->type == OBJ_FORWARDED) {         /* second visit */
      *slot = object->forward;
      continue;
    }

    Object* copy = copyIntoOld(vm, object);
    moved++;
    object->type = OBJ_FORWARDED;
    object->forward = copy;
    *slot = copy;

    if (copy->type == OBJ_PAIR) {
      /* Route each field: targets inside the cset feed the worklist
         (they will be copied or husk-followed); targets leaving it get
         the COPY's slot into their region's rset. */
      Object** fields[2] = { &copy->head, &copy->tail };
      for (int f = 0; f < 2; f++) {
        if (*fields[f] == NULL) continue;
        if (cset[regionOf(vm, *fields[f])]) {
          slotPush(&slots, fields[f]);
        } else {
          rsetAdd(&vm->regions[regionOf(vm, *fields[f])], fields[f]);
        }
      }
    }
  }

  free(slots.items);

  for (int r = 0; r < REGION_COUNT; r++) {
    if (cset[r]) {
      vm->regions[r].type = REGION_FREE;
      vm->regions[r].used = 0;
      vm->regions[r].live = 0;
      vm->regions[r].rsetCount = 0;
      if (vm->edenIndex == r) vm->edenIndex = -1;
    } else {
      /* Clear the census marks this pause consumed. */
      for (int j = 0; j < vm->regions[r].used; j++) {
        regionBase(vm, r)[j].marked = 0;
      }
    }
  }
  return moved;
}

/* Retire fully-dead regions for free — no evacuation needed (G1
   reclaims empty regions the same way during marking). Called after a
   census, so live[] is fresh. Counts toward regionsEvacuated. */
void freeDeadRegions(VM* vm) {
  for (int r = 0; r < REGION_COUNT; r++) {
    if (vm->regions[r].type == REGION_FREE) continue;
    if (vm->regions[r].used > 0 && vm->regions[r].live == 0) {
      vm->regions[r].type = REGION_FREE;
      vm->regions[r].used = 0;
      vm->regions[r].rsetCount = 0;
      if (vm->edenIndex == r) vm->edenIndex = -1;
      vm->regionsEvacuated++;
    }
  }
}

void youngGC(VM* vm) {
  vm->youngGcCount++;
  vm->oldTarget = -1;

  /* Census first: it feeds both the free-dead pass and the target
     budget below. (Real G1 gets its liveness from the concurrent
     cycle; our young pays one stop-the-world census for it.) */
  census(vm);
  freeDeadRegions(vm);

  int cset[REGION_COUNT] = {0};
  int regions = 0;
  int live = 0;
  for (int r = 0; r < REGION_COUNT; r++) {
    if (vm->regions[r].type == REGION_EDEN && vm->regions[r].live > 0) {
      cset[r] = 1;
      regions++;
      live += vm->regions[r].live;
    }
  }

  /* Up-front target budget: unit evacuation frees cset regions only
     at the end, so the copies must fit in the free regions that exist
     NOW. (G1 pipelines this with incremental region draining; we pay
     the simpler up-front rule.) */
  int need = (live + REGION_SIZE - 1) / REGION_SIZE;
  check(freeRegions(vm) >= need, "Heap exhausted: young needs target regions");

  int moved = evacuateCset(vm, cset);
  vm->regionsEvacuated += regions;
  vm->objectsMoved += moved;
  printf("Young GC: evacuated %d objects.\n", moved);
}

void mixedGC(VM* vm) {
  vm->mixedGcCount++;
  vm->oldTarget = -1;
  census(vm);   /* fresh liveness: the selection must not use stale live */
  freeDeadRegions(vm);

  /* Dirtiest first: fewest live. Full regions are skipped (no room
     won). Up to CSET_BUDGET regions AND bounded by the target budget:
     the pause only picks regions whose live set fits the free regions
     that exist now. */
  int cset[CSET_BUDGET];
  int n = 0;
  int live = 0;
  for (int pick = 0; pick < CSET_BUDGET; pick++) {
    int best = -1;
    for (int r = 0; r < REGION_COUNT; r++) {
      if (vm->regions[r].type != REGION_OLD) continue;
      if (vm->regions[r].live == REGION_SIZE) continue;
      int taken = 0;
      for (int k = 0; k < n; k++) if (cset[k] == r) taken = 1;
      if (taken) continue;
      if (best < 0 || vm->regions[r].live < vm->regions[best].live) best = r;
    }
    if (best < 0) break;
    int need = (live + vm->regions[best].live + REGION_SIZE - 1) / REGION_SIZE;
    if (need > freeRegions(vm)) break;
    cset[n++] = best;
    live += vm->regions[best].live;
  }

  int moved = 0;
  int inCset[REGION_COUNT] = {0};
  for (int i = 0; i < n; i++) inCset[cset[i]] = 1;
  moved = evacuateCset(vm, inCset);
  vm->regionsEvacuated += n;
  vm->objectsMoved += moved;
  printf("Mixed GC: evacuated %d region(s), %d objects.\n", n, moved);
}

void ensureEden(VM* vm) {
  if (vm->edenIndex >= 0 &&
      vm->regions[vm->edenIndex].used < REGION_SIZE) return;

  /* The ladder: never spend the last free region on allocation —
     every evacuation needs at least one copy target. Young frees all
     eden regions; if that is not enough, census+mixed until it is. */
  if (freeRegions(vm) < 2) {
    youngGC(vm);
    while (freeRegions(vm) < 2) {
      int before = freeRegions(vm);
      mixedGC(vm);
      if (freeRegions(vm) == before) {
        check(0, "Heap exhausted");
      }
    }
  }
  vm->edenIndex = acquireRegion(vm, REGION_EDEN);
  check(vm->edenIndex >= 0, "Heap exhausted");
}

Object* newObject(VM* vm, ObjectType type) {
  ensureEden(vm);
  Region* eden = &vm->regions[vm->edenIndex];
  Object* object = &regionBase(vm, vm->edenIndex)[eden->used++];
  object->type = type;
  object->marked = 0;
  return object;
}

void pushInt(VM* vm, int intValue) {
  Object* object = newObject(vm, OBJ_INT);
  object->value = intValue;

  push(vm, object);
}

/* Barriered field writes: a cross-region store records the SLOT in
   the target region's remembered set. (A real VM's compiler inserts
   these; we do it by hand.) */
void objectSetHead(VM* vm, Object* pair, Object* value) {
#ifndef RSET_OFF
  if (value != NULL && regionOf(vm, value) != regionOf(vm, pair)) {
    rsetAdd(&vm->regions[regionOf(vm, value)], &pair->head);
  }
#else
  (void)vm;
#endif
  pair->head = value;
}

void objectSetTail(VM* vm, Object* pair, Object* value) {
#ifndef RSET_OFF
  if (value != NULL && regionOf(vm, value) != regionOf(vm, pair)) {
    rsetAdd(&vm->regions[regionOf(vm, value)], &pair->tail);
  }
#else
  (void)vm;
#endif
  pair->tail = value;
}

Object* pushPair(VM* vm) {
  Object* object = newObject(vm, OBJ_PAIR);
  Object* tail = pop(vm);
  Object* head = pop(vm);
  objectSetTail(vm, object, tail);
  objectSetHead(vm, object, head);
  push(vm, object);
  return object;
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

    /* Unreachable in practice: only from-space husks are FORWARDED and
       they are never printed. Here so the switch is total. */
    case OBJ_FORWARDED:
      printf("<forwarded>");
      break;
  }
}

void freeVM(VM *vm) {
  for (int r = 0; r < REGION_COUNT; r++) {
    free(vm->regions[r].rset);
  }
  free(vm->heap);
  free(vm);
}

void test1() {
  printf("Test 1: Objects on stack are preserved.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);

  youngGC(vm);
  check(vm->objectsMoved == 2, "Both objects should have moved.");
  check(vm->stack[0]->value == 1, "Content should survive the copy.");
  freeVM(vm);
}

void test2() {
  printf("Test 2: Unreached objects are collected.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);
  pop(vm);
  pop(vm);

  youngGC(vm);
  check(vm->objectsMoved == 0, "Nothing should have moved.");
  check(vm->regionsEvacuated == 1, "The eden region should be freed.");
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

  youngGC(vm);
  check(vm->objectsMoved == 7, "The whole tree should have moved.");
  Object* outer = vm->stack[vm->stackSize - 1];
  check(outer->tail->type == OBJ_PAIR, "Inner pair should survive.");
  check(outer->tail->tail->value == 4, "Leaf values should survive.");
  freeVM(vm);
}

void test4() {
  printf("Test 4: Handle cycles across regions.\n");
  VM* vm = newVM();

  pushInt(vm, 1);
  pushInt(vm, 2);
  Object* a = pushPair(vm);      /* region 0 slots 0-2 */
  pushInt(vm, 0);
  for (int i = 1; i < 256; i++) {
    pushInt(vm, i);
    pushPair(vm);
  }                              /* chain: 1 + 2*255 = 511 more */
  /* 3 + 511 = 514 > 512: the chain's tail spills into region 1, and
     B's three objects land there too. */
  pushInt(vm, 7);
  pushInt(vm, 8);
  Object* b = pushPair(vm);      /* region 1 */

  /* Cross-region cycle: A->tail = B (region 1), B->tail = A (region 0).
     Both edges land in the opposite region's rset. */
  objectSetTail(vm, a, b);
  objectSetTail(vm, b, a);

  youngGC(vm);

  /* Re-fetch after the move, then close the cycle: B'->tail is A',
     and A'->tail must lead back to B'. */
  Object* pair = vm->stack[vm->stackSize - 1];
  check(pair->tail->tail == pair, "Cycle should close across regions.");
  freeVM(vm);
}

void test5() {
  printf("Test 5: Region grabs don't collect; the ladder does.\n");
  VM* vm = newVM();

  /* Phase (a): a chain spanning three eden regions — grabbing fresh
     regions is pure bookkeeping, no collection fires. */
  pushInt(vm, 0);
  for (int i = 1; i < 768; i++) {
    pushInt(vm, i);
    pushPair(vm);
  }                              /* 1535 objects, regions 0-2 */
  check(vm->youngGcCount == 0, "Region grabs must not collect.");
  check(vm->mixedGcCount == 0, "No mixed collection either.");

  /* Phase (b): grow to the 16th region acquire — that is where free
     drops below 2 and the ladder fires young. First shrink the live
     set: a fully-live 7680-object chain would fill every old region
     and the heap would be GENUINELY exhausted (all regions full). */
  for (int i = 768; i < 3836; i++) {
    pushInt(vm, i);
    pushPair(vm);
  }                              /* 7671 objects across 15 regions */
  Object* top = vm->stack[vm->stackSize - 1];
  Object* mid = top;
  for (int i = 0; i < 3536; i++) mid = mid->head;  /* keep last 299 pairs */
  pop(vm);
  push(vm, mid);                 /* live set: 599 objects (2*299+1) */

  for (int i = 0; i < 10; i++) { /* garbage to cross the 7681st alloc */
    pushInt(vm, i);
    pop(vm);
  }

  check(vm->youngGcCount == 1, "The ladder should fire one young GC.");
  check(vm->mixedGcCount == 0, "Young alone freed enough regions.");
  /* Copy cost must track the live set, not the swept area: without
     the cset filter on rset seeds, dead owners' stale entries drag
     whole garbage chains and this moves 7168. */
  check(vm->objectsMoved == 599, "Young should move only the live set.");

  /* The kept suffix must be intact after the evacuation. */
  Object* object = vm->stack[vm->stackSize - 1];
  int seen = 0;
  while (object->type == OBJ_PAIR) {
    object = object->head;
    seen++;
  }
  check(seen == 299, "Suffix links should be intact.");
  check(object->value == 0, "Suffix end should be intact.");
  freeVM(vm);
}

void test6() {
  printf("Test 6: Mixed pauses pick the dirtiest region.\n");
  VM* vm = newVM();

  pushInt(vm, 0);
  for (int i = 1; i < 256; i++) {
    pushInt(vm, i);
    pushPair(vm);
  }
  youngGC(vm);                   /* old region A: 511 live */
  Object* top = vm->stack[vm->stackSize - 1];
  Object* mid = top;
  for (int i = 0; i < 128; i++) mid = mid->head;
  pop(vm);
  push(vm, mid);                 /* region A: 255 live (2*127+1), 256 dead */

  pushInt(vm, -1);
  pushInt(vm, 0);
  for (int i = 1; i < 256; i++) {
    pushInt(vm, i);
    pushPair(vm);
  }
  youngGC(vm);                   /* old region B: 512 live, full */
  /* Stack: [midSuffix, minusOne, cleanTop]. */
  Object* cleanBefore = vm->stack[vm->stackSize - 1];
  Object* midBefore = vm->stack[0];

  mixedGC(vm);

  Object* cleanAfter = vm->stack[vm->stackSize - 1];
  Object* midAfter = vm->stack[0];
  check(cleanAfter == cleanBefore, "A full region must not move.");
  check(midAfter != midBefore, "The dirty region's live set must move.");
  check(midAfter->type == OBJ_PAIR, "The moved object is intact.");
  freeVM(vm);
}

void test7() {
  printf("Test 7: CSET budget bounds each mixed pause.\n");
  VM* vm = newVM();

  /* Three dirty regions: 511-object chains, re-rooted to ~128. */
  for (int c = 0; c < 3; c++) {
    pushInt(vm, c);
    for (int i = 1; i < 256; i++) {
      pushInt(vm, i);
      pushPair(vm);
    }
    youngGC(vm);
    Object* top = vm->stack[vm->stackSize - 1];
    Object* mid = top;
    for (int i = 0; i < 128; i++) mid = mid->head;
    pop(vm);
    push(vm, mid);
  }

  int before = vm->regionsEvacuated;
  mixedGC(vm);
  check(vm->regionsEvacuated - before == CSET_BUDGET,
        "A mixed pause evacuates at most CSET_BUDGET regions.");

  /* Pause two also evacuates a full budget: the third dirty region
     (never touched) plus pause one's copy-target region, which census
     now sees as 510-live — near-full all-live regions get re-picked
     because the metric is absolute liveness. Real G1 applies a
     liveness THRESHOLD here. The assertion is the point: every pause
     is bounded regardless. */
  mixedGC(vm);
  check(vm->regionsEvacuated - before == 2 * CSET_BUDGET,
        "Every mixed pause is bounded by CSET_BUDGET.");
  freeVM(vm);
}

void test8() {
  printf("Test 8: The remembered set saves a cross-region reference.\n");
  VM* vm = newVM();

  /* Fill old region O0 exactly: P's three objects + a 509-object
     chain (1 base int + 254 pairs = 509). */
  pushInt(vm, 1);
  pushInt(vm, 2);
  pushPair(vm);                  /* P = (1, 2) */
  pushInt(vm, 0);
  for (int i = 1; i < 255; i++) {
    pushInt(vm, i);
    pushPair(vm);
  }                              /* 509 more -> 512 total in O0 */
  youngGC(vm);                   /* O0 full (512 live) */

  Object* p = vm->stack[0];      /* re-fetch P from the stack */

  /* X is born in a fresh eden region, referenced ONLY via P->tail. */
  pushInt(vm, 42);
  Object* x = vm->stack[vm->stackSize - 1];
  objectSetTail(vm, p, x);       /* cross-region: slot -> eden rset */
  pop(vm);                       /* drop the root; P->tail is all X has */

  youngGC(vm);                   /* evacuates the eden region via O0's edge */

  /* Reuse the freed eden region: its slot 0 held X. */
  pushInt(vm, 7);

  /* ON: X lives in an old region, untouched by the overwrite.
     OFF (gc7_bo): X was swept with its region and P->tail reads the
     overwrite — observable by value, no UB (the arena persists). */
  check(p->tail->value == 42, "Cross-region edge should survive via rset.");
  freeVM(vm);
}

void exhaustTest() {
  printf("Exhaust Test: a fully-live heap is honestly refused.\n");
  VM* vm = newVM();
  /* One rooted chain bigger than the heap can hold: the ladder runs
     young, then mixed, then admits defeat via check. Only compiled
     into the EXHAUST_TEST build. */
  pushInt(vm, 0);
  for (int i = 1; i < 4100; i++) {
    pushInt(vm, i);
    pushPair(vm);
  }
  printf("ERROR: exhaustion was not caught.\n");
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
  printf("%d young GCs, %d mixed GCs, %d regions evacuated, %d objects moved.\n",
         vm->youngGcCount, vm->mixedGcCount,
         vm->regionsEvacuated, vm->objectsMoved);
  /* Deterministic: the ladder fires exactly at the two 7680-object
     region boundaries, with the churn stack empty both times. */
  check(vm->youngGcCount == 2, "The ladder fires exactly twice.");
  /* Churn's live set is tiny, so each young frees ~14 regions and free
     never runs low enough to need a mixed pause. */
  check(vm->mixedGcCount == 0, "Young alone should sustain churn.");
  freeVM(vm);
}

int main(int argc, const char * argv[]) {
  test1();
  test2();
  test3();
#ifndef RSET_OFF
  /* t4's cross-region cycle and t5's trans-region chain rewrite their
     edges through region rssets — exactly what RSET_OFF removes — so
     both fail before test8 can. Skip them in the OFF build so its
     failure lands on test8, the designated demonstration. */
  test4();
  test5();
#endif
  test6();
  test7();
  test8();
  perfTest();

#ifdef EXHAUST_TEST
  exhaustTest();
#endif

  return 0;
}
