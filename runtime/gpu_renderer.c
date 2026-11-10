/* Which renderer draws the game: display.ini's renderer (or SSBU_RENDERER), read by gpu_display.c
 * (renderer.h's names), acted on here once, as the renderer starts.
 *
 * The default is the system's own back end: Direct3D 11 (gpu_d3d11.c) on Windows, the same through
 * DXVK on Linux, Metal on macOS. vulkan asks for the Vulkan back end behind gpu_backend.h
 * (gpu_vk.c; docs/TECHNICAL.md, "A second back end: Vulkan"), which is in development, and on
 * Linux experimental (docs/LINUX.md): gpu_backend.c starts it at the first GPU work, and if it
 * cannot start the default renderer draws and the log says why. gfx_renderer_start's check of
 * Vulkan on this PC (gpu_vulkan.c) is for a start that did not try the back end. */
#include <stdio.h>

#include "display.h"
#include "gpu_renderer.h"
#include "gpu_vulkan.h"

#ifdef _WIN32
#define DEFAULT_WORDS "Direct3D 11"
#else
#define DEFAULT_WORDS "DXVK (Direct3D 11 on Vulkan)"
#endif

static int drawing = RENDERER_DEFAULT;
static int tried;              /* (gpu_backend.c's choice tried the Vulkan back end) */
static char failed[600];       /* why it could not start */

int gfx_renderer(void) { return drawing; }

void gfx_renderer_chosen(int renderer, const char *why) {
    drawing = renderer;
    tried = 1;
    snprintf(failed, sizeof failed, "%s", why ? why : "");
#ifdef _WIN32
    if (renderer == RENDERER_VULKAN) fprintf(stderr, "[gfx] renderer = vulkan: the Vulkan back end draws (in development: docs/TECHNICAL.md, \"A second back end: Vulkan\")\n");
#else
    if (renderer == RENDERER_VULKAN)
        fprintf(stderr, "[gfx] renderer = vulkan: the Vulkan back end draws (in development, experimental on Linux: docs/LINUX.md; "
                        "renderer = dxvk in display.ini goes back to DXVK)\n");
#endif
}

int gfx_renderer_start(void *window) {
    if (display_renderer() != RENDERER_VULKAN) return drawing = RENDERER_DEFAULT;
    if (tried) {   /* (the Vulkan back end could not start: Direct3D 11 is starting instead) */
        fprintf(stderr, "[gfx] renderer = vulkan: the Vulkan back end cannot start here: %s. " DEFAULT_WORDS " draws\n", failed);
        return drawing = RENDERER_DEFAULT;
    }
    char what[600];
    ULONGLONG t0 = GetTickCount64();
    int works = vk_probe(window, what, sizeof what);
    double ms = (double)(GetTickCount64() - t0);
    if (works)
        fprintf(stderr, "[gfx] renderer = vulkan: Vulkan works here: %s (checked in %.0f ms). The Vulkan renderer cannot draw the game "
                        "yet (docs/TECHNICAL.md, \"A second back end: Vulkan\"): " DEFAULT_WORDS " draws\n", what, ms);
    else
        fprintf(stderr, "[gfx] renderer = vulkan: Vulkan cannot be used here: %s. " DEFAULT_WORDS " draws\n", what);
    return drawing = RENDERER_DEFAULT;
}
