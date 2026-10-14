/*
 * VK_LAYER_DROIDDECK_venus: what the session needs from a Vulkan device that Venus over vtest, on
 * the phone's own driver (Mali, PowerVR), does not give it. Loaded implicitly in Venus sessions
 * (VenusBridge); every call it does not name goes straight to the next layer or the driver.
 *
 * gamescope
 *   - A render node for the device (VK_EXT_physical_device_drm): gamescope offers linux-dmabuf only
 *     with one, and its Wayland backend refuses a device without a primary node. The app binds the
 *     phone's GPU device in as /dev/dri/renderD* (LinuxRuntime.bindGpuNode), as KGSL is on Adreno,
 *     and passes its numbers as DROIDDECK_VENUS_DRM=<major>:<minor>.
 *   - VK_KHR_external_semaphore_fd, which Venus offers only over virtio-gpu. gamescope requires it
 *     for DRM syncobj timelines, which the stand-in node cannot back on any GPU here (KGSL's
 *     included): the extension is advertised and its two calls fail softly.
 *
 * DXVK (Direct3D 9-11) refuses a device without six features Mali lacks. Each is advertised and
 * kept from the driver:
 *   - textureCompressionBC: BC images are created in an uncompressed format, and an upload into one
 *     is redirected to a staging buffer the layer owns, which it fills at submit time by decoding
 *     the application's blocks on the CPU (bcdec). The copy stays where the application recorded
 *     it, so its barriers and layout transitions keep their meaning.
 *   - fillModeNonSolid: wireframe draws fill.
 *   - multiViewport: viewport and scissor counts are clamped to one.
 *   - shaderClipDistance, shaderCullDistance: the builtins become private variables in every
 *     shader (Mali's compiler crashed on them); geometry is not clipped by them.
 *   - robustBufferAccess2: the driver's robustBufferAccess still bounds-checks.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include "etc2_encode.h"

#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#include "third_party/bcdec.h"

#define EXPORT __attribute__((visibility("default")))
#define LAYER_NAME "VK_LAYER_DROIDDECK_venus"
#define MAX_OBJECTS 64
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void *key_of(const void *handle) { return *(void *const *)handle; }

/* ---- a u64 -> pointer map (open addressing), for handles ---- */

struct map {
   uint64_t *keys; /* 0 empty, UINT64_MAX deleted */
   void **values;
   uint32_t cap, used;
};

static uint64_t hash64(uint64_t k)
{
   k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ull; k ^= k >> 33;
   return k;
}

static void *map_get(struct map *m, uint64_t key)
{
   if (!m->cap || !key) return NULL;
   for (uint32_t i = hash64(key) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
      if (m->keys[i] == key) return m->values[i];
      if (m->keys[i] == 0) return NULL;
   }
}

static void map_put(struct map *m, uint64_t key, void *value);

static void map_grow(struct map *m)
{
   struct map old = *m;
   m->cap = old.cap ? old.cap * 2 : 64;
   m->used = 0;
   m->keys = calloc(m->cap, sizeof(*m->keys));
   m->values = calloc(m->cap, sizeof(*m->values));
   for (uint32_t i = 0; i < old.cap; i++)
      if (old.keys[i] && old.keys[i] != UINT64_MAX) map_put(m, old.keys[i], old.values[i]);
   free(old.keys);
   free(old.values);
}

static void map_put(struct map *m, uint64_t key, void *value)
{
   if (!key) return;
   if ((m->used + 1) * 4 >= m->cap * 3) map_grow(m);
   uint32_t slot = UINT32_MAX;
   for (uint32_t i = hash64(key) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
      if (m->keys[i] == key) { m->values[i] = value; return; }
      if (m->keys[i] == UINT64_MAX && slot == UINT32_MAX) slot = i;
      if (m->keys[i] == 0) {
         if (slot == UINT32_MAX) { slot = i; m->used++; }
         m->keys[slot] = key;
         m->values[slot] = value;
         return;
      }
   }
}

static void *map_take(struct map *m, uint64_t key)
{
   if (!m->cap || !key) return NULL;
   for (uint32_t i = hash64(key) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
      if (m->keys[i] == key) {
         void *v = m->values[i];
         m->keys[i] = UINT64_MAX;
         m->values[i] = NULL;
         return v;
      }
      if (m->keys[i] == 0) return NULL;
   }
}

#define H(x) ((uint64_t)(uintptr_t)(x))

/* ---- instance and device tables ---- */

struct instance_data {
   void *key;
   VkInstance instance;
   PFN_vkGetInstanceProcAddr next_gipa;
   PFN_vkDestroyInstance destroy;
   PFN_vkEnumerateDeviceExtensionProperties enum_exts;
   PFN_vkGetPhysicalDeviceProperties2 props2;
   PFN_vkGetPhysicalDeviceFeatures features;
   PFN_vkGetPhysicalDeviceFeatures2 features2;
   PFN_vkGetPhysicalDeviceFormatProperties format_props;
   PFN_vkGetPhysicalDeviceFormatProperties2 format_props2;
   PFN_vkGetPhysicalDeviceImageFormatProperties image_format_props;
   PFN_vkGetPhysicalDeviceImageFormatProperties2 image_format_props2;
   PFN_vkGetPhysicalDeviceMemoryProperties memory_props;
};

/* Features the driver lacks and this layer stands in for. */
enum {
   FAKE_BC = 1 << 0,
   FAKE_FILL_MODE = 1 << 1,
   FAKE_MULTI_VIEWPORT = 1 << 2,
   FAKE_CLIP_DISTANCE = 1 << 3,
   FAKE_CULL_DISTANCE = 1 << 4,
   FAKE_ROBUSTNESS2 = 1 << 5,
};

struct buffer_binding { VkDeviceMemory memory; VkDeviceSize offset; };
struct mapping { void *base; bool ours; };

struct device_data {
   void *key;
   VkDevice device;
   VkPhysicalDevice physical;
   uint32_t fake;
   bool pack_bc1; /* BC1 (UNORM) images stored as 16-bit 565 / 1555 */
   int fake_semaphore_fd;
   VkPhysicalDeviceMemoryProperties memory;
   PFN_vkGetDeviceProcAddr next_gdpa;
   PFN_vkDestroyDevice destroy;
   PFN_vkCreateSemaphore create_semaphore;
   PFN_vkCreateImage create_image;
   PFN_vkCreateSwapchainKHR create_swapchain;
   PFN_vkDestroyImage destroy_image;
   PFN_vkCreateImageView create_image_view;
   PFN_vkCreateBuffer create_buffer;
   PFN_vkDestroyBuffer destroy_buffer;
   PFN_vkGetBufferMemoryRequirements buffer_reqs;
   PFN_vkAllocateMemory allocate_memory;
   PFN_vkFreeMemory free_memory;
   PFN_vkBindBufferMemory bind_buffer_memory;
   PFN_vkBindBufferMemory2 bind_buffer_memory2;
   PFN_vkMapMemory map_memory;
   PFN_vkMapMemory2 map_memory2;
   PFN_vkUnmapMemory unmap_memory;
   PFN_vkUnmapMemory2 unmap_memory2;
   PFN_vkCmdCopyBufferToImage copy_buffer_to_image;
   PFN_vkCmdCopyBufferToImage2 copy_buffer_to_image2;
   PFN_vkCmdSetViewport set_viewport;
   PFN_vkCmdSetScissor set_scissor;
   PFN_vkCmdSetViewportWithCount set_viewport_with_count;
   PFN_vkCmdSetScissorWithCount set_scissor_with_count;
   PFN_vkCmdSetPolygonModeEXT set_polygon_mode;
   PFN_vkCreateGraphicsPipelines create_graphics_pipelines;
   PFN_vkCreateShaderModule create_shader_module;
   PFN_vkGetImageMemoryRequirements image_reqs;
   PFN_vkGetImageMemoryRequirements2 image_reqs2;
   PFN_vkGetDeviceImageMemoryRequirements device_image_reqs;
   uint32_t host_visible_types; /* memory types the server backs with AHardwareBuffers */
   /* DROIDDECK_VENUS_MEMORY_LOG=1: bytes allocated per memory type, and image placements */
   bool memory_log;
   struct map allocations; /* VkDeviceMemory -> size << 5 | memory type */
   int64_t type_bytes[32];
   uint32_t images_private, images_left;
   PFN_vkAllocateCommandBuffers allocate_command_buffers;
   PFN_vkFreeCommandBuffers free_command_buffers;
   PFN_vkResetCommandPool reset_command_pool;
   PFN_vkDestroyCommandPool destroy_command_pool;
   PFN_vkBeginCommandBuffer begin_command_buffer;
   PFN_vkResetCommandBuffer reset_command_buffer;
   PFN_vkQueueSubmit queue_submit;
   PFN_vkQueueSubmit2 queue_submit2;
   /* handle -> state, under lock */
   struct map images;    /* VkImage -> (uintptr_t) original BC format */
   struct map shared_images; /* VkImage -> 1: linear, external or DRM-modifier images */
   struct map buffers;   /* VkBuffer -> struct buffer_binding* */
   struct map mappings;  /* VkDeviceMemory -> struct mapping* (application's maps) */
   struct map cmd_pools; /* VkCommandBuffer -> VkCommandPool */
   struct map decodes;   /* VkCommandBuffer -> struct decode* list */
};

static struct instance_data instances[MAX_OBJECTS];
static struct device_data *devices[MAX_OBJECTS];
static struct map physical_fake; /* VkPhysicalDevice -> (uintptr_t)(fake mask + 1) */

static struct instance_data *find_instance(void *key)
{
   struct instance_data *found = NULL;
   pthread_mutex_lock(&lock);
   for (int i = 0; i < MAX_OBJECTS; i++)
      if (instances[i].key == key) { found = &instances[i]; break; }
   pthread_mutex_unlock(&lock);
   return found;
}

static struct device_data *find_device(void *key)
{
   struct device_data *found = NULL;
   pthread_mutex_lock(&lock);
   for (int i = 0; i < MAX_OBJECTS; i++)
      if (devices[i] && devices[i]->key == key) { found = devices[i]; break; }
   pthread_mutex_unlock(&lock);
   return found;
}

static bool env_off(const char *name)
{
   const char *v = getenv(name);
   return v && !strcmp(v, "1");
}

/* ---- BC formats ---- */

static bool is_bc(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }

/* ETC2 / EAC hold the BC formats' data in the same bits per texel and Mali samples them natively;
 * BC textures are transcoded to them unless the device lacks them (DROIDDECK_VENUS_NO_ETC2=1
 * forces the uncompressed substitutes). Set with the first physical device's feature query. */
static int etc2_enabled;

/* The uncompressed substitute: what a decoded block's texels are stored as. */
static VkFormat raw_substitute(VkFormat f)
{
   switch (f) {
   case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
   case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC7_UNORM_BLOCK:
      return VK_FORMAT_R8G8B8A8_UNORM;
   case VK_FORMAT_BC1_RGB_SRGB_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
   case VK_FORMAT_BC2_SRGB_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK:
      return VK_FORMAT_R8G8B8A8_SRGB;
   case VK_FORMAT_BC4_UNORM_BLOCK: return VK_FORMAT_R8_UNORM;
   case VK_FORMAT_BC4_SNORM_BLOCK: return VK_FORMAT_R8_SNORM;
   case VK_FORMAT_BC5_UNORM_BLOCK: return VK_FORMAT_R8G8_UNORM;
   case VK_FORMAT_BC5_SNORM_BLOCK: return VK_FORMAT_R8G8_SNORM;
   case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
   default: return f;
   }
}

/* The ETC2 / EAC substitute, or the uncompressed one where none fits (signed BC4/BC5, BC6H's
 * HDR). Direct3D's BC1 always carries an alpha bit, so BC1 with alpha takes ETC2's RGBA8. */
static VkFormat etc_substitute(VkFormat f)
{
   switch (f) {
   case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
   case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK;
   case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK:
   case VK_FORMAT_BC7_UNORM_BLOCK:
      return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
   case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK:
   case VK_FORMAT_BC7_SRGB_BLOCK:
      return VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK;
   case VK_FORMAT_BC4_UNORM_BLOCK: return VK_FORMAT_EAC_R11_UNORM_BLOCK;
   case VK_FORMAT_BC5_UNORM_BLOCK: return VK_FORMAT_EAC_R11G11_UNORM_BLOCK;
   default: return raw_substitute(f);
   }
}

static VkFormat bc_substitute(VkFormat f)
{
   return etc2_enabled ? etc_substitute(f) : raw_substitute(f);
}

/* Bytes per 4x4 block of an ETC2 / EAC substitute; 0 for an uncompressed one. */
static uint32_t etc_block_bytes(VkFormat sub)
{
   switch (sub) {
   case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
   case VK_FORMAT_EAC_R11_UNORM_BLOCK:
      return 8;
   case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK: case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
   case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
      return 16;
   default: return 0;
   }
}

