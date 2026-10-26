/* gc8 — Colored pointers + a load barrier: the ZGC ideas.
 *
 * gc3's pause rewrote EVERY pointer before the mutator could run
 * again. ZGC flips that: objects move FIRST, pointers are healed
 * LATER — by the load barrier, on every reference read, while the
 * mutator keeps running. This stage carries the three signature
 * mechanisms in miniature:
 *
 *   1. Colored pointers. Heap-field references are stored with a
 *      view color in bit 0 (heap addresses are 8-aligned; the bit is
 *      free). Every relocate flips the view — so after a pause, every
 *      copied field is detectably stale by ONE compare, no table
 *      lookup. (Real ZGC colors the high pointer bits: Marked0/1/
 *      Remapped.)
 *   2. The load barrier. loadRef(vm, slot) is the only way to read a
 *      heap field (a real VM's compiler instruments every load).
 *      Stale color OR a relocated target (a husk) heals the slot in
 *      place: follow the forwarding table, rewrite with the fresh
 *      pointer in the CURRENT view, count it. Two checks, not one:
 *      a fresh-colored store of an OLD-SPACE address (a C local held
 *      across a pause — see test5's razor case) passes the color
 *      compare; only the husk check catches it.
 *   3. Lazy relocation. The pause copies the live set, builds the
 *      forwarding table, and heals ONLY the roots (ZGC's stop-the-
 *      world root phase). Heap fields are left stale on purpose.
 *      The old space serves as the forwarding table until the NEXT
 *      pause's heal sweep — ZGC keeps relocation pages alive the
 *      same way, until every pointer into them has healed. Note the
 *      validity window this implies: a C local is healable only
 *      until the next pause — after that, the space it points into
 *      has been reused, and even the husk check reads whatever lives
 *      there now. Re-fetch pointers after every pause, exactly as
 *      gc3's header prescribed.
 *
 * diff gc3_copying.c gc8_zgc.c is the whole lazy-healing delta.
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

  /* Load-barrier state: the current view color and heal statistics. */
  int color;          /* 0 or 1: flips every relocate. */
  int healCount;
  int relocateCount;
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
  vm->color = 0;
  vm->healCount = 0;
  vm->relocateCount = 0;
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

/* Objects are 8-byte aligned in the heap arrays, so bit 0 of a
   reference is free to carry the view color. */
/* Set the bit rather than OR it: a value that already carries a color
   tag is re-based first — double-tagging must not silently fake the
   current view. */
Object* tagColor(int color, Object* object) {
  return (Object*)(((size_t)object & ~(size_t)1) | (size_t)color);
}

Object* untag(Object* pointer) {
  return (Object*)((size_t)pointer & ~(size_t)1);
}

/* The load barrier — every heap-field READ goes through here (a real
   VM's compiler instruments every reference load). Fast path: the
   slot's color is current AND the target has not been relocated.
   Otherwise heal: follow the forwarding table (the husk), rewrite
   the slot with the fresh pointer in the CURRENT view, count it.
   Two checks, not one: a fresh-colored store of an OLD-SPACE address
   passes the color compare — only the husk check catches it. */
Object* loadRef(VM* vm, Object** slot) {
  Object* pointer = *slot;
  Object* object = untag(pointer);

  if (object == NULL) return NULL;

#ifndef LOAD_BARRIER_OFF
  if (((size_t)pointer & 1) != (size_t)vm->color ||
      object->type == OBJ_FORWARDED) {
    if (object->type == OBJ_FORWARDED) object = object->forward;
    *slot = tagColor(vm->color, object);
    vm->healCount++;
  }
#else
  /* The barrier-free build: a raw read that returns husks. Exists so
     the killer test can prove the barrier is load-bearing. */
  (void)vm;
#endif
  return object;
}

/* The store side is deliberately light: stores carry the view color;
   loads do the healing. */
void objectSetHead(VM* vm, Object* pair, Object* value) {
  pair->head = value == NULL ? NULL : tagColor(vm->color, value);
}

void objectSetTail(VM* vm, Object* pair, Object* value) {
  pair->tail = value == NULL ? NULL : tagColor(vm->color, value);
}

