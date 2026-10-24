/* gc1 — Iterative marking with an explicit worklist.
 *
 * The baseline's recursive mark() walks the object graph on the C call
 * stack: a deep chain of pairs needs one C stack frame per link and
 * overflows for large graphs. This stage replaces recursion with an
 * explicit, dynamically-grown worklist (a "gray stack" in GC terms).
 *
 * Behavior is identical to gc0 — same tests, same output. The only
 * diff-worthy change is mark()/markAll() plus the new test5.
 *
 * Modern relevance: every production collector marks iteratively;
 * recursion is a non-starter once the heap is a graph you don't control.
 */
#include <stdio.h>
#include <stdlib.h>

#define STACK_MAX 256
#define INIT_OBJ_NUM_MAX 8

typedef enum {
  OBJ_INT,
  OBJ_PAIR
} ObjectType;

typedef struct sObject {
  ObjectType type;
  unsigned char marked;

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

  /* Seed the worklist with the roots. */
  for (int i = 0; i < vm->stackSize; i++) {
    workPush(&work, vm->stack[i]);
  }

  /* Drain it: pop, mark, push the children. Cycles terminate because
     we skip already-marked objects — same trick as the recursive
     version's early return. */
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

void sweep(VM* vm)
{
  Object** object = &vm->firstObject;
  while (*object) {
    if (!(*object)->marked) {
      /* This object wasn't reached, so remove it from the list and free it. */
      Object* unreached = *object;

      *object = unreached->next;
      free(unreached);

      vm->numObjects--;
    } else {
      /* This object was reached, so unmark it (for the next GC) and move on to
       the next. */
      (*object)->marked = 0;
      object = &(*object)->next;
    }
  }
}

void gc(VM* vm) {
  int numObjects = vm->numObjects;

  markAll(vm);
  sweep(vm);

  vm->maxObjects = vm->numObjects == 0 ? INIT_OBJ_NUM_MAX : vm->numObjects * 2;

  printf("Collected %d objects, %d remaining.\n", numObjects - vm->numObjects,
         vm->numObjects);
}

Object* newObject(VM* vm, ObjectType type) {
  if (vm->numObjects == vm->maxObjects) gc(vm);

  Object* object = malloc(sizeof(Object));
  object->type = type;
  object->next = vm->firstObject;
  vm->firstObject = object;
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

  /* Set up a cycle, and also make 2 and 4 unreachable and collectible. */
  a->tail = b;
  b->tail = a;

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
  perfTest();

  return 0;
}
