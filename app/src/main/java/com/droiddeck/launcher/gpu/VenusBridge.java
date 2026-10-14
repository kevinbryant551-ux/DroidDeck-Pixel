package com.droiddeck.launcher.gpu;

import android.content.Context;
import android.os.Process;
import android.util.Log;

import com.droiddeck.launcher.core.DeviceSupport;
import com.droiddeck.launcher.core.FileUtils;
import com.droiddeck.launcher.core.HostProcess;
import com.droiddeck.launcher.core.SessionPart;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

/**
 * Vulkan for the session on a GPU Turnip cannot drive - Mali (Pixel 6-9), PowerVR (Pixel 10),
 * Xclipse. Turnip talks to Qualcomm's KGSL directly, and no open driver can drive those GPUs from a
 * glibc process, so on them the session would have no Vulkan device at all: no gamescope, no
 * client, no game.
 *
 * <p>Instead the session draws with Mesa's Venus driver ({@code libvulkan_virtio.so}), which
 * serialises every Vulkan call over a unix socket, and this part runs virglrenderer's vtest server
 * out here under the app's uid, where it replays them on the phone's own Vulkan driver. GPU memory
 * is allocated as AHardwareBuffers, whose dma-buf the guest can map and share with gamescope and
 * the app's compositor (tools/venus has the build and the Android patches).
 *
 * <p>Both ends ship in the apk: the server as executables in the native library directory (the
 * only place Android runs them from), the guest's driver as an asset unpacked under files/, which
 * the session binds at its own path so the ICD manifest's absolute path is valid inside it.
 */
public final class VenusBridge extends SessionPart {
    private static final String TAG = "VenusBridge";
    public static final String SERVER = "libvirgl_test_server.so";
    public static final String RENDER_SERVER = "libvirgl_render_server.so";
    private static final String ASSET_DIR = "venus";
    private static final String DRIVER = "libvulkan_virtio.so";
    private static final String ICD = "virtio_icd.json";
    /** What the session needs that Venus on the phone's driver lacks (tools/venus/layer). */
    private static final String LAYER = "libVkLayer_droiddeck_venus.so";
    private static final String LAYER_DIR = "layers";
    private static final String SOCKET = "vtest.sock";
    /** Present in Downloads: use Turnip as on an Adreno even here (for testing the old path). */
    public static final String OFF_SWITCH = "Download/droiddeck-no-venus";

    private final File dir;
    private File logFile;
    private volatile int pid = -1;

    public VenusBridge(Context context) {
        this.dir = new File(context.getFilesDir(), "venus");
    }

    /** True where the session needs the bridge: no KGSL, the server in the apk, and not switched off. */
    public static boolean wanted(Context context) {
        if (DeviceSupport.INSTANCE.adreno()) return false;
        if (new File(android.os.Environment.getExternalStorageDirectory(), OFF_SWITCH).exists()) return false;
        return new File(context.getApplicationInfo().nativeLibraryDir, SERVER).isFile();
    }

    public void setLogFile(File file) {
        this.logFile = file;
    }

    public File socket() {
        return new File(dir, SOCKET);
    }