static uint32_t bc_block_bytes(VkFormat f)
{
   switch (f) {
   case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
   case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
   case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK:
      return 8;
   default: return 16;
   }
}

/* How an image's BC format is stored, kept in high bits of its dev->images entry: the default
 * substitute (bc_substitute), 16-bit (PACKED_BIT) or uncompressed (RAW_BIT: 3D images, which
 * ETC2 cannot hold). */
#define PACKED_BIT 0x80000000u
#define RAW_BIT 0x40000000u
#define KIND_BITS (PACKED_BIT | RAW_BIT)

/* BC1 holds 565 endpoints, so a 16-bit substitute halves its memory for one bit of interpolation
 * precision. sRGB has no 16-bit format and stays RGBA8. */
static VkFormat packed_substitute(VkFormat f)
{
   switch (f) {
   case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return VK_FORMAT_R5G6B5_UNORM_PACK16;
   case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
   default: return VK_FORMAT_UNDEFINED;
   }
}

static VkFormat image_substitute(VkFormat orig, uint32_t kind)
{
   if (kind & PACKED_BIT) return packed_substitute(orig);
   if (kind & RAW_BIT) return raw_substitute(orig);
   return bc_substitute(orig);
}

static uint32_t texel_bytes(VkFormat substitute)
{
   switch (substitute) {
   case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16: return 2;
   case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: return 1;
   case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: return 2;
   case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
   default: return 4;
   }
}

/* Decodes one 4x4 block into dst (pitch in bytes) in the substitute format's layout. */
static void decode_block(VkFormat f, const uint8_t *src, uint8_t *dst, uint32_t pitch)
{
   switch (f) {
   case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
      bcdec_bc1(src, dst, (int)pitch);
      for (int y = 0; y < 4; y++)
         for (int x = 0; x < 4; x++) dst[y * pitch + x * 4 + 3] = 0xff;
      break;
   case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
      bcdec_bc1(src, dst, (int)pitch); break;
   case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK: bcdec_bc2(src, dst, (int)pitch); break;
   case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK: bcdec_bc3(src, dst, (int)pitch); break;
   case VK_FORMAT_BC4_UNORM_BLOCK: bcdec_bc4(src, dst, (int)pitch, 0); break;
   case VK_FORMAT_BC4_SNORM_BLOCK: bcdec_bc4(src, dst, (int)pitch, 1); break;
   case VK_FORMAT_BC5_UNORM_BLOCK: bcdec_bc5(src, dst, (int)pitch, 0); break;
   case VK_FORMAT_BC5_SNORM_BLOCK: bcdec_bc5(src, dst, (int)pitch, 1); break;
   case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK: {
      uint16_t rgb[16 * 3];
      bcdec_bc6h_half(src, rgb, 4 * 3, f == VK_FORMAT_BC6H_SFLOAT_BLOCK);
      for (int y = 0; y < 4; y++) {
         uint16_t *row = (uint16_t *)(dst + y * pitch);
         for (int x = 0; x < 4; x++) {
            row[x * 4 + 0] = rgb[(y * 4 + x) * 3 + 0];
            row[x * 4 + 1] = rgb[(y * 4 + x) * 3 + 1];
            row[x * 4 + 2] = rgb[(y * 4 + x) * 3 + 2];
            row[x * 4 + 3] = 0x3c00; /* 1.0 */
         }
      }
      break;
   }
   case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK: bcdec_bc7(src, dst, (int)pitch); break;
   default: break;
   }
}

static void decode_block_packed(VkFormat f, const uint8_t *src, uint8_t *dst, uint32_t pitch)
{
   uint8_t rgba[4 * 4 * 4];
   bcdec_bc1(src, rgba, 4 * 4);
   for (int y = 0; y < 4; y++) {
      uint16_t *row = (uint16_t *)(dst + y * pitch);
      for (int x = 0; x < 4; x++) {
         const uint8_t *c = rgba + (y * 4 + x) * 4;
         const uint32_t r5 = (c[0] * 31u + 127) / 255, g6 = (c[1] * 63u + 127) / 255,
                        g5 = (c[1] * 31u + 127) / 255, b5 = (c[2] * 31u + 127) / 255;
         row[x] = f == VK_FORMAT_BC1_RGB_UNORM_BLOCK ? (uint16_t)(r5 << 11 | g6 << 5 | b5)
                                                     : (uint16_t)((c[3] >= 128) << 15 | r5 << 10 | g5 << 5 | b5);
      }
   }
}

/* Transcodes one BC block into its ETC2 / EAC substitute's block. */
static void transcode_block(VkFormat f, VkFormat sub, const uint8_t *src, uint8_t *dst)
{
   uint8_t px[4 * 4 * 4];
   switch (f) {
   case VK_FORMAT_BC4_UNORM_BLOCK:
      bcdec_bc4(src, px, 4, 0);
      eac_encode_r11(px, 1, dst);
      return;
   case VK_FORMAT_BC5_UNORM_BLOCK:
      bcdec_bc5(src, px, 8, 0);
      eac_encode_r11(px, 2, dst);
      eac_encode_r11(px + 1, 2, dst + 8);
      return;
   default:
      decode_block(f, src, px, 16);
      if (sub == VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK || sub == VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK)
         etc2_encode_rgb(px, dst);
      else
         etc2_encode_rgba(px, dst);
      return;
   }
}

/* ---- pending decodes: one per redirected upload, filled at submit ---- */

struct decode {
   struct decode *next;
   VkFormat format;
   uint32_t kind;
   VkBuffer src;
   VkDeviceSize src_offset;
   uint32_t row_blocks, height_blocks; /* the application's buffer layout */
   uint32_t blocks_w, blocks_h, slices; /* slices: depth * layers */
   VkBuffer staging;
   VkDeviceMemory staging_memory;
   uint8_t *staging_ptr;
   VkDeviceSize staging_size;
};

/* Bytes of decode staging alive across all devices; logged each time it crosses a 128 MiB step,
 * with the high-water mark (DROIDDECK_VENUS_STAGING_LOG=1). */
static int64_t staging_live, staging_peak;

