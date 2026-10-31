/* gc10 — The cycle collector: reference counting + tracing, hybrid.
 *
 * gc9 pinned the paradigm's fatal flaw: a rootless cycle leaks its
 * entire closure, because every count is held from inside the island.
 * The industrial fix — Python's gc family, Bacon's synchronous
 * trial-deletion — is this file, and it is the series' finale: the
 * tracing vocabulary (a worklist, three scratch colors) comes back to
 * rescue counting.
 *
 * The algorithm, in four moves:
 *
 *   1. Candidate buffering. Only objects whose count DROPPED without
 *      reaching zero can head a cycle (Bacon's insight), so release()
 *      buffers them; steady-state objects never enter the pass.
 *   2. Trial deletion (mark-gray). From each candidate, walk the
 *      children and decrement every internal edge once (the gray
 *      guard makes it once-per-object). External references keep
 *      their counts positive; intra-island counts fall to zero.
 *   3. Scan. A trial count > 0 means the object is reachable from
 *      outside the island: scan_black restores (+1) along its edges
 *      and propagates blackness — liveness spreads. The rest whiten.
 *   4. Collect white. The remaining whites are self-sustaining
 *      islands; they free outright (no release cascade — the counts
 *      are meaningless now), blackened on entry so island cycles
 *      cannot revisit.
 *
 * The invariant, one sentence: any edge not trial-deleted leaves its
 * target's count positive, and any trial-deleted edge from a live
 * source is restored by scan-black's propagation — so neither
 * miskill nor leak is possible.
 *
 * diff gc9_refcount.c gc10_cycles.c is the whole hybrid delta.
 */
#include <stdio.h>
#include <stdlib.h>

typedef enum {
  GC_WHITE,
  GC_GRAY,
  GC_BLACK
} GCColor;

#define STACK_MAX 256

typedef enum {
  OBJ_INT,
  OBJ_PAIR
} ObjectType;