    /**
     * Unpacks the guest's driver and the render-node layer (again only when the apk's copies
     * differ) and writes their manifests. Returns the driver's manifest, or null when the apk
     * carries no driver.
     */
    public File prepareGuestDriver(Context context) {
        File manifest = new File(dir, ICD);
        try {
            if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot create " + dir);
            File driver = unpack(context, DRIVER, dir);
            // api_version is what the loader reports before the driver answers; the driver's own
            // (26.2's) is the real ceiling, and Venus caps it further at what the phone offers.
            write(manifest, "{\"file_format_version\":\"1.0.1\",\"ICD\":{\"library_path\":\""
                    + driver.getAbsolutePath() + "\",\"api_version\":\"1.4.0\"}}\n");
            File layers = new File(dir, LAYER_DIR);
            if (!layers.isDirectory() && !layers.mkdirs()) throw new IOException("cannot create " + layers);
            File layer = unpack(context, LAYER, dir);
            //noinspection ResultOfMethodCallIgnored
            new File(layers, "droiddeck_venus_drm.json").delete();
            write(new File(layers, "droiddeck_venus.json"), "{\"file_format_version\":\"1.2.0\",\"layer\":{"
                    + "\"name\":\"VK_LAYER_DROIDDECK_venus\",\"type\":\"GLOBAL\","
                    + "\"library_path\":\"" + layer.getAbsolutePath() + "\","
                    + "\"api_version\":\"1.4.0\",\"implementation_version\":\"1\","
                    + "\"description\":\"What the session needs that Venus on this GPU lacks\","
                    + "\"functions\":{\"vkGetInstanceProcAddr\":\"venus_GetInstanceProcAddr\","
                    + "\"vkGetDeviceProcAddr\":\"venus_GetDeviceProcAddr\"},"
                    + "\"disable_environment\":{\"DROIDDECK_VENUS_LAYER_OFF\":\"1\"}}}\n");
            return manifest;
        } catch (IOException e) {
            Log.e(TAG, "guest driver unavailable", e);
            return null;
        }
    }

    /** Copies an asset from venus/ into {@code into}, unless an identical copy is already there. */
    private static File unpack(Context context, String name, File into) throws IOException {
        File target = new File(into, name);
        byte[] packed = readAsset(context, ASSET_DIR + "/" + name);
        if (target.isFile() && target.length() == packed.length
                && java.util.Arrays.equals(Files.readAllBytes(target.toPath()), packed)) return target;
        File tmp = new File(into, name + ".tmp");
        try (OutputStream out = new FileOutputStream(tmp)) {
            out.write(packed);
        }
        if (!tmp.renameTo(target)) throw new IOException("cannot replace " + target);
        Log.i(TAG, "unpacked " + target + " (" + packed.length + " bytes)");
        return target;
    }

    private static void write(File file, String text) throws IOException {
        Files.write(file.toPath(), text.getBytes(StandardCharsets.UTF_8));
    }

    /** The guest's environment for the bridge: its driver, and where the server listens. */
    public List<String> guestEnvironment(File manifest) {
        List<String> env = new ArrayList<>();
        env.add("VK_ICD_FILENAMES=" + manifest.getAbsolutePath());
        env.add("VK_DRIVER_FILES=" + manifest.getAbsolutePath());
        // Venus's vtest transport, in place of the virtio-gpu device it would look for in a VM.
        env.add("VN_DEBUG=vtest");
        env.add("VTEST_SOCKET_NAME=" + socket().getAbsolutePath());
        // Windows hand gamescope GPU buffers (dma-bufs) as on Adreno: the bridge's protocol 5 imports
        // them (tools/venus/mesa-patches). Upstream vtest cannot, which forced shared-memory
        // presentation, and gamescope uploads a shared-memory buffer only once - the same few frames
        // cycled on screen.
        // The session preload's Venus fixes (tools/linuxfs/preload/venus.c) key on this.
        env.add("DROIDDECK_VENUS=1");
        // gamescope wants a render node named by the device; the session's is the phone's GPU
        // device standing in as one (LinuxRuntime.bindGpuNode), and the layer names it.
        String node = com.droiddeck.launcher.runtime.LinuxRuntime.gpuNodeNumbers();
        if (node != null) {
            env.add("VK_ADD_IMPLICIT_LAYER_PATH=" + new File(dir, LAYER_DIR).getAbsolutePath());
            env.add("DROIDDECK_VENUS_DRM=" + node);
        }
        return env;
    }