static void account_staging(int64_t delta)
{
   static int enabled = -1;
   if (enabled < 0) enabled = env_off("DROIDDECK_VENUS_STAGING_LOG");
   const int64_t before = __atomic_fetch_add(&staging_live, delta, __ATOMIC_RELAXED);
   const int64_t after = before + delta;
   int64_t peak = __atomic_load_n(&staging_peak, __ATOMIC_RELAXED);
   while (after > peak && !__atomic_compare_exchange_n(&staging_peak, &peak, after, false,
                                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
   const int64_t step = 128ll << 20;
   if (enabled && before / step != after / step)
      fprintf(stderr, LAYER_NAME ": decode staging %lld MiB (peak %lld MiB)\n",
              (long long)(after >> 20), (long long)(__atomic_load_n(&staging_peak, __ATOMIC_RELAXED) >> 20));
}

static void free_decodes(struct device_data *dev, struct decode *d)
{
   while (d) {
      struct decode *next = d->next;
      if (d->staging) dev->destroy_buffer(dev->device, d->staging, NULL);
      if (d->staging_memory) {
         dev->free_memory(dev->device, d->staging_memory, NULL);
         account_staging(-(int64_t)d->staging_size);
      }
      free(d);
      d = next;
   }
}

static void release_command_buffer(struct device_data *dev, VkCommandBuffer cb)
{
   pthread_mutex_lock(&lock);
   struct decode *d = map_take(&dev->decodes, H(cb));
   pthread_mutex_unlock(&lock);
   free_decodes(dev, d);
}

static uint32_t find_memory_type(struct device_data *dev, uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < dev->memory.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (dev->memory.memoryTypes[i].propertyFlags & want) == want) return i;
   return UINT32_MAX;
}

/* A host-visible buffer of `size` bytes, mapped; false when the device cannot make one. */
static bool create_staging(struct device_data *dev, VkDeviceSize size, struct decode *d)
{
   const VkBufferCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   if (dev->create_buffer(dev->device, &info, NULL, &d->staging) != VK_SUCCESS) return false;
   VkMemoryRequirements reqs;
   dev->buffer_reqs(dev->device, d->staging, &reqs);
   uint32_t type = find_memory_type(dev, reqs.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (type == UINT32_MAX) return false;
   const VkMemoryAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = reqs.size,
      .memoryTypeIndex = type,
   };
   if (dev->allocate_memory(dev->device, &alloc, NULL, &d->staging_memory) != VK_SUCCESS) return false;
   d->staging_size = reqs.size;
   account_staging((int64_t)reqs.size);
   if (dev->bind_buffer_memory(dev->device, d->staging, d->staging_memory, 0) != VK_SUCCESS) return false;
   void *ptr;
   if (dev->map_memory(dev->device, d->staging_memory, 0, VK_WHOLE_SIZE, 0, &ptr) != VK_SUCCESS) return false;
   d->staging_ptr = ptr;
   return true;
}

/* One decode's work at submit: its source bytes and the substitute it is written in. Rows of
 * blocks are the unit handed to the decoding threads. */
struct decode_job {
   struct decode *d;
   const uint8_t *src;
   VkDeviceMemory temp; /* mapped here for the job, unmapped after */
   VkDeviceMemory memory; /* the source's memory, and where it is mapped */
   const uint8_t *base;
   VkFormat sub;
   uint32_t first_row, rows; /* in the submit's numbering of block rows */
};

struct decode_work {
   struct decode_job *jobs;
   uint32_t job_count, total_rows;
   uint32_t next_row; /* atomic: the next row to take */
};

static void decode_row(const struct decode_job *job, uint32_t row)
{
   const struct decode *d = job->d;
   const uint32_t s = row / d->blocks_h, by = row % d->blocks_h;
   const uint32_t bb = bc_block_bytes(d->format);
   const uint8_t *src = job->src + ((VkDeviceSize)s * d->height_blocks + by) * d->row_blocks * bb;
   const uint32_t eb = etc_block_bytes(job->sub);
   if (eb) {
      uint8_t *dst = d->staging_ptr + ((VkDeviceSize)s * d->blocks_h + by) * d->blocks_w * eb;
      for (uint32_t bx = 0; bx < d->blocks_w; bx++)
         transcode_block(d->format, job->sub, src + (VkDeviceSize)bx * bb, dst + (VkDeviceSize)bx * eb);
      return;
   }
   const uint32_t tb = texel_bytes(job->sub);
   const uint32_t pitch = d->blocks_w * 4 * tb;
   uint8_t *dst = d->staging_ptr + (VkDeviceSize)s * d->blocks_h * 4 * pitch + (VkDeviceSize)by * 4 * pitch;
   for (uint32_t bx = 0; bx < d->blocks_w; bx++) {
      if (d->kind & PACKED_BIT) decode_block_packed(d->format, src + (VkDeviceSize)bx * bb, dst + bx * 4 * tb, pitch);
      else decode_block(d->format, src + (VkDeviceSize)bx * bb, dst + bx * 4 * tb, pitch);
   }
}

static void *decode_worker(void *arg)
{
   struct decode_work *w = arg;
   uint32_t job = 0;
   for (;;) {
      const uint32_t row = __atomic_fetch_add(&w->next_row, 4, __ATOMIC_RELAXED);
      if (row >= w->total_rows) break;
      const uint32_t end = row + 4 < w->total_rows ? row + 4 : w->total_rows;
      for (uint32_t r = row; r < end; r++) {
         while (r >= w->jobs[job].first_row + w->jobs[job].rows) job++;
         while (r < w->jobs[job].first_row) job--;
         decode_row(&w->jobs[job], r - w->jobs[job].first_row);
      }
   }
   return NULL;
}

/* Fills the staging of every upload recorded in cb, on up to four threads: transcoding to ETC2
 * costs about a microsecond a block, and a level's textures run to tens of millions of blocks. */
static void run_decodes(struct device_data *dev, VkCommandBuffer cb)
{
   /* DROIDDECK_VENUS_NO_BC_DECODE=1: textures stay undecoded (a diagnostic switch). */
   static int no_decode = -1;
   if (no_decode < 0) no_decode = env_off("DROIDDECK_VENUS_NO_BC_DECODE");
   if (no_decode) return;
   pthread_mutex_lock(&lock);
   struct decode *list = map_get(&dev->decodes, H(cb));
   pthread_mutex_unlock(&lock);
   uint32_t count = 0;
   for (struct decode *d = list; d; d = d->next) count++;
   if (!count) return;
   struct decode_job *jobs = calloc(count, sizeof(*jobs));
   if (!jobs) return;
   struct decode_work work = { .jobs = jobs };
   for (struct decode *d = list; d; d = d->next) {
      struct decode_job *job = &jobs[work.job_count];
      /* The source's bytes: a mapping the application holds, one an earlier job made, or a new
       * one (memory cannot be mapped twice). */
      pthread_mutex_lock(&lock);
      struct buffer_binding *bind = map_get(&dev->buffers, H(d->src));
      struct mapping *m = bind ? map_get(&dev->mappings, H(bind->memory)) : NULL;
      job->memory = bind ? bind->memory : VK_NULL_HANDLE;
      job->base = m ? m->base : NULL;
      const VkDeviceSize offset = bind ? bind->offset : 0;
      pthread_mutex_unlock(&lock);
      for (uint32_t k = 0; k < work.job_count && job->memory && !job->base; k++)
         if (jobs[k].memory == job->memory) job->base = jobs[k].base;
      if (job->memory && !job->base) {
         void *ptr;
         if (dev->map_memory(dev->device, job->memory, 0, VK_WHOLE_SIZE, 0, &ptr) == VK_SUCCESS) {
            job->temp = job->memory;
            job->base = ptr;
         }
      }
      const uint8_t *src = job->base ? job->base + offset : NULL;
      if (!src) {
         static bool warned;
         if (!warned) fprintf(stderr, LAYER_NAME ": BC upload from memory the host cannot read; left undecoded\n");
         warned = true;
         continue;
      }
      job->d = d;
      job->src = src + d->src_offset;
      job->sub = image_substitute(d->format, d->kind);
      job->first_row = work.total_rows;
      job->rows = d->slices * d->blocks_h;
      work.total_rows += job->rows;
      work.job_count++;
   }
   pthread_t threads[3];
   int started = 0;
   const uint64_t blocks = (uint64_t)work.total_rows * (list ? list->blocks_w : 0);
   if (work.total_rows >= 64 && blocks >= 16384)
      for (; started < 3; started++)
         if (pthread_create(&threads[started], NULL, decode_worker, &work)) break;
   decode_worker(&work);
   for (int t = 0; t < started; t++) pthread_join(threads[t], NULL);
   for (uint32_t k = 0; k < work.job_count; k++)
      if (jobs[k].temp) dev->unmap_memory(dev->device, jobs[k].temp);
   free(jobs);
}

/* ---- DROIDDECK_VENUS_SEGV_TRACE=1: a backtrace for a fault, then the handler that was there ---- */

static struct sigaction previous_segv, previous_abrt;

static void segv_trace(int sig, siginfo_t *info, void *context)
{
   /* Wine and FEX take faults on purpose (guard pages, self-modifying code): trace the first few,
    * and every fault near address zero, which they do not take. */
   static int traced, traced_null;
   const struct sigaction *previous = sig == SIGABRT ? &previous_abrt : &previous_segv;
   const bool near_null = (uintptr_t)info->si_addr < 0x10000;
   if (sig != SIGABRT && __atomic_fetch_add(near_null ? &traced_null : &traced, 1, __ATOMIC_RELAXED) >= 16)
      goto chain;
   void *frames[48];
   const int n = backtrace(frames, 48);
   fprintf(stderr, LAYER_NAME ": signal %d at %p, %d frames\n", sig, info->si_addr, n);
   const ucontext_t *uc = context;
   const uintptr_t at[2] = { uc->uc_mcontext.pc, uc->uc_mcontext.regs[30] };
   for (int i = 0; i < 2; i++) {
      Dl_info dl;
      if (dladdr((void *)at[i], &dl) && dl.dli_fname)
         fprintf(stderr, "  %s %p %s+0x%lx (%s)\n", i ? "lr" : "pc", (void *)at[i], dl.dli_fname,
                 (unsigned long)(at[i] - (uintptr_t)dl.dli_fbase), dl.dli_sname ? dl.dli_sname : "?");
      else
         fprintf(stderr, "  %s %p\n", i ? "lr" : "pc", (void *)at[i]);
   }
   for (int i = 0; i < 31; i += 4)
      fprintf(stderr, "  x%-2d %016llx %016llx %016llx %016llx\n", i,
              (unsigned long long)uc->uc_mcontext.regs[i], (unsigned long long)uc->uc_mcontext.regs[i + 1 < 31 ? i + 1 : 30],
              (unsigned long long)uc->uc_mcontext.regs[i + 2 < 31 ? i + 2 : 30], (unsigned long long)uc->uc_mcontext.regs[i + 3 < 31 ? i + 3 : 30]);
   for (int i = 0; i < n; i++) {
      Dl_info dl;
      if (dladdr(frames[i], &dl) && dl.dli_fname)
         fprintf(stderr, "  #%d %p %s+0x%lx (%s)\n", i, frames[i], dl.dli_fname,
                 (unsigned long)((uintptr_t)frames[i] - (uintptr_t)dl.dli_fbase), dl.dli_sname ? dl.dli_sname : "?");
      else
         fprintf(stderr, "  #%d %p\n", i, frames[i]);
   }
chain:
   if (previous->sa_flags & SA_SIGINFO)
      previous->sa_sigaction(sig, info, context);
   else if (previous->sa_handler != SIG_DFL && previous->sa_handler != SIG_IGN)
      previous->sa_handler(sig);
}

static void install_segv_trace(void)
{
   static bool installed;
   if (installed || !env_off("DROIDDECK_VENUS_SEGV_TRACE")) return;
   installed = true;
   struct sigaction sa = { .sa_sigaction = segv_trace, .sa_flags = SA_SIGINFO | SA_ONSTACK };
   sigemptyset(&sa.sa_mask);
   sigaction(SIGSEGV, &sa, &previous_segv);
   sigaction(SIGABRT, &sa, &previous_abrt);
}

/* ---- render node and features (instance level) ---- */

static int node_numbers(uint32_t *major, uint32_t *minor)
{
   const char *node = getenv("DROIDDECK_VENUS_DRM");
   char *end;
   if (!node || !*node) return 0;
   unsigned long a = strtoul(node, &end, 10);
   if (*end != ':') return 0;
   unsigned long b = strtoul(end + 1, &end, 10);
   if (*end) return 0;
   *major = (uint32_t)a;
   *minor = (uint32_t)b;
   return 1;
}

static void name_node(VkPhysicalDeviceProperties2 *props)
{
   uint32_t major, minor;
   if (!node_numbers(&major, &minor)) return;
   for (VkBaseOutStructure *s = (VkBaseOutStructure *)props->pNext; s; s = s->pNext) {
      if (s->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT) continue;
      VkPhysicalDeviceDrmPropertiesEXT *drm = (VkPhysicalDeviceDrmPropertiesEXT *)s;
      if (drm->hasRender) return;
      drm->hasPrimary = VK_TRUE;
      drm->primaryMajor = major;
      drm->primaryMinor = minor;
      drm->hasRender = VK_TRUE;
      drm->renderMajor = major;
      drm->renderMinor = minor;
   }
}

static VKAPI_ATTR void VKAPI_CALL
layer_GetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *props)
{
   struct instance_data *in = find_instance(key_of(pd));
   if (!in || !in->props2) return;
   in->props2(pd, props);
   name_node(props);
}

/* What the driver itself lacks of the features the layer stands in for. */
static uint32_t driver_missing(struct instance_data *in, VkPhysicalDevice pd)
{
   pthread_mutex_lock(&lock);
   uintptr_t cached = (uintptr_t)map_get(&physical_fake, H(pd));
   pthread_mutex_unlock(&lock);
   if (cached) return (uint32_t)(cached - 1);
   if (env_off("DROIDDECK_VENUS_NO_FAKE")) return 0;

   VkPhysicalDeviceRobustness2FeaturesEXT rob = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
   VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &rob };
   in->features2(pd, &f2);
   const VkPhysicalDeviceFeatures *f = &f2.features;
   uint32_t missing = 0;
   if (!f->textureCompressionBC) missing |= FAKE_BC;
   if (f->textureCompressionETC2 && !env_off("DROIDDECK_VENUS_NO_ETC2")) etc2_enabled = 1;
   if (!f->fillModeNonSolid) missing |= FAKE_FILL_MODE;
   if (!f->multiViewport) missing |= FAKE_MULTI_VIEWPORT;
   if (!f->shaderClipDistance) missing |= FAKE_CLIP_DISTANCE;
   if (!f->shaderCullDistance) missing |= FAKE_CULL_DISTANCE;
   if (!rob.robustBufferAccess2 && rob.nullDescriptor) missing |= FAKE_ROBUSTNESS2;

   pthread_mutex_lock(&lock);
   map_put(&physical_fake, H(pd), (void *)(uintptr_t)(missing + 1));
   pthread_mutex_unlock(&lock);
   return missing;
}

static void set_core_features(VkPhysicalDeviceFeatures *f, uint32_t fake, VkBool32 value)
{
   if (fake & FAKE_BC) f->textureCompressionBC = value;
   if (fake & FAKE_FILL_MODE) f->fillModeNonSolid = value;
   if (fake & FAKE_MULTI_VIEWPORT) f->multiViewport = value;
   if (fake & FAKE_CLIP_DISTANCE) f->shaderClipDistance = value;
   if (fake & FAKE_CULL_DISTANCE) f->shaderCullDistance = value;
}

static void set_chain_features(void *chain, uint32_t fake, VkBool32 value)
{
   for (VkBaseOutStructure *s = chain; s; s = s->pNext) {
      if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT && (fake & FAKE_ROBUSTNESS2))
         ((VkPhysicalDeviceRobustness2FeaturesEXT *)s)->robustBufferAccess2 = value;
   }
}

static VKAPI_ATTR void VKAPI_CALL
layer_GetPhysicalDeviceFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f)
{
   struct instance_data *in = find_instance(key_of(pd));
   if (!in) return;
   in->features(pd, f);
   set_core_features(f, driver_missing(in, pd), VK_TRUE);
}

static VKAPI_ATTR void VKAPI_CALL
layer_GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
{
   struct instance_data *in = find_instance(key_of(pd));
   if (!in) return;
   in->features2(pd, f);
   const uint32_t fake = driver_missing(in, pd);
   set_core_features(&f->features, fake, VK_TRUE);
   set_chain_features(f->pNext, fake, VK_TRUE);
}

/* Format features a substituted BC format offers: what sampling and uploads need. */
#define BC_FEATURES (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | \
                     VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | \
                     VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT)

static VKAPI_ATTR void VKAPI_CALL
layer_GetPhysicalDeviceFormatProperties(VkPhysicalDevice pd, VkFormat format, VkFormatProperties *p)
{
   struct instance_data *in = find_instance(key_of(pd));
   if (!in) return;
   const bool sub = is_bc(format) && (driver_missing(in, pd) & FAKE_BC);
   in->format_props(pd, sub ? bc_substitute(format) : format, p);
   if (sub) {
      p->optimalTilingFeatures &= BC_FEATURES;
      p->linearTilingFeatures = 0;
      p->bufferFeatures = 0;
   }
}

