/* plat_heap.c - keep the C heap below 2^47 on Linux (nothing on Windows).
 *
 * The guest is handed malloc'd host memory all the time (boot.c i_malloc, _aligned_malloc, the C++ runtime). Windows
 * never gave it a pointer above 2^47; on this kernel (arm64, 48-bit VA) mmap's area is at 0xffff_xxxx_xxxx. The
 * executable is linked -no-pie, so the brk heap of glibc's main arena sits just above it, far below the guest window
 * (0x71_0000_0000). glibc leaves that heap for two things, both turned off here before anything can allocate:
 *   - per-thread arenas (mmap'd 64 MiB heaps, one per contended thread): M_ARENA_MAX 1, every thread uses the main
 *     arena (tcache still makes most small allocations lock-free);
 *   - mmap'd chunks for requests >= M_MMAP_THRESHOLD (128 KiB+): M_MMAP_MAX 0, every chunk comes from brk.
 * posix_memalign / _aligned_malloc / calloc / realloc / operator new all go through the same arena. glibc maps from
 * the main arena only if brk itself fails (the heap would have grown into another mapping: hundreds of GiB here).
 * Not covered: memory libraries map themselves (SDL, the Vulkan driver's buffer maps, thread stacks of threads they
 * create); the guest must never be handed such a pointer (vkMapMemory results are the graphics layer's concern).
 * Cost: one arena lock for all threads on tcache misses, and freed large blocks only return to the system when they
 * are at the top of the heap (M_TRIM_THRESHOLD).
 *
 * Runs from .preinit_array: before every shared library's constructors (a library thread could otherwise take a
 * second arena first), after libc's own early init (mallopt is safe there). Executable only: a shared object may not
 * have .preinit_array. Under a sanitizer the allocator is the sanitizer's and this does nothing useful. */
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <malloc.h>

static void plat_heap_init(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    mallopt(M_ARENA_MAX, 1);
    mallopt(M_MMAP_MAX, 0);
}
__attribute__((section(".preinit_array"), used)) static void (*const plat_heap_preinit)(int, char **, char **) = plat_heap_init;
/* For tests and diagnostics: the same settings, callable (idempotent). */
void plat_heap_setup(void) { plat_heap_init(0, 0, 0); }
#endif
