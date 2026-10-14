# Venus bridge (non-Adreno GPUs)

Turnip drives Adreno through Qualcomm's KGSL interface; no open driver can drive Mali (Pixel 6-9,
Pixel 10a), PowerVR (Pixel 10) or Xclipse from a glibc process. On those devices the session draws
with Mesa's Venus driver instead, which serialises every Vulkan call over a unix socket to
virglrenderer's vtest server. The server runs on the Android side under the app's uid and replays
the calls on the phone's own Vulkan driver (`app/.../gpu/VenusBridge.java` starts it per session).

```
guest (glibc, proot)                         Android (app uid)
gamescope / Steam / Zink / DXVK
   -> libvulkan_virtio.so (Mesa Venus)  --vtest socket-->  libvirgl_test_server.so
   -> VK_LAYER_DROIDDECK_venus_drm                          -> libvirgl_render_server.so
                                                            -> /vendor/.../vulkan.<gpu>.so
```

`build-virglrenderer.sh <out>` builds all of it from source (Gradle's `buildVenus` runs it;
`-PskipVenus=true` leaves it out): virglrenderer 1.3.0 with `patches/`, cross-compiled with the
NDK; Mesa 26.2.4's Venus driver (the runtime's Mesa series) with `mesa-patches/`, and the layer,
both built with the NDK's clang against a sysroot of Arch Linux ARM packages (the runtime's own
distribution and glibc). Every download is pinned by checksum.

## Patches (`patches/`, applied in order)

From Termux's `virglrenderer-android` package (xMeM's Venus-on-Android work, Termux's build fixes):

- `0001-do-not-use-gbm`, `0002-avoid-gnu-offsetof-extensions-error`, `0012-fix-build-on-arm`,
  `0013-disable-android-detection` - build fixes for Android.
- `0004-fallback-to-ashmem` - vtest's shared memory without memfd_create.
- `0011-venus-on-android-vulkan` - GPU memory as AHardwareBuffers, whose dma-buf the guest can map
  and share; every image linear; DRM modifiers answered as LINEAR.

This app's:

- `0100-ahb-allocation-for-dma-buf-branch` - 1.3.0's dma-buf allocation branch calls the gbm
  allocator by its old name; point it at 0011's.
- `0101-linear-modifier-and-dma-buf-export` - the LINEAR modifier reports what linear tiling can
  do (with no features, gamescope found no format to share); modifier queries become linear-tiling
  queries; dma-buf export is reported for the AHardwareBuffer path (the driver itself only imports
  dma-bufs); and structs taken out of a pNext chain go back where they were - Venus decodes the
  reply in the guest's chain order, and 0011 put them back at the head (Zink crashed in the decode).
- `0102-keep-android-libraries-loaded` - libvulkan and libandroid are never unloaded: Android's
  GPU stack leaves thread-exit destructors behind, and a worker thread exiting later jumped into
  unmapped code.
- `0103-destroy-object-mutex-after-objects` - an upstream lock-after-destroy in device teardown
  that glibc tolerates and bionic aborts on; it killed the render worker of every client that
  exited with objects alive.
- `0104-vtest-resource-import-fd` - vtest protocol 5: `VCMD_RESOURCE_IMPORT_FD` imports a dma-buf
  the client holds (another client's window, say) as a resource of its context. Upstream vtest has
  no import, so Venus left `create_from_dma_buf` NULL and any process handed another's GPU buffer
  called it: Xwayland's glamor at start, gamescope on a client's first frame.
- `0105-no-ahb-allocation-for-imports` - 0011's AHardwareBuffer path ran for imports too, put the
  import struct into the pNext chain twice and made it a cycle; the ring thread spun on it forever.

Mesa (`mesa-patches/`):

- `0001-vtest-dma-buf-import` - the client side of 0104: `create_from_dma_buf` over vtest when the
  server speaks protocol 5, and `has_dma_buf_import` with it.
- `0002-serialize-queue-submit-with-async-present` - Venus presents on its own thread, which submits
  on the application's queue; a submission only waited for the present in flight, so the next
  present could run alongside it. Both built their batches in the queue's one cached storage, and
  DXVK games crashed in `vkQueueSubmit2` on a sync pointer read from the other's arrays. Submissions
  now hold the queue's present lock throughout.

## The layer (`layer/venus_layer.c`, `VK_LAYER_DROIDDECK_venus`)

Loaded implicitly in Venus sessions; every call it does not name passes straight through.

- Names a render node for the Venus device (`VK_EXT_physical_device_drm`): the phone's GPU device
  bound in as `/dev/dri/renderD*` (`LinuxRuntime.bindGpuNode`), as KGSL is on Adreno. gamescope will
  not start without one.
- Stands in for `VK_KHR_external_semaphore_fd`, which Venus offers only over virtio-gpu. gamescope
  requires it for DRM syncobj timelines, which the stand-in node cannot back on any GPU here.
- Stands in for the six features DXVK requires and Mali lacks (it refused the adapter, so a D3D11
  game failed to create its DXGI factory):
  - `textureCompressionBC`: BC images are created uncompressed (RGBA8, R8, RG8 or RGBA16F), and an
    upload into one is redirected to a staging buffer that the layer fills at submit time by
    decoding the application's blocks on the CPU with `third_party/bcdec.h` (MIT / Unlicense).
    BC1 (UNORM) images no view can reinterpret are stored as R5G6B5 / A1R5G5B5, half the memory
    (`DROIDDECK_VENUS_BC_FULL=1` keeps them RGBA8).
  - `fillModeNonSolid` (wireframe draws fill), `multiViewport` (counts clamped to one),
    `shaderClipDistance` / `shaderCullDistance` (the builtins become Private variables in each
    shader; Mali's compiler crashed the server on them, Dark Souls Remastered loading a save), `robustBufferAccess2` (the
    driver's `robustBufferAccess` still bounds-checks).
  `DROIDDECK_VENUS_NO_FAKE=1` turns these off.
- Logs every swapchain's extent, format and image count.

## Known limits

- Every Vulkan call is a round trip through a unix socket; GPU memory is AHardwareBuffer-backed and
  every image is linear (0011), which costs bandwidth on Mali's tiled GPU.
- Mali has no BC texture compression, clip/cull distance, `fillModeNonSolid`, pipeline statistics
  or depth bounds, all of which DXVK's Direct3D feature levels expect.