static VKAPI_ATTR void VKAPI_CALL
layer_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice pd, VkFormat format, VkFormatProperties2 *p)
{
   struct instance_data *in = find_instance(key_of(pd));
   if (!in) return;
   const bool sub = is_bc(format) && (driver_missing(in, pd) & FAKE_BC);
   in->format_props2(pd, sub ? bc_substitute(format) : format, p);
   if (!sub) return;
   p->formatProperties.optimalTilingFeatures &= BC_FEATURES;
   p->formatProperties.linearTilingFeatures = 0;
   p->formatProperties.bufferFeatures = 0;
   for (VkBaseOutStructure *s = p->pNext; s; s = s->pNext) {
      if (s->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
         VkFormatProperties3 *p3 = (VkFormatProperties3 *)s;
         p3->optimalTilingFeatures &= (VkFormatFeatureFlags2)BC_FEATURES;
         p3->linearTilingFeatures = 0;
         p3->bufferFeatures = 0;
      } else if (s->sType == VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT) {
         ((VkDrmFormatModifierPropertiesListEXT *)s)->drmFormatModifierCount = 0;
      } else if (s->sType == VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT) {
         ((VkDrmFormatModifierPropertiesList2EXT *)s)->drmFormatModifierCount = 0;
      }
   }
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_GetPhysicalDeviceImageFormatProperties(VkPhysicalDevice pd, VkFormat format, VkImageType type,
                                             VkImageTiling tiling, VkImageUsageFlags usage,
                                             VkImageCreateFlags flags, VkImageFormatProperties *p)
{
   struct instance_data *in = find_instance(key_of(pd));
   if (!in) return VK_ERROR_INITIALIZATION_FAILED;
   if (is_bc(format) && (driver_missing(in, pd) & FAKE_BC)) {
      if (tiling != VK_IMAGE_TILING_OPTIMAL) return VK_ERROR_FORMAT_NOT_SUPPORTED;
      format = image_substitute(format, etc2_enabled && type == VK_IMAGE_TYPE_3D ? RAW_BIT : 0);
      flags &= ~VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
   }
   return in->image_format_props(pd, format, type, tiling, usage, flags, p);
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *info,
                                              VkImageFormatProperties2 *p)
{
   struct instance_data *in = find_instance(key_of(pd));
   if (!in) return VK_ERROR_INITIALIZATION_FAILED;
   if (!is_bc(info->format) || !(driver_missing(in, pd) & FAKE_BC))
      return in->image_format_props2(pd, info, p);
   if (info->tiling != VK_IMAGE_TILING_OPTIMAL) return VK_ERROR_FORMAT_NOT_SUPPORTED;
   VkPhysicalDeviceImageFormatInfo2 copy = *info;
   copy.format = image_substitute(info->format, etc2_enabled && info->type == VK_IMAGE_TYPE_3D ? RAW_BIT : 0);
   copy.flags &= ~VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
   /* A format list naming BC formats would not match the substitute. */
   VkBaseOutStructure *prev = (VkBaseOutStructure *)&copy, *list = NULL;
   for (VkBaseOutStructure *s = prev->pNext; s; prev = s, s = s->pNext)
      if (s->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) { list = s; break; }
   if (list) prev->pNext = list->pNext;
   VkResult r = in->image_format_props2(pd, &copy, p);
   if (list) prev->pNext = list;
   return r;
}

/* ---- instance and device creation ---- */

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *alloc, VkInstance *out)
{
   VkLayerInstanceCreateInfo *link = (VkLayerInstanceCreateInfo *)info->pNext;
   while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
      link = (VkLayerInstanceCreateInfo *)link->pNext;
   if (!link) return VK_ERROR_INITIALIZATION_FAILED;
   PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   link->u.pLayerInfo = link->u.pLayerInfo->pNext;
   PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
   if (!create) return VK_ERROR_INITIALIZATION_FAILED;
   VkResult result = create(info, alloc, out);
   if (result != VK_SUCCESS) return result;

#define GET(name) gipa(*out, name)
   struct instance_data in = {
      .key = key_of(*out),
      .instance = *out,
      .next_gipa = gipa,
      .destroy = (PFN_vkDestroyInstance)GET("vkDestroyInstance"),
      .enum_exts = (PFN_vkEnumerateDeviceExtensionProperties)GET("vkEnumerateDeviceExtensionProperties"),
      .props2 = (PFN_vkGetPhysicalDeviceProperties2)GET("vkGetPhysicalDeviceProperties2"),
      .features = (PFN_vkGetPhysicalDeviceFeatures)GET("vkGetPhysicalDeviceFeatures"),
      .features2 = (PFN_vkGetPhysicalDeviceFeatures2)GET("vkGetPhysicalDeviceFeatures2"),
      .format_props = (PFN_vkGetPhysicalDeviceFormatProperties)GET("vkGetPhysicalDeviceFormatProperties"),
      .format_props2 = (PFN_vkGetPhysicalDeviceFormatProperties2)GET("vkGetPhysicalDeviceFormatProperties2"),
      .image_format_props = (PFN_vkGetPhysicalDeviceImageFormatProperties)GET("vkGetPhysicalDeviceImageFormatProperties"),
      .image_format_props2 = (PFN_vkGetPhysicalDeviceImageFormatProperties2)GET("vkGetPhysicalDeviceImageFormatProperties2"),
      .memory_props = (PFN_vkGetPhysicalDeviceMemoryProperties)GET("vkGetPhysicalDeviceMemoryProperties"),
   };
#undef GET
   /* Venus is 1.1+, so the "2" entry points exist; prefer them for the "1" ones too. */
   if (!in.props2) in.props2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(*out, "vkGetPhysicalDeviceProperties2KHR");
   if (!in.features2) in.features2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(*out, "vkGetPhysicalDeviceFeatures2KHR");
   if (!in.format_props2) in.format_props2 = (PFN_vkGetPhysicalDeviceFormatProperties2)gipa(*out, "vkGetPhysicalDeviceFormatProperties2KHR");
   if (!in.image_format_props2) in.image_format_props2 = (PFN_vkGetPhysicalDeviceImageFormatProperties2)gipa(*out, "vkGetPhysicalDeviceImageFormatProperties2KHR");

   pthread_mutex_lock(&lock);
   for (int i = 0; i < MAX_OBJECTS; i++) {
      if (instances[i].key) continue;
      instances[i] = in;
      break;
   }
   pthread_mutex_unlock(&lock);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
layer_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc)
{
   if (!instance) return;
   struct instance_data *in = find_instance(key_of(instance));
   if (!in) return;
   PFN_vkDestroyInstance destroy = in->destroy;
   pthread_mutex_lock(&lock);
   memset(in, 0, sizeof(*in));
   pthread_mutex_unlock(&lock);
   if (destroy) destroy(instance, alloc);
}

static const VkExtensionProperties semaphore_fd_ext = {
   VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_SPEC_VERSION,
};

static int driver_has_semaphore_fd(struct instance_data *in, VkPhysicalDevice pd)
{
   uint32_t count = 0;
   if (!in->enum_exts || in->enum_exts(pd, NULL, &count, NULL) != VK_SUCCESS) return 1;
   VkExtensionProperties *props = calloc(count ? count : 1, sizeof(*props));
   if (!props) return 1;
   int found = 0;
   if (in->enum_exts(pd, NULL, &count, props) >= VK_SUCCESS)
      for (uint32_t i = 0; i < count; i++)
         if (!strcmp(props[i].extensionName, semaphore_fd_ext.extensionName)) found = 1;
   free(props);
   return found;
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_EnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *layer, uint32_t *count, VkExtensionProperties *props)
{
   if (layer && !strcmp(layer, LAYER_NAME)) {
      *count = 0;
      return VK_SUCCESS;
   }
   struct instance_data *in = find_instance(key_of(pd));
   if (!in || !in->enum_exts) return VK_ERROR_INITIALIZATION_FAILED;
   if (layer || driver_has_semaphore_fd(in, pd)) return in->enum_exts(pd, layer, count, props);

   uint32_t inner = 0;
   VkResult result = in->enum_exts(pd, NULL, &inner, NULL);
   if (result != VK_SUCCESS) return result;
   if (!props) {
      *count = inner + 1;
      return VK_SUCCESS;
   }
   VkExtensionProperties *all = calloc(inner + 1, sizeof(*all));
   if (!all) return VK_ERROR_OUT_OF_HOST_MEMORY;
   result = in->enum_exts(pd, NULL, &inner, all);
   if (result < VK_SUCCESS) { free(all); return result; }
   all[inner++] = semaphore_fd_ext;
   uint32_t n = *count < inner ? *count : inner;
   memcpy(props, all, n * sizeof(*props));
   free(all);
   *count = n;
   return n < inner ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *info, const VkAllocationCallbacks *alloc, VkDevice *out)
{
   VkLayerDeviceCreateInfo *link = (VkLayerDeviceCreateInfo *)info->pNext;
   while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
      link = (VkLayerDeviceCreateInfo *)link->pNext;
   struct instance_data *in = find_instance(key_of(pd));
   if (!link || !in) return VK_ERROR_INITIALIZATION_FAILED;
   PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
   link->u.pLayerInfo = link->u.pLayerInfo->pNext;
   PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(in->instance, "vkCreateDevice");
   if (!create) return VK_ERROR_INITIALIZATION_FAILED;

   /* Keep what the layer stands in for from the driver, which would refuse it; the application's
    * structs are restored after the call. */
   const int fake_sem = !driver_has_semaphore_fd(in, pd);
   const uint32_t fake = driver_missing(in, pd);
   VkDeviceCreateInfo copy = *info;
   const char **names = NULL;
   if (fake_sem && info->enabledExtensionCount) {
      names = calloc(info->enabledExtensionCount, sizeof(*names));
      if (!names) return VK_ERROR_OUT_OF_HOST_MEMORY;
      copy.enabledExtensionCount = 0;
      for (uint32_t i = 0; i < info->enabledExtensionCount; i++)
         if (strcmp(info->ppEnabledExtensionNames[i], semaphore_fd_ext.extensionName))
            names[copy.enabledExtensionCount++] = info->ppEnabledExtensionNames[i];
      copy.ppEnabledExtensionNames = names;
   }
   VkPhysicalDeviceFeatures saved_core = { 0 }, *core = (VkPhysicalDeviceFeatures *)info->pEnabledFeatures;
   VkPhysicalDeviceFeatures2 *f2 = NULL;
   VkPhysicalDeviceRobustness2FeaturesEXT *rob = NULL;
   for (VkBaseOutStructure *s = (VkBaseOutStructure *)info->pNext; s; s = s->pNext) {
      if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) f2 = (VkPhysicalDeviceFeatures2 *)s;
      if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) rob = (VkPhysicalDeviceRobustness2FeaturesEXT *)s;
   }
   if (!core && f2) core = &f2->features;
   if (core) { saved_core = *core; set_core_features(core, fake, VK_FALSE); }
   const VkBool32 saved_rba2 = rob ? rob->robustBufferAccess2 : VK_FALSE;
   if (rob && (fake & FAKE_ROBUSTNESS2)) rob->robustBufferAccess2 = VK_FALSE;

   VkResult result = create(pd, &copy, alloc, out);

   if (core) *core = saved_core;
   if (rob) rob->robustBufferAccess2 = saved_rba2;
   free(names);
   if (result != VK_SUCCESS) return result;

   install_segv_trace();
   struct device_data *dev = calloc(1, sizeof(*dev));
   if (!dev) return VK_ERROR_OUT_OF_HOST_MEMORY;
   dev->key = key_of(*out);
   dev->device = *out;
   dev->physical = pd;
   dev->fake = fake;
   dev->fake_semaphore_fd = fake_sem;
   dev->next_gdpa = gdpa;
   if (in->memory_props) in->memory_props(pd, &dev->memory);
   /* DROIDDECK_VENUS_BC_FULL=1 keeps BC1 at 32 bits a texel. */
   if ((fake & FAKE_BC) && !etc2_enabled && !env_off("DROIDDECK_VENUS_BC_FULL") && in->format_props) {
      const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
      VkFormatProperties a, b;
      in->format_props(pd, VK_FORMAT_R5G6B5_UNORM_PACK16, &a);
      in->format_props(pd, VK_FORMAT_A1R5G5B5_UNORM_PACK16, &b);
      dev->pack_bc1 = (a.optimalTilingFeatures & need) == need && (b.optimalTilingFeatures & need) == need;
   }
#define GET(field, name) dev->field = (void *)gdpa(*out, name)
   GET(destroy, "vkDestroyDevice");
   GET(create_semaphore, "vkCreateSemaphore");
   GET(create_image, "vkCreateImage");
   GET(create_swapchain, "vkCreateSwapchainKHR");
   GET(destroy_image, "vkDestroyImage");
   GET(create_image_view, "vkCreateImageView");
   GET(create_buffer, "vkCreateBuffer");
   GET(destroy_buffer, "vkDestroyBuffer");
   GET(buffer_reqs, "vkGetBufferMemoryRequirements");
   GET(allocate_memory, "vkAllocateMemory");
   GET(free_memory, "vkFreeMemory");
   GET(bind_buffer_memory, "vkBindBufferMemory");
   GET(bind_buffer_memory2, "vkBindBufferMemory2");
   GET(map_memory, "vkMapMemory");
   GET(map_memory2, "vkMapMemory2");
   if (!dev->map_memory2) GET(map_memory2, "vkMapMemory2KHR");
   GET(unmap_memory, "vkUnmapMemory");
   GET(unmap_memory2, "vkUnmapMemory2");
   if (!dev->unmap_memory2) GET(unmap_memory2, "vkUnmapMemory2KHR");
   GET(copy_buffer_to_image, "vkCmdCopyBufferToImage");
   GET(copy_buffer_to_image2, "vkCmdCopyBufferToImage2");
   if (!dev->copy_buffer_to_image2) GET(copy_buffer_to_image2, "vkCmdCopyBufferToImage2KHR");
   GET(set_viewport, "vkCmdSetViewport");
   GET(set_scissor, "vkCmdSetScissor");
   GET(set_viewport_with_count, "vkCmdSetViewportWithCount");
   if (!dev->set_viewport_with_count) GET(set_viewport_with_count, "vkCmdSetViewportWithCountEXT");
   GET(set_scissor_with_count, "vkCmdSetScissorWithCount");
   if (!dev->set_scissor_with_count) GET(set_scissor_with_count, "vkCmdSetScissorWithCountEXT");
   GET(set_polygon_mode, "vkCmdSetPolygonModeEXT");
   GET(create_graphics_pipelines, "vkCreateGraphicsPipelines");
   GET(create_shader_module, "vkCreateShaderModule");
   GET(image_reqs, "vkGetImageMemoryRequirements");
   GET(image_reqs2, "vkGetImageMemoryRequirements2");
   GET(device_image_reqs, "vkGetDeviceImageMemoryRequirements");
   if (!dev->image_reqs2) dev->image_reqs2 = (void *)gdpa(*out, "vkGetImageMemoryRequirements2KHR");
   if (!dev->device_image_reqs) dev->device_image_reqs = (void *)gdpa(*out, "vkGetDeviceImageMemoryRequirementsKHR");
   for (uint32_t i = 0; i < dev->memory.memoryTypeCount; i++)
      if (dev->memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
         dev->host_visible_types |= 1u << i;
   dev->memory_log = env_off("DROIDDECK_VENUS_MEMORY_LOG");
   GET(allocate_command_buffers, "vkAllocateCommandBuffers");
   GET(free_command_buffers, "vkFreeCommandBuffers");
   GET(reset_command_pool, "vkResetCommandPool");
   GET(destroy_command_pool, "vkDestroyCommandPool");
   GET(begin_command_buffer, "vkBeginCommandBuffer");
   GET(reset_command_buffer, "vkResetCommandBuffer");
   GET(queue_submit, "vkQueueSubmit");
   GET(queue_submit2, "vkQueueSubmit2");
   if (!dev->queue_submit2) GET(queue_submit2, "vkQueueSubmit2KHR");
#undef GET

   pthread_mutex_lock(&lock);
   int slot = -1;
   for (int i = 0; i < MAX_OBJECTS; i++)
      if (!devices[i]) { slot = i; break; }
   if (slot >= 0) devices[slot] = dev;
   pthread_mutex_unlock(&lock);
   if (slot < 0) {
      free(dev);
      return VK_ERROR_TOO_MANY_OBJECTS;
   }
   if (fake)
      fprintf(stderr, LAYER_NAME ": standing in for%s%s%s%s%s%s\n",
              fake & FAKE_BC ? " textureCompressionBC" : "", fake & FAKE_FILL_MODE ? " fillModeNonSolid" : "",
              fake & FAKE_MULTI_VIEWPORT ? " multiViewport" : "", fake & FAKE_CLIP_DISTANCE ? " shaderClipDistance" : "",
              fake & FAKE_CULL_DISTANCE ? " shaderCullDistance" : "", fake & FAKE_ROBUSTNESS2 ? " robustBufferAccess2" : "");
   return VK_SUCCESS;
}

