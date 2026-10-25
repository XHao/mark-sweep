/* gc6 — The Go-style hybrid write barrier (simulated concurrent marking).
 *
 * gc2 marked incrementally under a Yuasa deletion barrier. Go 1.8
 * ships a HYBRID barrier — deletion AND insertion halves combined —
 * and the reason is goroutine stacks: they are not write-barriered.
 * Under a pure insertion barrier (pre-1.8 Go), an unbarriered stack
 * store means every stack must be re-scanned at the end of marking,
 * under a stop-the-world pause. The hybrid barrier trades a little
 * floating garbage for eliminating that re-scan.
 *
 * Mapping to the hybrid barrier's pseudo-code in Go proposal 17503
 * ("eliminate STW stack re-scanning", the design that landed in 1.8):
 *
 *   shade(*slot);                       // deletion half (Yuasa)
 *   if current stack is grey:           // insertion half (Dijkstra)
 *     shade(ptr);
 *   *slot = ptr;
 *
 * Go conditions the insertion half on the writing goroutine's stack
 * color; this toy has no per-mutator stack color, so objectSetHead/
 * Tail below use the classic Dijkstra destination form instead: shade
 * the NEW value when the OWNER object is black.
 *
 * In this toy the "stack" is the VM stack. gcStart pre-grays every
 * root — the equivalent of snapshotting all stacks at GC start — so
 * this model never needs a re-scan and the hybrid behaves exactly
 * like pure Yuasa on every gc2 test. The NEW observable is pushRoot:
 * an unbarriered root write, mid-marking, of a white object whose
 * last field path is then deleted. The insertion-only variant
 * (-DBARRIER_DIJKSTRA, the pre-1.8 shape) loses that already-rooted
 * object; the deletion half saves it. That is the Go 1.8 story in
 * miniature.
 *
 * Everything else — stepped interleaving, pop-shading, black
 * allocation — is gc2, unchanged.
 */
#include <stdio.h>
#include <stdlib.h>

#define STACK_MAX 256
#define INIT_OBJ_NUM_MAX 8

typedef enum {
  OBJ_INT,
  OBJ_PAIR
} ObjectType;

typedef enum {
  COLOR_WHITE,  /* Not reached this cycle. */
  COLOR_GRAY,   /* Reached, children not yet scanned. */
  COLOR_BLACK   /* Reached and fully scanned. */
} Color;

typedef enum {
  GC_IDLE,      /* No collection in progress. */
  GC_MARKING    /* Incremental marking in progress. */
} GcState;

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

typedef struct {
  Object* stack[STACK_MAX];
  int stackSize;

  /* The first object in the linked list of all objects on the heap. */
  Object* firstObject;

  /* The total number of currently allocated objects. */
  int numObjects;

  /* The number of objects required to trigger a GC. */
  int maxObjects;

  /* Incremental GC state. */
  GcState gcState;
  Object** grayStack;
  int grayCount;
  int grayCapacity;
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
  vm->firstObject = NULL;
  vm->numObjects = 0;
  vm->maxObjects = INIT_OBJ_NUM_MAX;
  vm->gcState = GC_IDLE;
  vm->grayStack = NULL;
  vm->grayCount = 0;
  vm->grayCapacity = 0;
  return vm;
}


void push(VM* vm, Object* value) {
  check(vm->stackSize < STACK_MAX, "Stack overflow!");
  vm->stack[vm->stackSize++] = value;
}


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

Object* pop(VM* vm) {
  check(vm->stackSize > 0, "Stack underflow!");

  Object* popped = vm->stack[--vm->stackSize];

  /* Dropping a root deletes a snapshot path: shade the popped object so
     whatever the mutator does with it stays covered by the snapshot.
     Barriers only matter while a cycle is marking. */
  if (vm->gcState == GC_MARKING) shade(vm, popped);

  return popped;
}

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

/* Writes into a just-born (white or black) object are safe without
   the barrier: the popped roots were already shaded by pop(). */

/* Barriered field writes — the only sanctioned way to mutate a pair's
   fields. (A real VM's compiler inserts these; we do it by hand.)
   A field write is a mutator step: like an allocation, it drives one
   step of marking. Order matters: shade the old value BEFORE the
   assignment, step AFTER it. */
void objectSetHead(VM* vm, Object* pair, Object* value) {
  /* Both halves only matter while a cycle is marking — shading at
     idle would seed the gray stack and corrupt the next cycle (the
     gc2 lesson). */
  if (vm->gcState == GC_MARKING) {
#ifndef BARRIER_DIJKSTRA
    /* Deletion half (Yuasa): overwriting a field deletes a snapshot
       path; shade the OLD value. */
    shade(vm, pair->head);
    /* Insertion half (Dijkstra): a black object gaining a white
       reference breaks tricolor; shade the NEW value. */
    if (pair->color == COLOR_BLACK) shade(vm, value);
#else
    /* Insertion-only variant: the pre-1.8 Go barrier shape. */
    shade(vm, value);
#endif
  }
  pair->head = value;
  if (vm->gcState == GC_MARKING) gcStep(vm);
}

