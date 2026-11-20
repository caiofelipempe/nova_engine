// Single translation unit that compiles the Vulkan Memory Allocator implementation.
#include <vulkan/vulkan.h>

#if defined(_MSC_VER)
    #pragma warning(push, 0)
#endif
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>
#if defined(_MSC_VER)
    #pragma warning(pop)
#endif
