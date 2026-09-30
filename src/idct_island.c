#include <stdint.h>

#define IDCT_ISLAND_BYTES 512
#define IDCT_ISLAND_WORDS (IDCT_ISLAND_BYTES / sizeof(uint32_t))

#ifdef MPEG_ISLAND_DEBUG
struct idct_private_stack {
    volatile uint32_t guard;
    uint32_t words[IDCT_ISLAND_WORDS - 1];
} __attribute__((aligned(32)));
struct idct_private_stack mpeg_idct_private_stack
    __attribute__((section(".idct_private_stack"), aligned(32)));
uint32_t *mpeg_idct_private_stack_top =
    mpeg_idct_private_stack.words + (IDCT_ISLAND_WORDS - 1);
volatile uintptr_t mpeg_idct_private_stack_highwater =
    (uintptr_t)(mpeg_idct_private_stack.words + (IDCT_ISLAND_WORDS - 1));
volatile uint32_t mpeg_idct_private_stack_fault;

void mpeg_idct_private_stack_init(void) {
    mpeg_idct_private_stack.guard = 0x1dc751a1u;
}

void mpeg_idct_private_stack_check(void) {
    if (mpeg_idct_private_stack.guard != 0x1dc751a1u)
        mpeg_idct_private_stack_fault = 1;
}
#else
uint32_t mpeg_idct_private_stack[IDCT_ISLAND_WORDS]
    __attribute__((section(".idct_private_stack"), aligned(32)));
uint32_t *mpeg_idct_private_stack_top = mpeg_idct_private_stack + IDCT_ISLAND_WORDS;
#endif
