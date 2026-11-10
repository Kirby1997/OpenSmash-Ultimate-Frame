/* The Vulkan back end, in development (docs/TECHNICAL.md, "A second back end: Vulkan"): for now
 * the check of Vulkan on this PC that display.ini's renderer = vulkan asks for (gpu_renderer.c). */
#pragma once
#include <stddef.h>

/* Whether this PC can run the Vulkan back end: the system's Vulkan loader, an instance, the card
 * display.ini's gpu names (else the fastest), a graphics queue that can present to `window` (the
 * game's HWND; NULL: not checked), and a device made with it, then everything made let go again.
 * 1 if all of it worked, 0 if not; either way `what` says what was found or what failed, and
 * the cards found are logged ("[vulkan] ..." lines). SSBU_VULKAN_LOADER names another loader
 * (a path) to load. */
int vk_probe(void *window, char *what, size_t size);
/* The system's Vulkan loader, loaded once and kept: its vkGetInstanceProcAddr (a
 * PFN_vkGetInstanceProcAddr), or NULL with `why` saying why (gpu_vk.c's back end starts from it). */
void *vk_loader(char *why, size_t size);

#ifndef _WIN32
/* Linux: the game's SDL window as a Vulkan surface (linux/window.c), so that neither the back end nor
 * the check needs X11's or Wayland's Vulkan headers. SDL makes the surface with the loader it loaded
 * for the window (SDL_WINDOW_VULKAN: libvulkan.so.1, or SDL_VULKAN_LIBRARY), the same library as
 * vk_loader's: with renderer = vulkan, linux/window.c sets SDL_VULKAN_LIBRARY to SSBU_VULKAN_LOADER's
 * library before it makes the window.
 *   rt_vk_instance_extensions: the instance extensions a surface on the window needs (VK_KHR_surface
 *     and X11's or Wayland's), up to max names into `names` (SDL's strings, kept); 0 if SDL has none.
 *   rt_vk_create_surface: a surface on the window for `instance` (a VkInstance), into *surface (a
 *     VkSurfaceKHR); 1 if made, 0 (and why on stderr) if not.
 *   rt_vk_drawable_size: the window's inside in pixels (0 x 0 without a window). */
unsigned rt_vk_instance_extensions(const char **names, unsigned max);
int rt_vk_create_surface(void *instance, void *surface);
void rt_vk_drawable_size(unsigned *width, unsigned *height);
#endif