static void free_map_values(struct map *m)
{
   for (uint32_t i = 0; i < m->cap; i++)
      if (m->keys[i] && m->keys[i] != UINT64_MAX) free(m->values[i]);
   free(m->keys);
   free(m->values);
}

static VKAPI_ATTR void VKAPI_CALL
layer_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
   if (!device) return;
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return;
   pthread_mutex_lock(&lock);
   for (int i = 0; i < MAX_OBJECTS; i++)
      if (devices[i] == dev) devices[i] = NULL;
   pthread_mutex_unlock(&lock);
   for (uint32_t i = 0; i < dev->decodes.cap; i++)
      if (dev->decodes.keys[i] && dev->decodes.keys[i] != UINT64_MAX) free_decodes(dev, dev->decodes.values[i]);
   free(dev->decodes.keys);
   free(dev->decodes.values);
   free(dev->images.keys);
   free(dev->images.values);
   free(dev->cmd_pools.keys);
   free(dev->cmd_pools.values);
   free_map_values(&dev->buffers);
   free_map_values(&dev->mappings);
   if (dev->destroy) dev->destroy(device, alloc);
   free(dev);
}

/* ---- semaphores (gamescope) ---- */

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo *info, const VkAllocationCallbacks *alloc, VkSemaphore *out)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev || !dev->create_semaphore) return VK_ERROR_INITIALIZATION_FAILED;
   if (!dev->fake_semaphore_fd) return dev->create_semaphore(device, info, alloc, out);
   VkBaseOutStructure *prev = (VkBaseOutStructure *)info, *export = NULL;
   for (VkBaseOutStructure *s = prev->pNext; s; prev = s, s = s->pNext)
      if (s->sType == VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO) { export = s; break; }
   if (export) prev->pNext = export->pNext;
   VkResult result = dev->create_semaphore(device, info, alloc, out);
   if (export) prev->pNext = export;
   return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_GetSemaphoreFdKHR(VkDevice device, const VkSemaphoreGetFdInfoKHR *info, int *fd)
{
   (void)device; (void)info;
   *fd = -1;
   return VK_ERROR_TOO_MANY_OBJECTS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_ImportSemaphoreFdKHR(VkDevice device, const VkImportSemaphoreFdInfoKHR *info)
{
   (void)device; (void)info;
   return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

/* ---- image placement ---- */

/* The server creates an image linear only when another process may see it (0108); every other
 * image keeps its tiling, Mali's own for render targets and textures. Host-visible memory is an
 * AHardwareBuffer blob there, laid out for linear images only, so those images are kept to the
 * memory types that are not host-visible. */
/* DROIDDECK_VENUS_LINEAR=1: every image linear, as before 0108. */
static bool all_linear(void)
{
   static int value = -1;
   if (value < 0) value = env_off("DROIDDECK_VENUS_LINEAR");
   return value;
}

static bool is_shared_image(const VkImageCreateInfo *info)
{
   if (all_linear() || info->tiling != VK_IMAGE_TILING_OPTIMAL) return true;
   for (const VkBaseInStructure *s = info->pNext; s; s = s->pNext)
      if (s->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO ||
          s->sType == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT ||
          s->sType == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT)
         return true;
   return false;
}

static void keep_private(struct device_data *dev, uint32_t *bits)
{
   const uint32_t private_bits = *bits & ~dev->host_visible_types;
   if (private_bits) *bits = private_bits;
   if (dev->memory_log) __atomic_fetch_add(private_bits ? &dev->images_private : &dev->images_left, 1, __ATOMIC_RELAXED);
}

static void account_memory(struct device_data *dev, uint32_t type, int64_t delta)
{
   if (type >= 32) return;
   const int64_t before = __atomic_fetch_add(&dev->type_bytes[type], delta, __ATOMIC_RELAXED);
   const int64_t after = before + delta, step = 128ll << 20;
   if (before / step != after / step)
      fprintf(stderr, LAYER_NAME ": memory type %u (%s): %lld MiB; image requirements steered %u, left %u\n",
              type, dev->host_visible_types & (1u << type) ? "host-visible" : "device-only", (long long)(after >> 20),
              __atomic_load_n(&dev->images_private, __ATOMIC_RELAXED), __atomic_load_n(&dev->images_left, __ATOMIC_RELAXED));
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *alloc,
                     VkDeviceMemory *out)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
   VkResult r = dev->allocate_memory(device, info, alloc, out);
   if (r != VK_SUCCESS) {
      /* Failures are rare and fatal for most clients (Chromium drops its GPU process), so always say what was asked. */
      char chain[256] = "";
      size_t n = 0;
      for (const VkBaseInStructure *s = info->pNext; s && n < sizeof(chain) - 16; s = s->pNext)
         n += snprintf(chain + n, sizeof(chain) - n, " %d", (int)s->sType);
      fprintf(stderr, LAYER_NAME ": vkAllocateMemory failed (%d): %llu bytes, type %u (%s), pNext sTypes:%s\n", (int)r,
              (unsigned long long)info->allocationSize, info->memoryTypeIndex,
              dev->host_visible_types & (1u << info->memoryTypeIndex) ? "host-visible" : "device-only", chain);
      for (const VkBaseInStructure *s = info->pNext; s; s = s->pNext)
         if (s->sType == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO)
            fprintf(stderr, LAYER_NAME ":   export handle types 0x%x\n",
                    (unsigned)((const VkExportMemoryAllocateInfo *)s)->handleTypes);
         else if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO)
            fprintf(stderr, LAYER_NAME ":   dedicated image %p buffer %p\n",
                    (void *)((const VkMemoryDedicatedAllocateInfo *)s)->image,
                    (void *)((const VkMemoryDedicatedAllocateInfo *)s)->buffer);
   }
   if (r == VK_SUCCESS && dev->memory_log) {
      pthread_mutex_lock(&lock);
      map_put(&dev->allocations, H(*out), (void *)(uintptr_t)(info->allocationSize << 5 | info->memoryTypeIndex));
      pthread_mutex_unlock(&lock);
      account_memory(dev, info->memoryTypeIndex, (int64_t)info->allocationSize);
   }
   return r;
}

static bool image_is_shared(struct device_data *dev, VkImage image)
{
   pthread_mutex_lock(&lock);
   const bool shared = map_get(&dev->shared_images, H(image)) != NULL;
   pthread_mutex_unlock(&lock);
   return shared;
}

static VKAPI_ATTR void VKAPI_CALL
layer_GetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements *reqs)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return;
   dev->image_reqs(device, image, reqs);
   if (!image_is_shared(dev, image)) keep_private(dev, &reqs->memoryTypeBits);
}

static VKAPI_ATTR void VKAPI_CALL
layer_GetImageMemoryRequirements2(VkDevice device, const VkImageMemoryRequirementsInfo2 *info,
                                  VkMemoryRequirements2 *reqs)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return;
   dev->image_reqs2(device, info, reqs);
   if (!image_is_shared(dev, info->image)) keep_private(dev, &reqs->memoryRequirements.memoryTypeBits);
}

static VKAPI_ATTR void VKAPI_CALL
layer_GetDeviceImageMemoryRequirements(VkDevice device, const VkDeviceImageMemoryRequirements *info,
                                       VkMemoryRequirements2 *reqs)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return;
   dev->device_image_reqs(device, info, reqs);
   if (!is_shared_image(info->pCreateInfo)) keep_private(dev, &reqs->memoryRequirements.memoryTypeBits);
}

/* ---- swapchains ---- */

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                         const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
   fprintf(stderr, LAYER_NAME ": swapchain %ux%u format %d images %u\n", info->imageExtent.width,
           info->imageExtent.height, info->imageFormat, info->minImageCount);
   return dev->create_swapchain(device, info, alloc, out);
}

/* ---- BC images ---- */

static VkResult create_image_bc(struct device_data *dev, VkDevice device, const VkImageCreateInfo *info,
                                const VkAllocationCallbacks *alloc, VkImage *out);

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateImage(VkDevice device, const VkImageCreateInfo *info, const VkAllocationCallbacks *alloc, VkImage *out)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
   VkImageCreateInfo linear;
   if (all_linear() && info->tiling == VK_IMAGE_TILING_OPTIMAL) {
      linear = *info;
      linear.tiling = VK_IMAGE_TILING_LINEAR;
      info = &linear;
   }
   VkResult result = create_image_bc(dev, device, info, alloc, out);
   if (result == VK_SUCCESS && is_shared_image(info)) {
      pthread_mutex_lock(&lock);
      map_put(&dev->shared_images, H(*out), (void *)1);
      pthread_mutex_unlock(&lock);
   }
   return result;
}

static VkResult
create_image_bc(struct device_data *dev, VkDevice device, const VkImageCreateInfo *info,
                const VkAllocationCallbacks *alloc, VkImage *out)
{
   if (!(dev->fake & FAKE_BC) || !is_bc(info->format)) return dev->create_image(device, info, alloc, out);

   VkImageCreateInfo copy = *info;
   const uint32_t raw = etc2_enabled && info->imageType == VK_IMAGE_TYPE_3D ? RAW_BIT : 0;
   copy.format = image_substitute(info->format, raw);
   copy.flags &= ~VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
   copy.tiling = all_linear() ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
   /* Packed only when no view can ask for another format (an sRGB view, say). */
   bool packed = dev->pack_bc1 && !raw && packed_substitute(info->format);
   /* A view-format list: BC formats become the substitute, others (block-sized views for copies)
    * have no texel equivalent and are dropped. */
   VkImageFormatListCreateInfo *list = NULL;
   VkBaseOutStructure *prev = (VkBaseOutStructure *)&copy;
   for (VkBaseOutStructure *s = prev->pNext; s; prev = s, s = s->pNext)
      if (s->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) { list = (VkImageFormatListCreateInfo *)s; break; }
   VkFormat formats[32];
   const VkFormat *saved_formats = list ? list->pViewFormats : NULL;
   const uint32_t saved_count = list ? list->viewFormatCount : 0;
   if (list) {
      uint32_t n = 0;
      for (uint32_t i = 0; i < list->viewFormatCount && n < ARRAY_LEN(formats); i++) {
         VkFormat f = list->pViewFormats[i];
         if (!is_bc(f)) continue;
         f = image_substitute(f, raw);
         bool seen = false;
         for (uint32_t j = 0; j < n; j++) seen |= formats[j] == f;
         if (!seen) formats[n++] = f;
      }
      if (!n) formats[n++] = copy.format;
      if (packed && n == 1 && formats[0] == copy.format) formats[0] = packed_substitute(info->format);
      else packed = false;
      list->pViewFormats = formats;
      list->viewFormatCount = n;
      if (n == 1) copy.flags &= ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
   } else if (copy.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) {
      packed = false;
   }
   if (packed) copy.format = packed_substitute(info->format);
   VkResult result = dev->create_image(device, &copy, alloc, out);
   if (list) {
      list->pViewFormats = saved_formats;
      list->viewFormatCount = saved_count;
   }
   if (result == VK_SUCCESS) {
      pthread_mutex_lock(&lock);
      map_put(&dev->images, H(*out), (void *)(uintptr_t)(info->format | (packed ? PACKED_BIT : 0) | raw));
      pthread_mutex_unlock(&lock);
   }
   return result;
}

