/* startup.c - minimal Cortex-M4 vector table and reset handler. */
#include <stdint.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
int main(void);

void Reset_Handler(void)
{
    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;           /* .data: flash -> RAM */
    for (dst = &_sbss; dst < &_ebss; ) *dst++ = 0;   /* .bss = 0            */
    main();
    for (;;) { }
}

void Default_Handler(void) { for (;;) { } }

__attribute__((section(".isr_vector"), used))
static void (*const vectors[16])(void) = {
    (void (*)(void))(&_estack), Reset_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    0, 0, 0, 0,
    Default_Handler, Default_Handler, 0, Default_Handler, Default_Handler
};