typedef struct sObject {
  ObjectType type;
  int refCount;
  GCColor gcColor;            /* Cycle-collection scratch (trial state). */
  unsigned char buffered;     /* Already in the candidate buffer? */

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

typedef struct {
  Object* stack[STACK_MAX];
  int stackSize;

  /* Statistics: the paradigm's two observable faces — instant
     reclamation and per-write cost. */
  int liveObjects;
  int countOps;

  /* Cycle collection: Bacon's candidate buffer + statistics. */
  Object** cycleRoots;
  int cycleRootsCount, cycleRootsCapacity;
  int cyclesCollected, cycleFreed;
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
  vm->liveObjects = 0;
  vm->countOps = 0;
  vm->cycleRoots = NULL;
  vm->cycleRootsCount = 0;
  vm->cycleRootsCapacity = 0;
  vm->cyclesCollected = 0;
  vm->cycleFreed = 0;
  return vm;
}


void cycleRootsAdd(VM* vm, Object* object) {
  if (vm->cycleRootsCount == vm->cycleRootsCapacity) {
    vm->cycleRootsCapacity =
        vm->cycleRootsCapacity == 0 ? 16 : vm->cycleRootsCapacity * 2;
    vm->cycleRoots = realloc(vm->cycleRoots,
                             vm->cycleRootsCapacity * sizeof(Object*));
    check(vm->cycleRoots != NULL, "Out of memory growing cycle roots.");
  }
  vm->cycleRoots[vm->cycleRootsCount++] = object;
}

/* A dying candidate must leave the buffer before its memory frees,
   or the next collection would walk freed objects. */
void unbuffer(VM* vm, Object* object) {
  for (int i = 0; i < vm->cycleRootsCount; i++) {
    if (vm->cycleRoots[i] == object) {
      vm->cycleRoots[i] = vm->cycleRoots[--vm->cycleRootsCount];
      break;
    }
  }
  object->buffered = 0;
}

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
    unbuffer(vm, object);
    free(object);
    vm->liveObjects--;
  } else if (!object->buffered) {
    /* Bacon's insight: only a count that DROPPED without dying can
       head a cycle — buffer it as a collection candidate. */
    object->buffered = 1;
    cycleRootsAdd(vm, object);
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

typedef struct {
  Object** items;
  int count, capacity;
} Worklist;

void workPush(Worklist* work, Object* object) {
  if (object == NULL) return;   /* NULL fields are legitimate */
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

/* Scan-black: externally alive — restore the trial-deleted edges
   (+1) and propagate blackness (liveness spreads). */
void scanBlack(VM* vm, Object* object) {
  Worklist work = {NULL, 0, 0};
  workPush(&work, object);
  while (work.count > 0) {
    Object* x = workPop(&work);
    if (x->gcColor == GC_BLACK) continue;
    x->gcColor = GC_BLACK;
    if (x->type == OBJ_PAIR) {
      Object* children[2] = {x->head, x->tail};
      for (int i = 0; i < 2; i++) {
        if (children[i] != NULL) {
          children[i]->refCount++;
          workPush(&work, children[i]);
        }
      }
    }
  }
  free(work.items);
}

void collectCycles(VM* vm) {
  int n = vm->cycleRootsCount;
  if (n == 0) return;

  /* Consume the candidate buffer. */
  Object** candidates = malloc(n * sizeof(Object*));
  check(candidates != NULL, "Out of memory for candidates.");
  for (int i = 0; i < n; i++) {
    candidates[i] = vm->cycleRoots[i];
    candidates[i]->buffered = 0;
  }
  vm->cycleRootsCount = 0;

  Worklist work = {NULL, 0, 0};

  /* Phase 1 — trial deletion: every internal edge loses 1. The gray
     guard makes each object's children decrement exactly once. */
  for (int i = 0; i < n; i++) workPush(&work, candidates[i]);
  while (work.count > 0) {
    Object* x = workPop(&work);
    if (x->gcColor == GC_GRAY) continue;
    x->gcColor = GC_GRAY;
    if (x->type == OBJ_PAIR) {
      Object* children[2] = {x->head, x->tail};
      for (int i = 0; i < 2; i++) {
        if (children[i] != NULL) {
          children[i]->refCount--;
          workPush(&work, children[i]);
        }
      }
    }
  }

  /* Phase 2 — scan: trial count > 0 means externally reachable;
     scan-black restores and blackens. The rest whiten. (The
     NO_SCAN_BLACK build removes the rescue — the miskill killer.) */
  for (int i = 0; i < n; i++) workPush(&work, candidates[i]);
  while (work.count > 0) {
    Object* x = workPop(&work);
    if (x->gcColor != GC_GRAY) continue;   /* already decided */
#ifdef NO_SCAN_BLACK
    if (0) {
#else
    if (x->refCount > 0) {
#endif
      scanBlack(vm, x);
    } else {
      x->gcColor = GC_WHITE;
      if (x->type == OBJ_PAIR) {
        workPush(&work, x->head);
        workPush(&work, x->tail);
      }
    }
  }

  /* Phase 3 — collect the whites: self-sustaining islands. Blacken
     on entry so island cycles cannot revisit, and DEFER the frees to
     a drain list: freeing during traversal would leave later objects
     reading their already-freed siblings' fields — Bacon's own shape
     is collect-then-free. The counts are meaningless now; free
     outright, no release cascade. */
  int freed = 0;
  Worklist toFree = {NULL, 0, 0};
  for (int i = 0; i < n; i++) workPush(&work, candidates[i]);
  while (work.count > 0) {
    Object* x = workPop(&work);
    if (x->gcColor != GC_WHITE) continue;
    x->gcColor = GC_BLACK;
    if (x->type == OBJ_PAIR) {
      workPush(&work, x->head);
      workPush(&work, x->tail);
    }
    workPush(&toFree, x);
  }
  while (toFree.count > 0) {
    Object* x = workPop(&toFree);
    free(x);
    vm->liveObjects--;
    freed++;
  }
  free(toFree.items);

  free(work.items);
  free(candidates);
  if (freed > 0) {
    vm->cyclesCollected++;
    vm->cycleFreed += freed;
  }
  printf("Cycle GC: freed %d objects.\n", freed);
}

Object* newObject(VM* vm, ObjectType type) {
  Object* object = malloc(sizeof(Object));
  object->type = type;
  object->refCount = 0;   /* the caller's push/store retains */
  object->gcColor = GC_WHITE;
  object->buffered = 0;
  vm->liveObjects++;
  return object;
}

void pushInt(VM* vm, int intValue) {
  Object* object = newObject(vm, OBJ_INT);
  object->value = intValue;

  push(vm, object);
}

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
  /* The transfer reads the top two slots directly (retain before pop
     — the pop releases, and without the retain the last reference
     could die mid-transfer; net count change per value: zero), so the
     underflow guard gc0 got from pop() lives here now. */
  check(vm->stackSize >= 2, "Stack underflow!");
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

void objectPrint(Object* object) {
  switch (object->type) {
    case OBJ_INT:
      printf("%d", object->value);
      break;

    case OBJ_PAIR:
      /* NULL fields are legitimate here (a broken cycle leaves one),
         and printing one must not dereference it. */
      printf("(");
      if (object->head) objectPrint(object->head); else printf("null");
      printf(", ");
      if (object->tail) objectPrint(object->tail); else printf("null");
      printf(")");
      break;
  }
}

void freeVM(VM *vm) {
  /* Release the roots, then let the cycle collector clear whatever
     islands remain — teardown leaves nothing behind. */
  while (vm->stackSize > 0) pop(vm);
  collectCycles(vm);
  free(vm->cycleRoots);
  free(vm);
}

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

  /* Each cycle node's count is held by the other's tail — and the
     cycle pins its ENTIRE closure: both pairs and their head ints
     stay live (4 objects; the replaced tail ints freed correctly).
     This is naive RC's fatal flaw, asserted on purpose — the
     automatic fix (periodic cycle tracing) is gc10's chapter. */
  check(vm->liveObjects == 4, "Cycles leak their whole closure.");

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
    /* Transfer to a field, in the safe order: STORE before POP. The
       store's retain-new step protects the value across the pop's
       release, and the store's release-old frees the previous tail —
       live count stays flat at 3 with no manual retain. */
    pushInt(vm, i);
    objectSetTail(vm, p, vm->stack[vm->stackSize - 1]);
    pop(vm);
    check(vm->liveObjects == 3, "Overwrites must not leak or over-free.");
  }

  check(p->tail->value == 999, "The final overwrite should stick.");
  freeVM(vm);
}

void test7() {
  printf("Test 7: The cycle collector clears the pinned leak.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);
  Object* a = pushPair(vm);
  pushInt(vm, 3);
  pushInt(vm, 4);
  Object* b = pushPair(vm);

  objectSetTail(vm, a, b);
  objectSetTail(vm, b, a);
  pop(vm);
  pop(vm);

  /* gc9's t5 pinned this exact state at liveObjects == 4. The
     collector now clears it automatically. */
  check(vm->liveObjects == 4, "The leak should still be there first.");
  collectCycles(vm);
  check(vm->liveObjects == 0, "The island should be collected.");
  check(vm->cyclesCollected == 1, "One collection counted.");
  check(vm->cycleFreed == 4, "All four island objects freed.");
  freeVM(vm);
}

void test8() {
  printf("Test 8: An externally referenced island must survive.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);
  Object* a = pushPair(vm);
  pushInt(vm, 3);
  pushInt(vm, 4);
  Object* b = pushPair(vm);

  objectSetTail(vm, a, b);
  objectSetTail(vm, b, a);
  pop(vm);
  pop(vm);
  push(vm, a);   /* one external reference into the island */

  collectCycles(vm);
  /* Trial deletion zeroed the island's internal counts, but a's
     count keeps the external +1: scan_black restores the whole
     island. The NO_SCAN_BLACK build miskills exactly here. */
  check(vm->liveObjects == 4, "Should have preserved the island.");
  check(vm->cycleFreed == 0, "Nothing should have been freed.");

  /* Drop the external reference and collect again — this time the
     restored counts must leave the island fully collectable. */
  pop(vm);
  collectCycles(vm);
  check(vm->liveObjects == 0, "The island should now be collected.");
  check(vm->cycleFreed == 4, "Scan-black's restoration was exact.");
  check(vm->cyclesCollected == 1, "Only the second run collected.");
  freeVM(vm);
}

void test9() {
  printf("Test 9: Nested cycles and a pendant split correctly.\n");
  VM* vm = newVM();
  pushInt(vm, 11);
  pushInt(vm, 12);
  Object* x = pushPair(vm);
  pushInt(vm, 21);
  pushInt(vm, 22);
  Object* y = pushPair(vm);
  pushInt(vm, 3);
  pushInt(vm, 4);
  Object* z = pushPair(vm);

  /* x = (y, z): cycle x<->y via x.head/y.tail, pendant z via x.tail.
     The 11/12 ints die in the stores, leaving a 7-object island. */
  objectSetHead(vm, x, y);
  objectSetTail(vm, x, z);
  objectSetTail(vm, y, x);

  /* Drop x and y, keep z rooted. The stack is [x, y, z], so take z
     off first, drop the other two, then re-root z (the pop-then-push
     idiom — net count change for z is zero). */
  Object* zRef = pop(vm);
  pop(vm);   /* drop y */
  pop(vm);   /* drop x */
  push(vm, zRef);

  collectCycles(vm);
  /* z is externally referenced: scan_black saves z, 3, 4 — only the
     cycle proper is collected. Note it is THREE objects, not four:
     the 22 int already died in the objectSetTail(y, x) store, so
     the island is x, y, and y's head int. */
  check(vm->liveObjects == 3, "The pendant should survive alone.");
  check(vm->cycleFreed == 3, "Only the cycle proper should be freed.");

  pop(vm);
  check(vm->liveObjects == 0, "The survivor should die normally.");
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
  printf("%d objects live, %d refcount ops.\n",
         vm->liveObjects, vm->countOps);
  check(vm->liveObjects == 0, "Churn should leave nothing live.");

  /* Cycle churn: a hundred leaked islands, then one collection.
     Each island is 4 objects (a, b, and the two head ints — the tail
     ints die in the stores). */
  for (int i = 0; i < 100; i++) {
    pushInt(vm, 1);
    pushInt(vm, 2);
    pushPair(vm);
    pushInt(vm, 3);
    pushInt(vm, 4);
    pushPair(vm);
    Object* a = vm->stack[vm->stackSize - 2];
    Object* b = vm->stack[vm->stackSize - 1];
    objectSetTail(vm, a, b);
    objectSetTail(vm, b, a);
    pop(vm);
    pop(vm);
  }
  check(vm->liveObjects == 400, "A hundred islands should be leaked.");
  collectCycles(vm);
  check(vm->cycleFreed == 400, "One pass should clear them all.");
  check(vm->liveObjects == 0, "Churn should leave nothing live.");
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
  test8();
  test9();
  perfTest();

  return 0;
}