static VKAPI_ATTR void VKAPI_CALL
layer_DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *alloc)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return;
   if (image) {
      pthread_mutex_lock(&lock);
      map_take(&dev->images, H(image));
      map_take(&dev->shared_images, H(image));
      pthread_mutex_unlock(&lock);
   }
   dev->destroy_image(device, image, alloc);
}

static VkFormat substituted_format(struct device_data *dev, VkImage image, uint32_t *kind)
{
   pthread_mutex_lock(&lock);
   uintptr_t f = (uintptr_t)map_get(&dev->images, H(image));
   pthread_mutex_unlock(&lock);
   if (kind) *kind = (uint32_t)(f & KIND_BITS);
   return (VkFormat)(f & ~(uintptr_t)KIND_BITS);
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateImageView(VkDevice device, const VkImageViewCreateInfo *info, const VkAllocationCallbacks *alloc, VkImageView *out)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
   uint32_t kind;
   const VkFormat orig = substituted_format(dev, info->image, &kind);
   if (!orig) return dev->create_image_view(device, info, alloc, out);
   VkImageViewCreateInfo copy = *info;
   copy.format = (kind & PACKED_BIT) ? packed_substitute(orig)
                                     : image_substitute(is_bc(info->format) ? info->format : orig, kind);
   return dev->create_image_view(device, &copy, alloc, out);
}

/* ---- buffers and mappings, for reading the application's blocks at submit ---- */

static void track_binding(struct device_data *dev, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset)
{
   struct buffer_binding *b = malloc(sizeof(*b));
   if (!b) return;
   *b = (struct buffer_binding){ memory, offset };
   pthread_mutex_lock(&lock);
   free(map_take(&dev->buffers, H(buffer)));
   map_put(&dev->buffers, H(buffer), b);
   pthread_mutex_unlock(&lock);
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_BindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
   VkResult r = dev->bind_buffer_memory(device, buffer, memory, offset);
   if (r == VK_SUCCESS) track_binding(dev, buffer, memory, offset);
   return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_BindBufferMemory2(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo *infos)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
   VkResult r = dev->bind_buffer_memory2(device, count, infos);
   if (r == VK_SUCCESS)
      for (uint32_t i = 0; i < count; i++) track_binding(dev, infos[i].buffer, infos[i].memory, infos[i].memoryOffset);
   return r;
}

static VKAPI_ATTR void VKAPI_CALL
layer_DestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *alloc)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return;
   if (buffer) {
      pthread_mutex_lock(&lock);
      free(map_take(&dev->buffers, H(buffer)));
      pthread_mutex_unlock(&lock);
   }
   dev->destroy_buffer(device, buffer, alloc);
}

static void track_map(struct device_data *dev, VkDeviceMemory memory, VkDeviceSize offset, void *ptr)
{
   struct mapping *m = malloc(sizeof(*m));
   if (!m) return;
   m->base = (uint8_t *)ptr - offset;
   m->ours = false;
   pthread_mutex_lock(&lock);
   free(map_take(&dev->mappings, H(memory)));
   map_put(&dev->mappings, H(memory), m);
   pthread_mutex_unlock(&lock);
}

static void untrack_map(struct device_data *dev, VkDeviceMemory memory)
{
   pthread_mutex_lock(&lock);
   free(map_take(&dev->mappings, H(memory)));
   pthread_mutex_unlock(&lock);
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_MapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
                VkMemoryMapFlags flags, void **ptr)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
   VkResult r = dev->map_memory(device, memory, offset, size, flags, ptr);
   if (r == VK_SUCCESS) track_map(dev, memory, offset, *ptr);
   return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_MapMemory2(VkDevice device, const VkMemoryMapInfo *info, void **ptr)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
   VkResult r = dev->map_memory2(device, info, ptr);
   if (r == VK_SUCCESS) track_map(dev, info->memory, info->offset, *ptr);
   return r;
}

static VKAPI_ATTR void VKAPI_CALL
layer_UnmapMemory(VkDevice device, VkDeviceMemory memory)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return;
   untrack_map(dev, memory);
   dev->unmap_memory(device, memory);
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_UnmapMemory2(VkDevice device, const VkMemoryUnmapInfo *info)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
   untrack_map(dev, info->memory);
   return dev->unmap_memory2(device, info);
}

static VKAPI_ATTR void VKAPI_CALL
layer_FreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *alloc)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return;
   if (memory) untrack_map(dev, memory);
   if (memory && dev->memory_log) {
      pthread_mutex_lock(&lock);
      const uintptr_t v = (uintptr_t)map_take(&dev->allocations, H(memory));
      pthread_mutex_unlock(&lock);
      if (v) account_memory(dev, (uint32_t)(v & 31), -(int64_t)(v >> 5));
   }
   dev->free_memory(device, memory, alloc);
}

/* ---- uploads into BC images ---- */

/* Plans one region's decode; returns false (region skipped) when it cannot. */
static bool plan_region(struct device_data *dev, VkCommandBuffer cb, VkFormat format, uint32_t kind, VkBuffer src,
                        VkDeviceSize offset, uint32_t row_length, uint32_t image_height,
                        VkExtent3D extent, uint32_t layers, VkBuffer *staging_out)
{
   struct decode *d = calloc(1, sizeof(*d));
   if (!d) return false;
   d->format = format;
   d->kind = kind;
   d->src = src;
   d->src_offset = offset;
   d->blocks_w = (extent.width + 3) / 4;
   d->blocks_h = (extent.height + 3) / 4;
   d->row_blocks = row_length ? (row_length + 3) / 4 : d->blocks_w;
   d->height_blocks = image_height ? (image_height + 3) / 4 : d->blocks_h;
   d->slices = (extent.depth ? extent.depth : 1) * (layers ? layers : 1);
   const VkFormat sub = image_substitute(format, kind);
   const VkDeviceSize size = etc_block_bytes(sub)
      ? (VkDeviceSize)d->blocks_w * d->blocks_h * etc_block_bytes(sub) * d->slices
      : (VkDeviceSize)d->blocks_w * 4 * d->blocks_h * 4 * texel_bytes(sub) * d->slices;
   if (!create_staging(dev, size, d)) {
      free_decodes(dev, d);
      return false;
   }
   pthread_mutex_lock(&lock);
   d->next = map_get(&dev->decodes, H(cb));
   map_put(&dev->decodes, H(cb), d);
   pthread_mutex_unlock(&lock);
   *staging_out = d->staging;
   return true;
}

static VKAPI_ATTR void VKAPI_CALL
layer_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout layout,
                           uint32_t count, const VkBufferImageCopy *regions)
{
   struct device_data *dev = find_device(key_of(cb));
   if (!dev) return;
   uint32_t kind;
   const VkFormat orig = substituted_format(dev, dst, &kind);
   if (!orig) {
      dev->copy_buffer_to_image(cb, src, dst, layout, count, regions);
      return;
   }
   for (uint32_t i = 0; i < count; i++) {
      const VkBufferImageCopy *r = &regions[i];
      VkBuffer staging;
      if (!plan_region(dev, cb, orig, kind, src, r->bufferOffset, r->bufferRowLength, r->bufferImageHeight,
                       r->imageExtent, r->imageSubresource.layerCount, &staging))
         continue;
      VkBufferImageCopy c = *r;
      c.bufferOffset = 0;
      c.bufferRowLength = (r->imageExtent.width + 3) & ~3u;
      c.bufferImageHeight = (r->imageExtent.height + 3) & ~3u;
      dev->copy_buffer_to_image(cb, staging, dst, layout, 1, &c);
   }
}

static VKAPI_ATTR void VKAPI_CALL
layer_CmdCopyBufferToImage2(VkCommandBuffer cb, const VkCopyBufferToImageInfo2 *info)
{
   struct device_data *dev = find_device(key_of(cb));
   if (!dev) return;
   uint32_t kind;
   const VkFormat orig = substituted_format(dev, info->dstImage, &kind);
   if (!orig) {
      dev->copy_buffer_to_image2(cb, info);
      return;
   }
   for (uint32_t i = 0; i < info->regionCount; i++) {
      const VkBufferImageCopy2 *r = &info->pRegions[i];
      VkBuffer staging;
      if (!plan_region(dev, cb, orig, kind, info->srcBuffer, r->bufferOffset, r->bufferRowLength,
                       r->bufferImageHeight, r->imageExtent, r->imageSubresource.layerCount, &staging))
         continue;
      VkBufferImageCopy2 c = *r;
      c.bufferOffset = 0;
      c.bufferRowLength = (r->imageExtent.width + 3) & ~3u;
      c.bufferImageHeight = (r->imageExtent.height + 3) & ~3u;
      VkCopyBufferToImageInfo2 one = *info;
      one.srcBuffer = staging;
      one.regionCount = 1;
      one.pRegions = &c;
      dev->copy_buffer_to_image2(cb, &one);
   }
}

/* ---- command buffer lifetimes: staging is freed when its command buffer is reset ---- */

static VKAPI_ATTR VkResult VKAPI_CALL
layer_AllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *info, VkCommandBuffer *out)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
   VkResult r = dev->allocate_command_buffers(device, info, out);
   if (r == VK_SUCCESS) {
      pthread_mutex_lock(&lock);
      for (uint32_t i = 0; i < info->commandBufferCount; i++)
         map_put(&dev->cmd_pools, H(out[i]), (void *)(uintptr_t)info->commandPool);
      pthread_mutex_unlock(&lock);
   }
   return r;
}

static VKAPI_ATTR void VKAPI_CALL
layer_FreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer *cbs)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return;
   for (uint32_t i = 0; i < count; i++) {
      if (!cbs[i]) continue;
      release_command_buffer(dev, cbs[i]);
      pthread_mutex_lock(&lock);
      map_take(&dev->cmd_pools, H(cbs[i]));
      pthread_mutex_unlock(&lock);
   }
   dev->free_command_buffers(device, pool, count, cbs);
}

static void release_pool(struct device_data *dev, VkCommandPool pool, bool forget)
{
   for (;;) {
      VkCommandBuffer found = VK_NULL_HANDLE;
      pthread_mutex_lock(&lock);
      for (uint32_t i = 0; i < dev->cmd_pools.cap && !found; i++) {
         uint64_t k = dev->cmd_pools.keys[i];
         if (k && k != UINT64_MAX && (uintptr_t)dev->cmd_pools.values[i] == (uintptr_t)pool &&
             (forget || map_get(&dev->decodes, k)))
            found = (VkCommandBuffer)(uintptr_t)k;
      }
      if (found && forget) map_take(&dev->cmd_pools, H(found));
      pthread_mutex_unlock(&lock);
      if (!found) return;
      release_command_buffer(dev, found);
   }
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_ResetCommandPool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
   release_pool(dev, pool, false);
   return dev->reset_command_pool(device, pool, flags);
}

static VKAPI_ATTR void VKAPI_CALL
layer_DestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks *alloc)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return;
   if (pool) release_pool(dev, pool, true);
   dev->destroy_command_pool(device, pool, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *info)
{
   struct device_data *dev = find_device(key_of(cb));
   if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
   release_command_buffer(dev, cb);
   return dev->begin_command_buffer(cb, info);
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_ResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags flags)
{
   struct device_data *dev = find_device(key_of(cb));
   if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
   release_command_buffer(dev, cb);
   return dev->reset_command_buffer(cb, flags);
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence)
{
   struct device_data *dev = find_device(key_of(queue));
   if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
   if (dev->fake & FAKE_BC)
      for (uint32_t i = 0; i < count; i++)
         for (uint32_t j = 0; j < submits[i].commandBufferCount; j++) run_decodes(dev, submits[i].pCommandBuffers[j]);
   return dev->queue_submit(queue, count, submits, fence);
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence fence)
{
   struct device_data *dev = find_device(key_of(queue));
   if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
   if (dev->fake & FAKE_BC)
      for (uint32_t i = 0; i < count; i++)
         for (uint32_t j = 0; j < submits[i].commandBufferInfoCount; j++)
            run_decodes(dev, submits[i].pCommandBufferInfos[j].commandBuffer);
   return dev->queue_submit2(queue, count, submits, fence);
}

/* ---- viewports and fill modes ---- */

static VKAPI_ATTR void VKAPI_CALL
layer_CmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkViewport *v)
{
   struct device_data *dev = find_device(key_of(cb));
   if (!dev) return;
   if (dev->fake & FAKE_MULTI_VIEWPORT) {
      if (first > 0) return;
      if (count > 1) count = 1;
   }
   dev->set_viewport(cb, first, count, v);
}

