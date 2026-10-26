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
#include <stdio.h>
#include <stdlib.h>

#define STACK_MAX 256
#define INIT_OBJ_NUM_MAX 8
#define HEAP_MAX 8192  /* Same total memory as gc3's two 4096 semispaces;
                          with compaction, 100% of it is usable. */

typedef enum {
  OBJ_INT,
  OBJ_PAIR
} ObjectType;

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

typedef struct {
  Object* stack[STACK_MAX];
  int stackSize;

  /* One contiguous arena. Allocation bumps `freeCount`. */
  Object* heap;
  int freeCount;

  /* The total number of live objects (== freeCount). */
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
  vm->heap = malloc(HEAP_MAX * sizeof(Object));
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
  }
}

void freeVM(VM *vm) {
  vm->stackSize = 0;
  gc(vm);
  free(vm->heap);
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
  enum { CHAINS = 8, CHAIN_DEPTH = 3 };
  const int chains = CHAINS;        /* loop bounds only */
  const int depth = CHAIN_DEPTH;
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
  Object* roots[CHAINS];
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
  /* pushPair builds (previous, fresh): head walks down the chain,
     tail is the freshly pushed int. */
  check(vm->stack[0]->tail->value == 0, "Chain content survives.");

  /* The merged holes are reusable: allocate another 20 objects. */
  for (int i = 0; i < 4 * (2 * depth - 1); i++) {
    pushInt(vm, i);
  }
  check(vm->numObjects == 40, "Holes should have been reused.");

  /* Round two, chains still rooted: the 20 ints die, the chains
     survive a compaction that runs over previous survivors. freeVM
     can never pin this — its gc sees an empty root set. */
  for (int i = 0; i < 4 * (2 * depth - 1); i++) {
    pop(vm);
  }
  gc(vm);
  check(vm->numObjects == 20, "Chains should survive a second round.");
  check(vm->stack[0]->tail->value == 0, "Content across two rounds.");

  /* Round three: two chains die. If survivors' marks ever leak across
     rounds, these chains become un-collectable and this fails. */
  pop(vm);
  pop(vm);
  gc(vm);
  check(vm->numObjects == 10, "Dead chains must not leak across rounds.");
  check(vm->stack[0]->tail->value == 0, "Content across three rounds.");
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
  test7();
  test8();
  perfTest();

  return 0;
}
