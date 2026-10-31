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
#include <stdio.h>
#include <stdlib.h>

#define STACK_MAX 256

typedef enum {
  OBJ_INT,
  OBJ_PAIR
} ObjectType;

typedef struct sObject {
  ObjectType type;
  int refCount;

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
  return vm;
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

Object* newObject(VM* vm, ObjectType type) {
  Object* object = malloc(sizeof(Object));
  object->type = type;
  object->refCount = 0;   /* the caller's push/store retains */
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
  }
}

void freeVM(VM *vm) {
  /* No collector to call — releasing the roots is the whole
     teardown. Cycles would leak here; tests break theirs by hand. */
  while (vm->stackSize > 0) pop(vm);
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
  freeVM(vm);
}

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