static VKAPI_ATTR void VKAPI_CALL
layer_CmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkRect2D *s)
{
   struct device_data *dev = find_device(key_of(cb));
   if (!dev) return;
   if (dev->fake & FAKE_MULTI_VIEWPORT) {
      if (first > 0) return;
      if (count > 1) count = 1;
   }
   dev->set_scissor(cb, first, count, s);
}

static VKAPI_ATTR void VKAPI_CALL
layer_CmdSetViewportWithCount(VkCommandBuffer cb, uint32_t count, const VkViewport *v)
{
   struct device_data *dev = find_device(key_of(cb));
   if (!dev) return;
   if ((dev->fake & FAKE_MULTI_VIEWPORT) && count > 1) count = 1;
   dev->set_viewport_with_count(cb, count, v);
}

static VKAPI_ATTR void VKAPI_CALL
layer_CmdSetScissorWithCount(VkCommandBuffer cb, uint32_t count, const VkRect2D *s)
{
   struct device_data *dev = find_device(key_of(cb));
   if (!dev) return;
   if ((dev->fake & FAKE_MULTI_VIEWPORT) && count > 1) count = 1;
   dev->set_scissor_with_count(cb, count, s);
}

static VKAPI_ATTR void VKAPI_CALL
layer_CmdSetPolygonModeEXT(VkCommandBuffer cb, VkPolygonMode mode)
{
   struct device_data *dev = find_device(key_of(cb));
   if (!dev) return;
   if (dev->fake & FAKE_FILL_MODE) mode = VK_POLYGON_MODE_FILL;
   dev->set_polygon_mode(cb, mode);
}

/* ---- shaders: clip and cull distances become private variables ---- */

/* Mali's compiler has no clip or cull distance and crashes on a shader that accesses one
 * (spir2lir::on_AccessChain, Dark Souls Remastered on loading a save). The builtins become Private
 * variables of the same type: writes go nowhere, reads see what the shader wrote. Geometry is not
 * clipped by them - the cost of standing in for the feature. */

enum {
   SPV_OP_NAME_ENTRY_POINT = 15, SPV_OP_CAPABILITY = 17, SPV_OP_TYPE_POINTER = 32,
   SPV_OP_VARIABLE = 59, SPV_OP_ACCESS_CHAIN = 65, SPV_OP_IN_BOUNDS_ACCESS_CHAIN = 66,
   SPV_OP_DECORATE = 71, SPV_OP_MEMBER_DECORATE = 72,
   SPV_CAP_CLIP = 32, SPV_CAP_CULL = 33, SPV_DEC_BUILTIN = 11, SPV_BI_CLIP = 3, SPV_BI_CULL = 4,
   SPV_SC_PRIVATE = 6,
};

struct spv_ids { uint32_t *v; uint32_t n, cap; };

static bool ids_has(const struct spv_ids *s, uint32_t id)
{
   for (uint32_t i = 0; i < s->n; i++)
      if (s->v[i] == id) return true;
   return false;
}

static bool ids_append(struct spv_ids *s, uint32_t id)
{
   if (s->n == s->cap) {
      uint32_t cap = s->cap ? s->cap * 2 : 16;
      uint32_t *v = realloc(s->v, cap * sizeof(*v));
      if (!v) return false;
      s->v = v;
      s->cap = cap;
   }
   s->v[s->n++] = id;
   return true;
}

static bool ids_add(struct spv_ids *s, uint32_t id) { return ids_has(s, id) || ids_append(s, id); }

/* types[k] -> values[k], or 0 */
static uint32_t ids_lookup(const struct spv_ids *keys, const struct spv_ids *values, uint32_t key)
{
   for (uint32_t k = 0; k < keys->n; k++)
      if (keys->v[k] == key) return values->v[k];
   return 0;
}

static uint32_t private_index(const struct spv_ids *pointees, uint32_t pointee)
{
   for (uint32_t k = 0; k < pointees->n; k++)
      if (pointees->v[k] == pointee) return k;
   return 0;
}

/* Returns a rewritten copy (malloc'd, *out_size bytes) or NULL when the code has no clip or cull
 * distance or cannot be rewritten. */
static uint32_t *strip_clip_cull(const uint32_t *code, size_t size, size_t *out_size)
{
   const uint32_t n = (uint32_t)(size / 4);
   if (n < 5 || code[0] != 0x07230203) return NULL;
   bool any = false;
   for (uint32_t i = 5; i < n;) {
      const uint32_t wc = code[i] >> 16, op = code[i] & 0xffff;
      if (!wc || i + wc > n) return NULL;
      if (op == SPV_OP_CAPABILITY && (code[i + 1] == SPV_CAP_CLIP || code[i + 1] == SPV_CAP_CULL)) any = true;
      if (op == SPV_OP_TYPE_POINTER || op == SPV_OP_VARIABLE) break; /* capabilities come first */
      i += wc;
   }
   if (!any) return NULL;

   /* Pass 1: the builtin variables, every pointer derived from them, and the pointees whose
    * Private pointer types are needed. */
   struct spv_ids vars = {0}, ptrs = {0}, pointees = {0}, ptr_types = {0}, ptr_pointee = {0};
   uint32_t *result = NULL;
   bool ok = true;
   for (uint32_t i = 5; i < n; i += code[i] >> 16) {
      const uint32_t op = code[i] & 0xffff, wc = code[i] >> 16;
      if (op == SPV_OP_DECORATE && wc >= 4 && code[i + 2] == SPV_DEC_BUILTIN &&
          (code[i + 3] == SPV_BI_CLIP || code[i + 3] == SPV_BI_CULL))
         ok &= ids_add(&vars, code[i + 1]);
      else if (op == SPV_OP_MEMBER_DECORATE && wc >= 5 && code[i + 3] == SPV_DEC_BUILTIN &&
               (code[i + 4] == SPV_BI_CLIP || code[i + 4] == SPV_BI_CULL))
         goto out; /* a gl_PerVertex member: not handled */
      else if (op == SPV_OP_TYPE_POINTER && wc == 4) {
         ok &= ids_append(&ptr_types, code[i + 1]);
         ok &= ids_append(&ptr_pointee, code[i + 3]); /* parallel to ptr_types */
      }
   }
   if (!ok || !vars.n) goto out;
#define POINTEE_OF(type) ids_lookup(&ptr_types, &ptr_pointee, type)
   for (uint32_t i = 5; i < n; i += code[i] >> 16) {
      const uint32_t op = code[i] & 0xffff, wc = code[i] >> 16;
      if (op == SPV_OP_VARIABLE && wc >= 4 && ids_has(&vars, code[i + 2])) {
         const uint32_t p = POINTEE_OF(code[i + 1]);
         if (!p) goto out;
         ok &= ids_add(&ptrs, code[i + 2]) && ids_add(&pointees, p);
      } else if ((op == SPV_OP_ACCESS_CHAIN || op == SPV_OP_IN_BOUNDS_ACCESS_CHAIN) && wc >= 4 &&
                 ids_has(&ptrs, code[i + 3])) {
         const uint32_t p = POINTEE_OF(code[i + 1]);
         if (!p) goto out;
         ok &= ids_add(&ptrs, code[i + 2]) && ids_add(&pointees, p);
      }
   }
   if (!ok) goto out;

   /* Pass 2: copy, with a Private pointer type declared after each pointee's definition. */
   uint32_t bound = code[3];
   const uint32_t first_new = bound;
   bound += pointees.n;
   const bool lists_private = code[1] >= 0x00010400; /* SPIR-V 1.4+ lists every global in OpEntryPoint */
   result = malloc((size_t)(n + pointees.n * 4) * 4);
   if (!result) goto out;
   uint32_t w = 0;
   memcpy(result, code, 5 * 4);
   result[3] = bound;
   w = 5;
#define PRIVATE_PTR(pointee) (first_new + private_index(&pointees, pointee))
   for (uint32_t i = 5; i < n;) {
      const uint32_t op = code[i] & 0xffff, wc = code[i] >> 16;
      const uint32_t start = w;
      if (op == SPV_OP_CAPABILITY && (code[i + 1] == SPV_CAP_CLIP || code[i + 1] == SPV_CAP_CULL)) {
         i += wc;
         continue;
      }
      if (op == SPV_OP_DECORATE && ids_has(&vars, code[i + 1])) {
         i += wc;
         continue;
      }
      if (op == SPV_OP_NAME_ENTRY_POINT && !lists_private) {
         /* execution model, function, name (a nul-terminated string), interface ids */
         uint32_t j = i + 3;
         while (j < i + wc && ((code[j] >> 24) && (code[j] >> 16 & 0xff) && (code[j] >> 8 & 0xff) && (code[j] & 0xff))) j++;
         j++; /* the word holding the nul */
         if (j > i + wc) j = i + wc;
         memcpy(result + w, code + i, (j - i) * 4);
         w += j - i;
         for (; j < i + wc; j++)
            if (!ids_has(&vars, code[j])) result[w++] = code[j];
         result[start] = (w - start) << 16 | op;
         i += wc;
         continue;
      }
      memcpy(result + w, code + i, wc * 4);
      w += wc;
      if (op == SPV_OP_VARIABLE && ids_has(&vars, code[i + 2])) {
         result[start + 1] = PRIVATE_PTR(POINTEE_OF(code[i + 1]));
         result[start + 3] = SPV_SC_PRIVATE;
      } else if ((op == SPV_OP_ACCESS_CHAIN || op == SPV_OP_IN_BOUNDS_ACCESS_CHAIN) && ids_has(&ptrs, code[i + 2])) {
         result[start + 1] = PRIVATE_PTR(POINTEE_OF(code[i + 1]));
      }
      /* Declare the Private pointers to a type right after the type. Every OpType* has its
       * result id first. */
      if (op >= 19 && op <= 39 && wc >= 2)
         for (uint32_t k = 0; k < pointees.n; k++)
            if (pointees.v[k] == code[i + 1]) {
               result[w++] = 4u << 16 | SPV_OP_TYPE_POINTER;
               result[w++] = first_new + k;
               result[w++] = SPV_SC_PRIVATE;
               result[w++] = code[i + 1];
            }
      i += wc;
   }
#undef PRIVATE_PTR
#undef POINTEE_OF
   *out_size = (size_t)w * 4;
   static bool told;
   if (!told) fprintf(stderr, LAYER_NAME ": clip/cull distances made private in a shader\n");
   told = true;
out:
   free(vars.v); free(ptrs.v); free(pointees.v); free(ptr_types.v); free(ptr_pointee.v);
   if (result && !*out_size) { free(result); result = NULL; }
   return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *info,
                         const VkAllocationCallbacks *alloc, VkShaderModule *out)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
   size_t size = 0;
   uint32_t *code = strip_clip_cull(info->pCode, info->codeSize, &size);
   if (!code) return dev->create_shader_module(device, info, alloc, out);
   VkShaderModuleCreateInfo copy = *info;
   copy.pCode = code;
   copy.codeSize = size;
   VkResult r = dev->create_shader_module(device, &copy, alloc, out);
   free(code);
   return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL
layer_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                              const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *alloc,
                              VkPipeline *out)
{
   struct device_data *dev = find_device(key_of(device));
   if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
   if (!(dev->fake & (FAKE_FILL_MODE | FAKE_MULTI_VIEWPORT | FAKE_CLIP_DISTANCE | FAKE_CULL_DISTANCE)))
      return dev->create_graphics_pipelines(device, cache, count, infos, alloc, out);

   /* The application's states are adjusted for the call and put back after it. */
   struct saved { VkPolygonMode mode; uint32_t viewports, scissors; } *saved = calloc(count ? count : 1, sizeof(*saved));
   if (!saved) return VK_ERROR_OUT_OF_HOST_MEMORY;
   /* Shader code given inline (a VkShaderModuleCreateInfo in a stage's pNext). */
   struct inline_code { VkShaderModuleCreateInfo *info; const uint32_t *code; size_t size; uint32_t *ours; };
   struct inline_code *inl = NULL;
   uint32_t inl_n = 0, inl_cap = 0;
   for (uint32_t i = 0; i < count && (dev->fake & (FAKE_CLIP_DISTANCE | FAKE_CULL_DISTANCE)); i++)
      for (uint32_t s = 0; s < infos[i].stageCount; s++) {
         if (infos[i].pStages[s].module) continue;
         for (VkBaseOutStructure *e = (VkBaseOutStructure *)infos[i].pStages[s].pNext; e; e = e->pNext) {
            if (e->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) continue;
            VkShaderModuleCreateInfo *m = (VkShaderModuleCreateInfo *)e;
            size_t size = 0;
            uint32_t *code = strip_clip_cull(m->pCode, m->codeSize, &size);
            if (!code) break;
            if (inl_n == inl_cap) {
               inl_cap = inl_cap ? inl_cap * 2 : 8;
               struct inline_code *grown = realloc(inl, inl_cap * sizeof(*inl));
               if (!grown) { free(code); break; }
               inl = grown;
            }
            inl[inl_n++] = (struct inline_code){ m, m->pCode, m->codeSize, code };
            m->pCode = code;
            m->codeSize = size;
            break;
         }
      }
   for (uint32_t i = 0; i < count; i++) {
      VkPipelineRasterizationStateCreateInfo *rs = (VkPipelineRasterizationStateCreateInfo *)infos[i].pRasterizationState;
      VkPipelineViewportStateCreateInfo *vp = (VkPipelineViewportStateCreateInfo *)infos[i].pViewportState;
      if (rs) {
         saved[i].mode = rs->polygonMode;
         if (dev->fake & FAKE_FILL_MODE) rs->polygonMode = VK_POLYGON_MODE_FILL;
      }
      if (vp) {
         saved[i].viewports = vp->viewportCount;
         saved[i].scissors = vp->scissorCount;
         if (dev->fake & FAKE_MULTI_VIEWPORT) {
            if (vp->viewportCount > 1) vp->viewportCount = 1;
            if (vp->scissorCount > 1) vp->scissorCount = 1;
         }
      }
   }
   VkResult r = dev->create_graphics_pipelines(device, cache, count, infos, alloc, out);
   for (uint32_t k = 0; k < inl_n; k++) {
      inl[k].info->pCode = inl[k].code;
      inl[k].info->codeSize = inl[k].size;
      free(inl[k].ours);
   }
   free(inl);
   for (uint32_t i = 0; i < count; i++) {
      VkPipelineRasterizationStateCreateInfo *rs = (VkPipelineRasterizationStateCreateInfo *)infos[i].pRasterizationState;
      VkPipelineViewportStateCreateInfo *vp = (VkPipelineViewportStateCreateInfo *)infos[i].pViewportState;
      if (rs) rs->polygonMode = saved[i].mode;
      if (vp) {
         vp->viewportCount = saved[i].viewports;
         vp->scissorCount = saved[i].scissors;
      }
   }
   free(saved);
   return r;
}

/* ---- dispatch ---- */

struct entry { const char *name; PFN_vkVoidFunction fn; uint32_t needs; };
/* needs: 0 always; 1 the semaphore stand-in; otherwise a FAKE_* mask */
#define SEM 0x80000000u

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL venus_GetDeviceProcAddr(VkDevice device, const char *name);

static const struct entry device_entries[] = {
   { "vkGetDeviceProcAddr", (PFN_vkVoidFunction)venus_GetDeviceProcAddr, 0 },
   { "vkDestroyDevice", (PFN_vkVoidFunction)layer_DestroyDevice, 0 },
   { "vkCreateSemaphore", (PFN_vkVoidFunction)layer_CreateSemaphore, SEM },
   { "vkGetSemaphoreFdKHR", (PFN_vkVoidFunction)layer_GetSemaphoreFdKHR, SEM },
   { "vkImportSemaphoreFdKHR", (PFN_vkVoidFunction)layer_ImportSemaphoreFdKHR, SEM },
   { "vkCreateSwapchainKHR", (PFN_vkVoidFunction)layer_CreateSwapchainKHR, 0 },
   { "vkCreateImage", (PFN_vkVoidFunction)layer_CreateImage, 0 },
   { "vkDestroyImage", (PFN_vkVoidFunction)layer_DestroyImage, 0 },
   { "vkGetImageMemoryRequirements", (PFN_vkVoidFunction)layer_GetImageMemoryRequirements, 0 },
   { "vkGetImageMemoryRequirements2", (PFN_vkVoidFunction)layer_GetImageMemoryRequirements2, 0 },
   { "vkGetImageMemoryRequirements2KHR", (PFN_vkVoidFunction)layer_GetImageMemoryRequirements2, 0 },
   { "vkGetDeviceImageMemoryRequirements", (PFN_vkVoidFunction)layer_GetDeviceImageMemoryRequirements, 0 },
   { "vkGetDeviceImageMemoryRequirementsKHR", (PFN_vkVoidFunction)layer_GetDeviceImageMemoryRequirements, 0 },
   { "vkCreateImageView", (PFN_vkVoidFunction)layer_CreateImageView, FAKE_BC },
   { "vkBindBufferMemory", (PFN_vkVoidFunction)layer_BindBufferMemory, FAKE_BC },
   { "vkBindBufferMemory2", (PFN_vkVoidFunction)layer_BindBufferMemory2, FAKE_BC },
   { "vkBindBufferMemory2KHR", (PFN_vkVoidFunction)layer_BindBufferMemory2, FAKE_BC },
   { "vkDestroyBuffer", (PFN_vkVoidFunction)layer_DestroyBuffer, FAKE_BC },
   { "vkMapMemory", (PFN_vkVoidFunction)layer_MapMemory, FAKE_BC },
   { "vkMapMemory2", (PFN_vkVoidFunction)layer_MapMemory2, FAKE_BC },
   { "vkMapMemory2KHR", (PFN_vkVoidFunction)layer_MapMemory2, FAKE_BC },
   { "vkUnmapMemory", (PFN_vkVoidFunction)layer_UnmapMemory, FAKE_BC },
   { "vkUnmapMemory2", (PFN_vkVoidFunction)layer_UnmapMemory2, FAKE_BC },
   { "vkUnmapMemory2KHR", (PFN_vkVoidFunction)layer_UnmapMemory2, FAKE_BC },
   { "vkAllocateMemory", (PFN_vkVoidFunction)layer_AllocateMemory, 0 },
   { "vkFreeMemory", (PFN_vkVoidFunction)layer_FreeMemory, 0 },
   { "vkCmdCopyBufferToImage", (PFN_vkVoidFunction)layer_CmdCopyBufferToImage, FAKE_BC },
   { "vkCmdCopyBufferToImage2", (PFN_vkVoidFunction)layer_CmdCopyBufferToImage2, FAKE_BC },
   { "vkCmdCopyBufferToImage2KHR", (PFN_vkVoidFunction)layer_CmdCopyBufferToImage2, FAKE_BC },
   { "vkAllocateCommandBuffers", (PFN_vkVoidFunction)layer_AllocateCommandBuffers, FAKE_BC },
   { "vkFreeCommandBuffers", (PFN_vkVoidFunction)layer_FreeCommandBuffers, FAKE_BC },
   { "vkResetCommandPool", (PFN_vkVoidFunction)layer_ResetCommandPool, FAKE_BC },
   { "vkDestroyCommandPool", (PFN_vkVoidFunction)layer_DestroyCommandPool, FAKE_BC },
   { "vkBeginCommandBuffer", (PFN_vkVoidFunction)layer_BeginCommandBuffer, FAKE_BC },
   { "vkResetCommandBuffer", (PFN_vkVoidFunction)layer_ResetCommandBuffer, FAKE_BC },
   { "vkQueueSubmit", (PFN_vkVoidFunction)layer_QueueSubmit, FAKE_BC },
   { "vkQueueSubmit2", (PFN_vkVoidFunction)layer_QueueSubmit2, FAKE_BC },
   { "vkQueueSubmit2KHR", (PFN_vkVoidFunction)layer_QueueSubmit2, FAKE_BC },
   { "vkCmdSetViewport", (PFN_vkVoidFunction)layer_CmdSetViewport, FAKE_MULTI_VIEWPORT },
   { "vkCmdSetScissor", (PFN_vkVoidFunction)layer_CmdSetScissor, FAKE_MULTI_VIEWPORT },
   { "vkCmdSetViewportWithCount", (PFN_vkVoidFunction)layer_CmdSetViewportWithCount, FAKE_MULTI_VIEWPORT },
   { "vkCmdSetViewportWithCountEXT", (PFN_vkVoidFunction)layer_CmdSetViewportWithCount, FAKE_MULTI_VIEWPORT },
   { "vkCmdSetScissorWithCount", (PFN_vkVoidFunction)layer_CmdSetScissorWithCount, FAKE_MULTI_VIEWPORT },
   { "vkCmdSetScissorWithCountEXT", (PFN_vkVoidFunction)layer_CmdSetScissorWithCount, FAKE_MULTI_VIEWPORT },
   { "vkCmdSetPolygonModeEXT", (PFN_vkVoidFunction)layer_CmdSetPolygonModeEXT, FAKE_FILL_MODE },
   { "vkCreateGraphicsPipelines", (PFN_vkVoidFunction)layer_CreateGraphicsPipelines,
     FAKE_FILL_MODE | FAKE_MULTI_VIEWPORT | FAKE_CLIP_DISTANCE | FAKE_CULL_DISTANCE },
   { "vkCreateShaderModule", (PFN_vkVoidFunction)layer_CreateShaderModule, FAKE_CLIP_DISTANCE | FAKE_CULL_DISTANCE },
};

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
venus_GetDeviceProcAddr(VkDevice device, const char *name)
{
   struct device_data *dev = device ? find_device(key_of(device)) : NULL;
   if (!dev) return NULL;
   for (size_t i = 0; i < ARRAY_LEN(device_entries); i++) {
      const struct entry *e = &device_entries[i];
      if (strcmp(name, e->name)) continue;
      const bool wanted = !e->needs || (e->needs == SEM ? dev->fake_semaphore_fd : (dev->fake & e->needs) != 0);
      PFN_vkVoidFunction next = dev->next_gdpa(device, name);
      /* Never hand out an entry point the driver below does not have. */
      if (wanted && (next || e->needs == SEM || !strcmp(name, "vkGetDeviceProcAddr"))) return e->fn;
      return next;
   }
   return dev->next_gdpa(device, name);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
venus_GetInstanceProcAddr(VkInstance instance, const char *name)
{
   static const struct { const char *name; PFN_vkVoidFunction fn; } own[] = {
      { "vkGetInstanceProcAddr", (PFN_vkVoidFunction)venus_GetInstanceProcAddr },
      { "vkCreateInstance", (PFN_vkVoidFunction)layer_CreateInstance },
      { "vkDestroyInstance", (PFN_vkVoidFunction)layer_DestroyInstance },
      { "vkCreateDevice", (PFN_vkVoidFunction)layer_CreateDevice },
      { "vkGetDeviceProcAddr", (PFN_vkVoidFunction)venus_GetDeviceProcAddr },
      { "vkEnumerateDeviceExtensionProperties", (PFN_vkVoidFunction)layer_EnumerateDeviceExtensionProperties },
      { "vkGetPhysicalDeviceProperties2", (PFN_vkVoidFunction)layer_GetPhysicalDeviceProperties2 },
      { "vkGetPhysicalDeviceProperties2KHR", (PFN_vkVoidFunction)layer_GetPhysicalDeviceProperties2 },
      { "vkGetPhysicalDeviceFeatures", (PFN_vkVoidFunction)layer_GetPhysicalDeviceFeatures },
      { "vkGetPhysicalDeviceFeatures2", (PFN_vkVoidFunction)layer_GetPhysicalDeviceFeatures2 },
      { "vkGetPhysicalDeviceFeatures2KHR", (PFN_vkVoidFunction)layer_GetPhysicalDeviceFeatures2 },
      { "vkGetPhysicalDeviceFormatProperties", (PFN_vkVoidFunction)layer_GetPhysicalDeviceFormatProperties },
      { "vkGetPhysicalDeviceFormatProperties2", (PFN_vkVoidFunction)layer_GetPhysicalDeviceFormatProperties2 },
      { "vkGetPhysicalDeviceFormatProperties2KHR", (PFN_vkVoidFunction)layer_GetPhysicalDeviceFormatProperties2 },
      { "vkGetPhysicalDeviceImageFormatProperties", (PFN_vkVoidFunction)layer_GetPhysicalDeviceImageFormatProperties },
      { "vkGetPhysicalDeviceImageFormatProperties2", (PFN_vkVoidFunction)layer_GetPhysicalDeviceImageFormatProperties2 },
      { "vkGetPhysicalDeviceImageFormatProperties2KHR", (PFN_vkVoidFunction)layer_GetPhysicalDeviceImageFormatProperties2 },
   };
   for (size_t i = 0; i < ARRAY_LEN(own); i++)
      if (!strcmp(name, own[i].name)) return own[i].fn;
   struct instance_data *in = instance ? find_instance(key_of(instance)) : NULL;
   return in ? in->next_gipa(instance, name) : NULL;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *iface)
{
   if (iface->loaderLayerInterfaceVersion > 2) iface->loaderLayerInterfaceVersion = 2;
   iface->pfnGetInstanceProcAddr = venus_GetInstanceProcAddr;
   iface->pfnGetDeviceProcAddr = venus_GetDeviceProcAddr;
   iface->pfnGetPhysicalDeviceProcAddr = NULL;
   return VK_SUCCESS;
}
