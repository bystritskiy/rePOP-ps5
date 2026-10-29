/* reSOT PS5, MIT. Bridge FreeBSD signal state to the runtime's x64 decoder. */
#include "platform/win32_compat.h"
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>
/* PS5 adds 48 bytes after sigset_t. Stock PayloadSDK's ucontext_t omits
 * these; PS5_PayloadSDK platform/context.h documents mc at 64, RIP at 0xe0.
 * Use the public mcontext register layout with an explicit console prefix. */
_Static_assert(offsetof(mcontext_t, mc_rip) == 0xa0, "unexpected mcontext registers");
#define PS5_MCONTEXT_OFFSET 64
extern ptrdiff_t g_xbox_mem_offset;
extern bool apu_hook_handle_mmio(void *, uintptr_t, uint32_t, int);
extern int xbox_OhciOwnsAddress(uint32_t);
extern int xbox_OhciHandleMmio(void *, uint32_t);
extern int xbox_Ac97HandleFault(void *, uintptr_t);
static struct sigaction previous_segv, previous_bus;
static stack_t previous_stack;
static unsigned char signal_stack[65536] __attribute__((aligned(16)));
static int installed;
/* Handled device faults by kind (APU, OHCI, AC97), read by the host's
 * periodic performance line: a polling loop over a trapped register costs a
 * signal per access. */
volatile unsigned long resot_ps5_mmio_faults[3];
#define REGISTERS(X) \
    X(Rax,rax) X(Rbx,rbx) X(Rcx,rcx) X(Rdx,rdx) X(Rsp,rsp) X(Rbp,rbp) \
    X(Rsi,rsi) X(Rdi,rdi) X(R8,r8) X(R9,r9) X(R10,r10) X(R11,r11) \
    X(R12,r12) X(R13,r13) X(R14,r14) X(R15,r15) X(Rip,rip) X(EFlags,rflags)
extern int sceKernelDebugOutText(int, const char *);
static void fault_handler(int, siginfo_t *, void *);
extern const char *resot_ps5_crash_path(void);
static char *put(char *out, const char *text)
{
    while (*text) *out++ = *text++;
    return out;
}
static char *hex(char *out, uint64_t value)
{
    *out++ = '0'; *out++ = 'x';
    for (int shift = 60; shift >= 0; shift -= 4)
        *out++ = "0123456789abcdef"[(value >> shift) & 15];
    return out;
}
/* stderr goes through a pipe that dies with the process, so an unhandled
 * fault goes to klog and, synchronously, to logs/<target>-crash.txt; the
 * address of this function locates the eboot (llvm-nm gives its offset).
 * The 300 ms let the log threads drain the pipe before the process ends.
 * A SYSTEM_XO_VIOLATION never reaches it: the kernel ends the process and
 * prints its own fatal-signal block to klog (observed). */