/* Heal every heap field: the sweep that finishes the PREVIOUS pause's
   lazy healing. The old space is still intact, so its husks are still
   a valid forwarding table — after this, the space this pause is
   about to vacate can be reused. (ZGC reclaims per page as its
   pointers heal; we sweep at pause start.) */
void healAll(VM* vm) {
  for (int i = 0; i < vm->freeCount; i++) {
    Object* object = &vm->heap[vm->space][i];
    if (object->type == OBJ_PAIR) {
      loadRef(vm, &object->head);
      loadRef(vm, &object->tail);
    }
  }
}

void relocate(VM* vm) {
  healAll(vm);
  vm->relocateCount++;

  vm->toCount = 0;

  /* Stop-the-world root phase: roots are healed eagerly, untagged. */
  for (int i = 0; i < vm->stackSize; i++) {
    vm->stack[i] = copyObject(vm, vm->stack[i]);
  }

  /* Concurrent-relocation analog: scan to-space to copy the reachable
     set — but only TRAVERSE, never rewrite. The fields keep pointing
     into the old space (stale color); loads after the pause heal them
     lazily. Traversal follows the old pointers through the husks. */
  int scan = 0;
  while (scan < vm->toCount) {
    Object* object = &vm->heap[1 - vm->space][scan++];
    if (object->type == OBJ_PAIR) {
      copyObject(vm, untag(object->head));
      copyObject(vm, untag(object->tail));
    }
  }

  vm->space = 1 - vm->space;
  vm->freeCount = vm->toCount;
  vm->numObjects = vm->toCount;

  /* The view flips: every old-color field copy is now detectably
     stale by the color compare alone — the payoff of coloring. */
  vm->color = 1 - vm->color;

  printf("Relocated %d objects.\n", vm->toCount);
}

Object* newObject(VM* vm, ObjectType type) {
  /* From-space full: relocate, then insist on room. */
  if (vm->freeCount == HEAP_MAX) relocate(vm);
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
  Object* tail = pop(vm);
  Object* head = pop(vm);
  objectSetTail(vm, object, tail);
  objectSetHead(vm, object, head);
  push(vm, object);
  return object;
}

void objectPrint(VM* vm, Object* object) {
  switch (object->type) {
    case OBJ_INT:
      printf("%d", object->value);
      break;

    case OBJ_PAIR:
      printf("(");
      objectPrint(vm, loadRef(vm, &object->head));
      printf(", ");
      objectPrint(vm, loadRef(vm, &object->tail));
      printf(")");
      break;

    case OBJ_FORWARDED:
      printf("<forwarded>");
      break;
  }
}

void freeVM(VM *vm) {
  free(vm->heap[0]);
  free(vm->heap[1]);
  free(vm);
}

void test1() {
  printf("Test 1: Objects on stack are preserved.\n");
  VM* vm = newVM();
  pushInt(vm, 1);
  pushInt(vm, 2);

  relocate(vm);
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

  relocate(vm);
  check(vm->numObjects == 0, "Should have collected objects.");
  check(vm->relocateCount == 1, "One pause counted.");
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

  relocate(vm);
  check(vm->numObjects == 7, "Should have reached objects.");

  Object* outer = vm->stack[vm->stackSize - 1];
  Object* inner = loadRef(vm, &outer->tail);
  check(inner->type == OBJ_PAIR, "Inner pair should survive.");
  check(loadRef(vm, &inner->tail)->value == 4, "Leaf values should survive.");
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

  /* Cycle through the barriered stores. */
  objectSetTail(vm, a, b);
  objectSetTail(vm, b, a);

  relocate(vm);
  check(vm->numObjects == 4, "Cycle should survive relocation.");

  /* Re-fetch b', then close the cycle through two heals. */
  Object* pair = vm->stack[vm->stackSize - 1];
  Object* through = loadRef(vm, &pair->tail);       /* a' */
  check(loadRef(vm, &through->tail) == pair, "Cycle should close through healing.");
  freeVM(vm);
}

