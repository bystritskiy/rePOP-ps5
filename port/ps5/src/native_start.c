/* reSOT PS5, MIT. Native application entry, SysV loader argument block. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <xmmintrin.h>
typedef void (*ResotInit)(void);
extern void _init_env(void *);
extern ResotInit __init_array_start[], __init_array_end[];
extern ResotInit __fini_array_start[], __fini_array_end[];
extern int main(int, char **);
void _init(void) { for(ResotInit *p=__init_array_start;p<__init_array_end;p++)(*p)(); }
void _fini(void) { for(ResotInit *p=__fini_array_end;p>__fini_array_start;)(*--p)(); }
__attribute__((noreturn)) void _start(void *arguments, ResotInit teardown)
{
    _mm_setcsr(0x1f80); __asm__ volatile("fninit");
    _init_env(arguments);
    if(teardown)atexit(teardown);
    atexit(_fini);_init();
    exit(main(*(int *)arguments,(char **)((uint8_t *)arguments+8)));
}