void objectSetTail(VM* vm, Object* pair, Object* value) {
  if (vm->gcState == GC_MARKING) {
#ifndef BARRIER_DIJKSTRA
    shade(vm, pair->tail);
    if (pair->color == COLOR_BLACK) shade(vm, value);
#else
    shade(vm, value);
#endif
  }
  pair->tail = value;
  if (vm->gcState == GC_MARKING) gcStep(vm);
}

/* Simulate an UNBARRIERED stack write, like a goroutine stack store.
   Deliberately shades nothing and drives no GC step — that is the
   point: the barrier must tolerate this. */
void pushRoot(VM* vm, Object* object) {
  push(vm, object);
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
  vm->stackSize = 0;
  gc(vm);
  free(vm->grayStack);
  free(vm);
}

void test1() {
  printf("Test 1: Objects on stack are preserved.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);

  gc(vm);
  check(vm->numObjects == 2, "Should have preserved objects.");
  freeVM(vm);
}

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
     Field writes go through the barrier API from now on. */
  objectSetTail(vm, a, b);
  objectSetTail(vm, b, a);

  gc(vm);
  check(vm->numObjects == 4, "Should have collected objects.");
  freeVM(vm);
}

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

void test6() {
  printf("Test 6: Marking interleaves with the mutator, nothing is lost.\n");
  VM* vm = newVM();

  /* Push past the initial threshold (INIT_OBJ_NUM_MAX == 8): the 9th
     allocation starts a cycle instead of running one to completion. */
  for (int i = 0; i < 12; i++) {
    pushInt(vm, i);
  }
  check(vm->gcState == GC_MARKING, "Marking should be in progress.");

  /* Rule 2: popping a root during marking keeps it covered by the
     snapshot. In this toy the popped object is already gray or black —
     roots are grayed at start and fresh allocations are born black —
     so the pop-shade never actually fires; it guards the general case
     a real collector faces (roots not pre-grayed). */
  Object* popped = pop(vm);
  check(vm->gcState == GC_MARKING, "Still marking after the pop.");
  check(popped != NULL, "Popped object should exist.");

  /* Rule 3: objects born during marking are born BLACK. */
  pushInt(vm, 99);
  Object* fresh = vm->stack[vm->stackSize - 1];
  check(fresh->color == COLOR_BLACK, "Fresh object should be born black.");

  gcDrive(vm);
  check(vm->gcState == GC_IDLE, "Marking should have finished.");
  check(vm->numObjects == 13, "No object should be lost: 12 + fresh.");

  freeVM(vm);
}

void test7() {
  printf("Test 7: The barrier saves an object deleted then re-added.\n");
  VM* vm = newVM();

  /* The victim is reachable ONLY through q->tail — not via a root.
     pushPair pops the top of the stack into tail, so push the victim
     LAST: q = (2, victim). */
  pushInt(vm, 2);
  pushInt(vm, 42);
  Object* q = pushPair(vm);
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

void test8() {
  printf("Test 8: An unbarriered root write survives the hybrid barrier.\n");
  VM* vm = newVM();

  pushInt(vm, 1);
  pushInt(vm, 42);              /* victim */
  Object* g = pushPair(vm);     /* g = (1, victim) */
  Object* victim = g->tail;

  gcStart(vm);                  /* roots [g] gray */
  pushRoot(vm, victim);         /* unbarriered stack write: victim is
                                   now a root, still WHITE */
  objectSetTail(vm, g, g->head);/* delete victim's last field path */
  gcDrive(vm);

  /* Hybrid: the deletion half shaded victim, so the newly-rooted
     object survived — 3 objects. The insertion-only build shaded the
     NEW value instead; victim stayed white and was swept — 2 objects.
     Count-only assertion: never deref possibly-freed memory. */
  check(vm->numObjects == 3, "The barrier must save the newly-rooted object.");

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
  freeVM(vm);
}

int main(int argc, const char * argv[]) {
  test1();
  test2();
  test3();
  test4();
  test5();
  test6();
#ifndef BARRIER_DIJKSTRA
  /* t7's deletion case is a Yuasa win by design — under the
     insertion-only variant it fails exactly like test8 does. Skip it
     there so this build's failure lands on test8, the demo this
     stage exists for. */
  test7();
#endif
  test8();
  perfTest();

  return 0;
}
