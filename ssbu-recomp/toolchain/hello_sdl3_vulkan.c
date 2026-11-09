/* Toolchain smoke test: SDL3 video + Vulkan. Prints SDL version/video driver, SDL's required instance extensions, the GPU
 * names and (with a window) the surface present modes. Used by `setup.py --target linux-arm64 --only deps`.
 *   hello_sdl3_vulkan [--no-window]     env: SDL_VIDEO_DRIVER=wayland|x11|offscreen|dummy */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

int main(int argc, char **argv) {
    int want_window = !(argc > 1 && !strcmp(argv[1], "--no-window"));
    printf("SDL %d.%d.%d\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);
    if (!SDL_Init(SDL_INIT_VIDEO)) { printf("SDL_Init(VIDEO) failed: %s\n", SDL_GetError()); return 2; }
    printf("video driver: %s\n", SDL_GetCurrentVideoDriver());
    if (!SDL_Vulkan_LoadLibrary(NULL)) { printf("SDL_Vulkan_LoadLibrary failed: %s\n", SDL_GetError()); return 3; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    Uint32 n = 0;
    char const *const *exts = SDL_Vulkan_GetInstanceExtensions(&n);
    if (!exts) { printf("SDL_Vulkan_GetInstanceExtensions failed: %s\n", SDL_GetError()); return 4; }
    printf("instance extensions required by SDL (%u):", n);
    for (Uint32 i = 0; i < n; i++) printf(" %s", exts[i]);
    printf("\n");
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "ssbu-hello", 1, "none", 1, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai, 0, NULL, n, exts };
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkInstance inst;
    VkResult r = create(&ci, NULL, &inst);
    if (r) { printf("vkCreateInstance failed: %d\n", (int)r); return 5; }
    PFN_vkEnumeratePhysicalDevices enumd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    uint32_t cnt = 0;
    enumd(inst, &cnt, NULL);
    VkPhysicalDevice devs[8];
    if (cnt > 8) cnt = 8;
    enumd(inst, &cnt, devs);
    for (uint32_t i = 0; i < cnt; i++) {
        VkPhysicalDeviceProperties p;
        props(devs[i], &p);
        printf("GPU %u: %s  api %u.%u.%u  driver %#x  type %d\n", i, p.deviceName, VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
               VK_VERSION_PATCH(p.apiVersion), p.driverVersion, (int)p.deviceType);
    }
    if (want_window && cnt) {
        SDL_Window *w = SDL_CreateWindow("ssbu hello", 640, 360, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
        if (!w) { printf("window: SDL_CreateWindow failed: %s\n", SDL_GetError()); }
        else {
            VkSurfaceKHR surf;
            if (!SDL_Vulkan_CreateSurface(w, inst, NULL, &surf)) printf("window: SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
            else {
                PFN_vkGetPhysicalDeviceSurfacePresentModesKHR pm = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)gipa(inst, "vkGetPhysicalDeviceSurfacePresentModesKHR");
                VkPresentModeKHR modes[16]; uint32_t mc = 16;
                if (pm && pm(devs[0], surf, &mc, modes) >= 0) {
                    printf("window: surface ok, present modes (0=immediate 1=mailbox 2=fifo 3=fifo_relaxed):");
                    for (uint32_t i = 0; i < mc; i++) printf(" %d", (int)modes[i]);
                    printf("\n");
                } else printf("window: surface ok, present modes query failed\n");
                ((PFN_vkDestroySurfaceKHR)gipa(inst, "vkDestroySurfaceKHR"))(inst, surf, NULL);
            }
            SDL_DestroyWindow(w);
        }
    }
    ((PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance"))(inst, NULL);
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
    printf("hello: ok\n");
    return 0;
}