void test5() {
  printf("Test 5: Colored pointers heal lazily under the load barrier.\n");
  VM* vm = newVM();

  pushInt(vm, 2);
  pushInt(vm, 42);
  Object* q = pushPair(vm);     /* q = (2, 42) */
  Object* victim = untag(q->tail);   /* C-held pre-pause pointer */
  check(vm->healCount == 0, "No heals before the first pause.");

  relocate(vm);

  /* Re-fetch the moved pair; its FIELDS were not rewritten — the
     stored tag still carries the OLD view color. */
  Object* moved = vm->stack[vm->stackSize - 1];
  check(((size_t)moved->tail & 1) != (size_t)vm->color,
        "The field should still be stale-colored.");

  /* The barrier heals in place: pointer refreshed, color current. */
  Object* tail = loadRef(vm, &moved->tail);
  check(tail->value == 42, "Healed pointer reads correctly.");
  check(((size_t)moved->tail & 1) == (size_t)vm->color,
        "Healing should re-color the slot.");
  check(vm->healCount == 1, "One heal counted.");

  /* The razor case: store a C-held OLD-SPACE address. The store takes
     the CURRENT color, so the color compare passes — only the husk
     check can save it. */
  objectSetTail(vm, moved, victim);
  check(vm->healCount == 1, "Stores don't heal.");
  Object* healed = loadRef(vm, &moved->tail);
  check(healed->value == 42, "The husk check must catch fresh-colored stale.");
  check(vm->healCount == 2, "Razor-case heal counted.");
  freeVM(vm);
}

void test6() {
  printf("Test 6: The next pause's sweep finishes lazy heals.\n");
  VM* vm = newVM();

  pushInt(vm, 1);
  pushInt(vm, 2);
  pushPair(vm);
  relocate(vm);                 /* both pair fields now stale-colored */

  /* Do NOT read the fields; allocate until the second pause fires. */
  pushInt(vm, 0);
  for (int i = 0; vm->relocateCount == 1 && i < 10000; i++) {
    pushInt(vm, i);
    pop(vm);
  }
  check(vm->relocateCount == 2, "The fill should trigger the second pause.");

  /* The sweep at pause two healed both stale fields. */
  check(vm->healCount == 2, "The sweep counted both lazy heals.");

  Object* pair = vm->stack[vm->stackSize - 2];
  check(loadRef(vm, &pair->head)->value == 1, "Swept fields read correctly.");
  check(loadRef(vm, &pair->tail)->value == 2, "Swept fields read correctly.");

  /* Pause two's view flip re-staled the survivors' fields (their
     copies still carry the previous color); reading heals lazily
     again — the discipline repeats every pause. */
  check(vm->healCount == 4, "Post-pause reads heal the new staleness.");
  freeVM(vm);
}

void test7() {
  printf("Test 7: Healed slots take the fast path.\n");
  VM* vm = newVM();
  pushInt(vm, 5);
  pushInt(vm, 6);
  pushPair(vm);
  relocate(vm);

  Object* pair = vm->stack[vm->stackSize - 1];
  Object* first = loadRef(vm, &pair->tail);
  check(first->value == 6, "Fast-path loads read correctly.");

  int heals = vm->healCount;
  for (int i = 0; i < 100; i++) {
    check(loadRef(vm, &pair->tail) == first, "Repeated loads are stable.");
  }
  check(vm->healCount == heals, "The fast path never heals.");

  /* NULL is neither colored nor healed — just passed through. */
  objectSetTail(vm, pair, NULL);
  check(loadRef(vm, &pair->tail) == NULL, "NULL stays NULL.");
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
  printf("%d relocations, %d heals.\n", vm->relocateCount, vm->healCount);
  check(vm->relocateCount >= 1, "The workload should relocate.");
  freeVM(vm);
}

int main(int argc, const char * argv[]) {
  test1();
  test2();
#ifndef LOAD_BARRIER_OFF
  /* t3/t4/t6 read fields through the barrier after a pause — with
     the barrier compiled out they return husks and fail, exactly as
     test5's killer does. Skip them so the OFF build's failure lands
     on test5, the designated demonstration. */
  test3();
  test4();
#endif
  test5();
#ifndef LOAD_BARRIER_OFF
  test6();
#endif
  test7();
  perfTest();

  return 0;
}