    @Override
    public void start() {
        stop();
        String libDir = app().getApplicationInfo().nativeLibraryDir;
        File binary = new File(libDir, SERVER);
        if (!binary.isFile()) {
            Log.w(TAG, "server missing at " + binary + "; the session will have no Vulkan device");
            return;
        }
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        // A socket left by a session that did not end cleanly would refuse the bind.
        //noinspection ResultOfMethodCallIgnored
        socket().delete();
        // Every client (gamescope, the client's GL through Zink, each game) gets its own forked
        // server and render-server worker; --no-virgl, since only Venus is used and the GL renderer
        // would want an EGL display it does not need.
        String command = binary.getAbsolutePath().replace(" ", "\\ ")
                + " --venus --no-virgl --socket-path " + socket().getAbsolutePath().replace(" ", "\\ ");
        String[] env = {
                "LD_LIBRARY_PATH=" + libDir,
                "RENDER_SERVER_EXEC_PATH=" + new File(libDir, RENDER_SERVER).getAbsolutePath(),
                "HOME=" + app().getFilesDir(),
        };
        PrintWriter log = null;
        if (logFile != null) {
            try {
                log = new PrintWriter(new java.io.FileWriter(logFile, true));
                log.println("== venus server starting: " + command);
                log.flush();
            } catch (IOException e) {
                Log.w(TAG, "could not open " + logFile, e);
            }
        }
        final PrintWriter out = log;
        pid = HostProcess.start(command, env, app().getFilesDir(), status -> {
            Log.i(TAG, "server exited: " + status);
            if (out != null) synchronized (out) { out.println("== venus server exited: " + status); out.flush(); }
        }, line -> {
            Log.i(TAG, line);
            if (out != null) synchronized (out) { out.println(line); out.flush(); }
        });
        // The guest connects as soon as it starts; give the server its socket first.
        long deadline = System.currentTimeMillis() + 2000L;
        while (pid > 0 && !socket().exists() && System.currentTimeMillis() < deadline) {
            try {
                Thread.sleep(20L);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }
        }
        Log.i(TAG, "started pid=" + pid + " socket=" + socket() + (socket().exists() ? "" : " (not listening yet)"));
    }

    @Override
    public void stop() {
        if (pid != -1) {
            // The forked per-client servers and their render-server workers are its children.
            for (int child : childrenOf(pid)) Process.killProcess(child);
            Process.killProcess(pid);
            pid = -1;
        }
    }

    // Not frozen with the session (SessionPart.suspendPid's default). Venus clients watch the
    // server's ring-alive status and abort after ~3 s without it; a game thawed a moment before
    // the server read the frozen server as dead and exited when the phone was unlocked. The server
    // idles while its clients are stopped.

    /** Every descendant of {@code root}, from /proc: the app may read its own processes' stat. */
    private static List<Integer> childrenOf(int root) {
        List<Integer> found = new ArrayList<>();
        List<Integer> parents = new ArrayList<>();
        parents.add(root);
        File[] procs = new File("/proc").listFiles();
        if (procs == null) return found;
        boolean grew = true;
        while (grew) {
            grew = false;
            for (File p : procs) {
                int id;
                try {
                    id = Integer.parseInt(p.getName());
                } catch (NumberFormatException e) {
                    continue;
                }
                if (found.contains(id) || id == root) continue;
                String stat = FileUtils.readString(new File(p, "stat"));
                if (stat == null) continue;
                int close = stat.lastIndexOf(')');
                if (close < 0) continue;
                String[] fields = stat.substring(close + 2).split(" ");
                if (fields.length < 2) continue;
                int ppid;
                try {
                    ppid = Integer.parseInt(fields[1]);
                } catch (NumberFormatException e) {
                    continue;
                }
                if (parents.contains(ppid)) {
                    found.add(id);
                    parents.add(id);
                    grew = true;
                }
            }
        }
        return found;
    }

    private static byte[] readAsset(Context context, String name) throws IOException {
        try (InputStream in = context.getAssets().open(name)) {
            java.io.ByteArrayOutputStream buf = new java.io.ByteArrayOutputStream();
            byte[] chunk = new byte[1 << 16];
            int n;
            while ((n = in.read(chunk)) > 0) buf.write(chunk, 0, n);
            return buf.toByteArray();
        }
    }
}
