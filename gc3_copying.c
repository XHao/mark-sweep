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
#include <stdio.h>
#include <stdlib.h>

#define STACK_MAX 256
#define INIT_OBJ_NUM_MAX 8
#define HEAP_MAX 4096

typedef enum {
  OBJ_INT,
  OBJ_PAIR,
  OBJ_FORWARDED  /* From-space husk: its union holds the new address. */
} ObjectType;

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

void check(int condition, const char* message) {
  if (!condition) {
    printf("%s\n", message);
    exit(1);
  }
}

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


void push(VM* vm, Object* value) {
  check(vm->stackSize < STACK_MAX, "Stack overflow!");
  vm->stack[vm->stackSize++] = value;
}


Object* pop(VM* vm) {
  check(vm->stackSize > 0, "Stack underflow!");
  return vm->stack[--vm->stackSize];
}

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

Object* newObject(VM* vm, ObjectType type) {
  /* From-space full: collect, then insist on room. */
  if (vm->freeCount == HEAP_MAX) gc(vm);
  check(vm->freeCount < HEAP_MAX, "Heap overflow!");

  Object* object = &vm->heap[vm->space][vm->freeCount++];
  object->type = type;

  vm->numObjects++;

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
  vm->stackSize = 0;
  gc(vm);
  free(vm->heap[0]);
  free(vm->heap[1]);
  free(vm);
}

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
  const int depth = 2000;  /* 2*depth-1 must fit one semispace. */

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

  /* Build a chain: each iteration adds two live objects while the VM
     stack stays at ~2 entries. A flat pushInt loop could never test
     the heap — the stack is the only root set, so it would hit
     STACK_MAX (256) long before HEAP_MAX (4096) fills. */
  pushInt(vm, 0);
  for (int i = 1; i < HEAP_MAX / 2; i++) {
    pushInt(vm, i);
    pushPair(vm);
  }

  /* From-space now holds HEAP_MAX - 1 live objects. One more fits,
     exactly filling it. */
  pushInt(vm, -1);

  /* One past the limit: gc runs but the chain is all live, nothing is
     freed, and the capacity check must fire. */
  pushInt(vm, -2);

  printf("ERROR: overflow was not caught.\n");
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
  perfTest();

#ifdef OVERFLOW_TEST
  overflowTest();
#endif

  return 0;
}
