/*
 * Session fixes for the Venus bridge (VenusBridge: Mali, PowerVR), keyed on DROIDDECK_VENUS=1.
 *
 * Valve's steamwebhelper.sh exports LIBGL_KOPPER_DISABLE=true before starting the web helper, which
 * sends Zink's GLX through DRI3 instead of its own Vulkan presentation (kopper). DRI3 needs
 * Xwayland's glamor, and under Venus glamor is off (vtest cannot import a dma-buf), so the helper's
 * output window got no GL context ("CreateOutputWindow: failed to acquire a gl context") and the
 * client restarted it forever. Kopper presents through the Vulkan WSI, which works there; the
 * variable is dropped before Mesa can read it.
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>

__attribute__((constructor)) static void venus_session_env(void) {
  const char *venus = getenv("DROIDDECK_VENUS");
  if (!venus || strcmp(venus, "1") != 0) {
    return;
  }
  unsetenv("LIBGL_KOPPER_DISABLE");
}