static void report_fault(int sig, siginfo_t *info, mcontext_t *mc)
{
    static char line[1024];
    char *p = put(line, "[reSOT] [CRASH] signal ");
    p = hex(p, (uint64_t)sig); p = put(p, " code "); p = hex(p, (uint64_t)info->si_code);
    p = put(p, " addr "); p = hex(p, (uint64_t)(uintptr_t)info->si_addr);
    p = put(p, "\n[reSOT] [CRASH] rip "); p = hex(p, (uint64_t)mc->mc_rip);
    p = put(p, " rsp "); p = hex(p, (uint64_t)mc->mc_rsp);
    p = put(p, " err "); p = hex(p, (uint64_t)mc->mc_err);
    p = put(p, " trap "); p = hex(p, (uint64_t)mc->mc_trapno);
    p = put(p, "\n[reSOT] [CRASH] fault_handler "); p = hex(p, (uint64_t)(uintptr_t)fault_handler);
    p = put(p, " guest base "); p = hex(p, (uint64_t)g_xbox_mem_offset);
    p = put(p, "\n");
    *p = 0;
    /* Out first: a bad rsp faults again below, and that ends the process. */
    sceKernelDebugOutText(0, line);
    p = put(p - 1, "\n[reSOT] [CRASH] stack");
    const uint64_t *stack = (const uint64_t *)(uintptr_t)mc->mc_rsp;
    for (int i = 0; i < 16; i++) { p = put(p, i % 4 ? " " : "\n[reSOT] [CRASH]  "); p = hex(p, stack[i]); }
    p = put(p, "\n");
    *p = 0;
    sceKernelDebugOutText(0, strstr(line, "[reSOT] [CRASH] stack"));
    int fd = open(resot_ps5_crash_path(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { (void)write(fd, line, (size_t)(p - line)); close(fd); }
    usleep(300000);
}
static void fault_handler(int sig, siginfo_t *info, void *context)
{
    mcontext_t *mc = (mcontext_t *)((unsigned char *)context + PS5_MCONTEXT_OFFSET);
    RESOT_PS5_CONTEXT regs;
#define LOAD(dst,src) regs.dst = (uint64_t)mc->mc_##src;
    REGISTERS(LOAD)
#undef LOAD
    uintptr_t base = (uintptr_t)g_xbox_mem_offset, fault = (uintptr_t)info->si_addr;
    int handled = 0;
    if (base && fault >= base && fault - base < UINT64_C(0x100000000)) {
        uint32_t va = (uint32_t)(fault - base);
        unsigned kind;
        if (va >= 0xfe800000u && va < 0xfe880000u)
            kind = 0, handled = apu_hook_handle_mmio(&regs, fault, va, (mc->mc_err & 2) != 0);
        else if (xbox_OhciOwnsAddress(va))
            kind = 1, handled = xbox_OhciHandleMmio(&regs, va);
        else kind = 2, handled = xbox_Ac97HandleFault(&regs, fault);
        if (handled) __atomic_fetch_add(&resot_ps5_mmio_faults[kind], 1, __ATOMIC_RELAXED);
    }
    if (handled) {
#define SAVE(dst,src) mc->mc_##src = regs.dst;
        REGISTERS(SAVE)
#undef SAVE
        return;
    }
    report_fault(sig, info, mc);
    /* Re-deliver to the pre-existing disposition; never resume an undecoded
     * instruction or pretend an unknown device read returned zero. */
    sigaction(sig, sig == SIGSEGV ? &previous_segv : &previous_bus, NULL);
    raise(sig);
}
/* eboot code is execute-only on the console, so the decoder reads the bytes
 * at Rip from the code segment of /app0/eboot.bin: the SELF entry of segment
 * 0, stored plain (neither encrypted nor compressed); it matched eboot.elf's
 * text byte for byte. The kernel's module table places it (the segments klog
 * prints, xotext at 0x400000); the linker script is RADV's, without symbols
 * of ours. ps5-native-tool writes the PS4 FSELF magic 0x1D3D154F; fPKG
 * builders (PS5 UltraPack) rewrite it to the PS5 magic 0xEEF51454 and keep
 * the header and segment table as they are (compared on the 01.004.000
 * package). */
typedef struct { uint64_t address; uint32_t size; int32_t prot; } module_segment;
typedef struct { uint64_t size; char name[256]; module_segment segments[4];
                 uint32_t count; uint8_t fingerprint[20]; } module_info;
_Static_assert(sizeof(module_info) == 0x160, "SceKernelModuleInfo is 0x160 bytes");
extern int sceKernelGetExecutableModuleHandle(void);
extern int sceKernelGetModuleInfo(int, module_info *);
static const uint8_t *code_copy;
static uintptr_t code_start, code_end;
const uint8_t *resot_ps5_code_bytes(uint64_t rip)
{
    if (code_copy && rip >= code_start && rip < code_end) return code_copy + (rip - code_start);
    return (const uint8_t *)(uintptr_t)rip;
}
static int load_code_copy(void)
{
    module_info info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    int handle = sceKernelGetExecutableModuleHandle(), rc = sceKernelGetModuleInfo(handle, &info);
    uintptr_t self = (uintptr_t)resot_ps5_code_bytes, start = 0, limit = 0;
    for (uint32_t i = 0; !rc && i < info.count && i < 4; i++)
        if (self >= info.segments[i].address && self - info.segments[i].address < info.segments[i].size)
            start = (uintptr_t)info.segments[i].address, limit = info.segments[i].size;
    int fd = start ? open("/app0/eboot.bin", O_RDONLY) : -1;
    struct { uint32_t magic; uint8_t version, mode, endian, attributes; uint32_t key;
             uint16_t header, meta; uint64_t file; uint16_t entries, flags; uint32_t pad; } header;
    struct { uint64_t properties, offset, file, memory; } entry;
    uint64_t offset = 0, size = 0;
    header.magic = 0;
    if (fd >= 0 && pread(fd, &header, sizeof header, 0) == sizeof header &&
        (header.magic == 0x1d3d154fu || header.magic == 0xeef51454u))
        for (unsigned i = 0; i < header.entries; i++) {
            if (pread(fd, &entry, sizeof entry, 0x20 + 0x20 * (off_t)i) != sizeof entry) break;
            /* bits 20+: segment index; bit 1: encrypted; bit 3: compressed.
             * The module segment is the file size rounded up to 16 KiB. */
            if ((entry.properties >> 20) == 0 && !(entry.properties & 0xa) &&
                entry.file <= limit && limit - entry.file < 0x4000) {
                offset = entry.offset; size = entry.file; break;
            }
        }
    uint8_t *copy = offset ? malloc(size) : NULL;
    size_t done = 0;
    while (copy && done < size) {
        ssize_t n = pread(fd, copy + done, size - done, (off_t)(offset + done));
        if (n <= 0) { free(copy); copy = NULL; }
        else done += (size_t)n;
    }
    if (fd >= 0) close(fd);
    char line[256];
    snprintf(line, sizeof line, "[reSOT] [BOOT] code copy for MMIO decoding: module %#x (%#x), segment %#llx+%#llx, "
             "eboot.bin %s magic %#x, file offset %#llx, %llu bytes: %s\n", (unsigned)handle, (unsigned)rc,
             (unsigned long long)start, (unsigned long long)limit, fd >= 0 ? "opened" : "not opened",
             (unsigned)header.magic, (unsigned long long)offset, (unsigned long long)size, copy ? "loaded" : "FAILED");
    sceKernelDebugOutText(0, line);
    fputs(line + 8, stderr);
    code_start = start; code_end = start + size; code_copy = copy;
    return copy != NULL;
}
int resot_ps5_install_fault_handler(void)
{
    if (installed) return 1;
    if (!code_copy && !load_code_copy()) return 0;
    stack_t stack = {.ss_sp = signal_stack, .ss_size = sizeof(signal_stack)};
    if (sigaltstack(&stack, &previous_stack) != 0) return 0;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = fault_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGSEGV); sigaddset(&action.sa_mask, SIGBUS);
    if (sigaction(SIGSEGV, &action, &previous_segv) != 0) {
        sigaltstack(&previous_stack, NULL); return 0;
    }
    if (sigaction(SIGBUS, &action, &previous_bus) != 0) {
        sigaction(SIGSEGV, &previous_segv, NULL);
        sigaltstack(&previous_stack, NULL); return 0;
    }
    installed = 1;
    return 1;
}
/* Call after guest threads have stopped, on the installation thread. */
void resot_ps5_remove_fault_handler(void)
{
    if (!installed) return;
    sigaction(SIGSEGV, &previous_segv, NULL); sigaction(SIGBUS, &previous_bus, NULL);
    sigaltstack(&previous_stack, NULL); installed = 0;
}
