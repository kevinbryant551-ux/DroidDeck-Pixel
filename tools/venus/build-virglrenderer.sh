#!/bin/bash
# Builds the Venus Vulkan bridge for GPUs Turnip cannot drive (Mali, PowerVR, Xclipse); see
# README.md here. Both ends are built from source, patched to match:
#
#   Android side  virglrenderer's vtest server with Venus, cross-compiled with the NDK, replaying
#                 the session's Vulkan calls on the phone's own driver (patches/).
#   guest side    Mesa's Venus driver (libvulkan_virtio.so) for the glibc runtime, with the vtest
#                 dma-buf import the server's protocol 5 adds (mesa-patches/); and the render-node
#                 layer (layer/). Built with the NDK's clang against Arch Linux ARM packages - the
#                 runtime's own distribution and glibc - so nothing links against a symbol the
#                 rootfs lacks.
#
#   tools/venus/build-virglrenderer.sh <out-dir>
#
# Output, laid out for Gradle (app/build.gradle's buildVenus):
#   <out-dir>/jniLibs/arm64-v8a/  libvirgl_test_server.so, libvirgl_render_server.so (executables
#                                 named so the installer extracts them to nativeLibraryDir),
#                                 libvirglrenderer.so, libepoxy.so
#   <out-dir>/assets/venus/       libvulkan_virtio.so, libVkLayer_droiddeck_venus.so, licences
#
# Host needs: curl, meson, ninja, pkg-config, python3 (mako, pyyaml and packaging are installed into
# a private virtualenv under the work directory when missing), bison, flex, wayland-scanner.
set -euo pipefail

VIRGL_VERSION=1.3.0
VIRGL_SHA256=56170f8caa1bb642a2624b649e3bcca095ec2834814e5c308efc8a85a709e4ce
EPOXY_VERSION=1.5.10
EPOXY_SHA256=a7ced37f4102b745ac86d6a70a9da399cc139ff168ba6b8002b4d8d43c900c15
# The runtime's Mesa series (26.2), which Termux also pairs with virglrenderer 1.3.0.
MESA_VERSION=26.2.4
MESA_SHA256=bce5f7fbebb934373b86c999a064d52fb5065878dc57f287f95346648ec832e9
# The guest sysroot: Arch Linux ARM packages, pinned. glibc is the runtime's exact build.
SYSROOT_PKGS=(
  "core/glibc-2.43+r22+g8362e8ce10b2-2-aarch64.pkg.tar.xz 8fac217e98c6e4342326726b2640ac254e8c82032f06f30bfa13ebbcc4fcb25b"
  "core/linux-api-headers-7.2-1-aarch64.pkg.tar.xz e03baabe6bbf49b65b394eb24629978bbb2094cd17b3fecd892a011041f9c24a"
  "core/gcc-16.1.1+r12+g301eb08fa2c5-1-aarch64.pkg.tar.xz 067a2aa64f5c7e626e3910fed6fd1f173a1fd9eb6bebaebdd7a7f5be58e170c4"
  "core/libgcc-16.1.1+r12+g301eb08fa2c5-1-aarch64.pkg.tar.xz 0f98bdf94a6e2f6758b9ffe98cbc9391a109326ed23bd264cabe0a81a04d7ae4"
  "core/libstdc++-16.1.1+r12+g301eb08fa2c5-1-aarch64.pkg.tar.xz 19832a38b2c4820695d28289f1c4f371955586d39fb893d8cbc0d8dbb09a4383"
  "core/zlib-1:1.3.2-3-aarch64.pkg.tar.xz 7e31b465e09e8e61375578b8e26ed883906e126e0c0ef1c75b7ccccfa95a58c4"
  "core/zstd-1.5.7-3-aarch64.pkg.tar.xz c3b907c36049b338bda0c6d85260251e48d17e2be6c0e11ddc0dfd4cf9d2e89a"
  "core/expat-2.9.0-1-aarch64.pkg.tar.xz c2bc1dd56d595b24f561e9514f0e82eaee1da5e7b1e0d46126868b9defe4e06a"
  "core/libffi-3.8.0-1-aarch64.pkg.tar.xz dfece459e053d53a5a7fdfbb2caf1b0fa662470220f86615a7789eba8571c0ff"
  "core/systemd-libs-262-1-aarch64.pkg.tar.xz 8e697c8f2db7c82dfcdcdc63540f7ad265d8e10fc7aab656c60229d858fca901"
  "extra/vulkan-headers-1:1.4.363.0-1-any.pkg.tar.xz ed90fb67e0e2c49f9871f5f2d7d62f9e41b58e058481400bf8da580225cde88f"
  "extra/libdrm-2.4.134-1-aarch64.pkg.tar.xz d4173d7adc60d32d389e44e976447962c31bc23ff9a2153539bd8ad766e307b8"
  "extra/wayland-1.26.0-1-aarch64.pkg.tar.xz 9382b38f97ee3841597f89aa6ca1fceea3d968ae759151eaab9c4b5373d1ce72"
  "extra/wayland-protocols-1.49-1-any.pkg.tar.xz 9678c0d371ecfb5041b1b1d8a7986977bd82b7233984dbe25b33a54da0dbee27"
  "extra/libx11-1.8.13-2-aarch64.pkg.tar.xz b391fdf1927ec6f8de2f3e5998c927df5fe1b13d9338e56ade48154af460a234"
  "extra/libxcb-1.17.0-1-aarch64.pkg.tar.xz 329a41249a09b4bb7f9b5f9694c1848c971188243cc1c0a479df4227800214c0"
  "extra/xorgproto-2026.1-1-any.pkg.tar.xz 8869e139c5f9fd2df006a01f45ec4d2173ef2f7f8e03d54bfcda9ab38370ae81"
  "extra/libxau-1.0.12-1-aarch64.pkg.tar.xz f9abaead8ca4f0d4f9a6e181c3fcad8f96ddb9cf1d75d1d82a81c0ca8ad08a50"
  "extra/libxdmcp-1.1.5-2-aarch64.pkg.tar.xz aaf3575b4139a0ee675cbc15c03324d09892ded2188ac7cc326a8b36ef308496"
  "extra/libxshmfence-1.3.3-1-aarch64.pkg.tar.xz d137d6cc13a75284267cd56beabfde6094fb1a4f8fb373af555c2e47c0e6b636"
  "extra/xcb-util-keysyms-0.4.1-5-aarch64.pkg.tar.xz 05bd273ab344ea9d8fd44d1688f8ac5aa009071672bbc8b50571c252c39dfe7a"
  "extra/libxrandr-1.5.5-1-aarch64.pkg.tar.xz 50ced1a5945e325ef28fd4a5f9ee3e45e49f538841271f49326916576213a5f3"
  "extra/libxext-1.3.7-1-aarch64.pkg.tar.xz bff987768ac6bb9ed809d49f3c171c93ccf0c7264c59ec7b0940e844ba4e53ac"
  "extra/libxrender-0.9.12-1-aarch64.pkg.tar.xz f5f344d116ff42b379c50ef595e7e9a3a6f7ed09357f8cd3acab90a3d11c9960"
  "extra/libxfixes-6.0.2-1-aarch64.pkg.tar.xz be025023ed05babf0372515e8346128f5dd7eccd82fd51be6c3c88eb8881d7a2"
  "extra/libxxf86vm-1.1.7-1-aarch64.pkg.tar.xz 9c07efd6fe6040a7e2d19af307c74d5f2235b6313e74710a29ecedb56f9a6ded"
  "extra/libdisplay-info-0.3.0-1-aarch64.pkg.tar.xz 73af5a8923b764610ed281c3d1529c73bfeb40dcb3a4015a1e36ef4fc1ba7e5d"
)
API=29
JOBS=${JOBS:-4}

out=$(realpath -m "${1:?usage: $0 <out-dir>}")
here=$(cd "$(dirname "$0")" && pwd)
ndk=${ANDROID_NDK_HOME:-${ANDROID_HOME:-$HOME/Android/Sdk}/ndk/27.3.13750724}
tc=$ndk/toolchains/llvm/prebuilt/linux-x86_64
work=${VENUS_WORK:-$here/../../app/build/venus}
mkdir -p "$work/downloads" "$out"
cd "$work"

fetch() { # url sha256 file
  [ -f "$3" ] && echo "$2  $3" | sha256sum -c --status && return
  curl -fsSL -o "$3" "$1"
  echo "$2  $3" | sha256sum -c --status || { echo "checksum mismatch: $3" >&2; exit 1; }
}
pkg_file() { local name; name=$(basename "$1"); echo "downloads/${name//:/_}"; }
fetch "https://gitlab.freedesktop.org/virgl/virglrenderer/-/archive/virglrenderer-$VIRGL_VERSION/virglrenderer-virglrenderer-$VIRGL_VERSION.tar.gz" $VIRGL_SHA256 downloads/virgl.tar.gz
fetch "https://github.com/anholt/libepoxy/archive/refs/tags/$EPOXY_VERSION.tar.gz" $EPOXY_SHA256 downloads/epoxy.tar.gz
fetch "https://archive.mesa3d.org/mesa-$MESA_VERSION.tar.xz" $MESA_SHA256 downloads/mesa.tar.xz
for entry in "${SYSROOT_PKGS[@]}"; do
  pkg=${entry% *} sum=${entry#* } url=${pkg//+/%2B}
  fetch "http://mirror.archlinuxarm.org/aarch64/${url//:/%3A}" "$sum" "$(pkg_file "$pkg")"
done

# Mesa's build scripts need mako, pyyaml and packaging; keep them out of the host's Python.
if ! python3 -c 'import mako, yaml, packaging' 2>/dev/null; then
  [ -x pyenv/bin/python ] || python3 -m venv pyenv
  pyenv/bin/python -c 'import mako, yaml, packaging' 2>/dev/null || pyenv/bin/pip install -q mako pyyaml packaging
  export PATH="$work/pyenv/bin:$PATH"
fi

libs=$out/jniLibs/arm64-v8a assets=$out/assets/venus
rm -rf src prefix build-epoxy build-virgl build-mesa sysroot "$out/jniLibs" "$out/assets"
mkdir -p src sysroot "$libs" "$assets"

# ---- Android side: virglrenderer ----
tar -xzf downloads/virgl.tar.gz -C src && tar -xzf downloads/epoxy.tar.gz -C src
virgl_src=src/virglrenderer-virglrenderer-$VIRGL_VERSION
for p in "$here"/patches/*.patch; do patch -d "$virgl_src" -p1 -s --no-backup-if-mismatch < "$p"; done

cat > cross-android.ini <<EOF
[binaries]
c = '$tc/bin/aarch64-linux-android$API-clang'
cpp = '$tc/bin/aarch64-linux-android$API-clang++'
ar = '$tc/bin/llvm-ar'
strip = '$tc/bin/llvm-strip'
pkg-config = 'pkg-config'
[built-in options]
c_args = ['-D__TERMUX__', '-Wno-error=gnu-offsetof-extensions']
c_link_args = ['-Wl,-z,max-page-size=16384']
[properties]
pkg_config_libdir = '$work/prefix/lib/pkgconfig'
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF

meson setup build-epoxy src/libepoxy-$EPOXY_VERSION --cross-file cross-android.ini --prefix="$work/prefix" \
  --libdir lib -Dbuildtype=release -Degl=yes -Dglx=no -Dx11=false -Dtests=false >/dev/null
ninja -C build-epoxy -j "$JOBS" install >/dev/null
meson setup build-virgl "$virgl_src" --cross-file cross-android.ini --prefix="$work/prefix" \
  --libdir lib -Dbuildtype=release -Dvenus=true -Dplatforms=egl >/dev/null
ninja -C build-virgl -j "$JOBS" install >/dev/null

install -m 755 prefix/bin/virgl_test_server "$libs/libvirgl_test_server.so"
install -m 755 prefix/libexec/virgl_render_server "$libs/libvirgl_render_server.so"
install -m 755 prefix/lib/libvirglrenderer.so prefix/lib/libepoxy.so "$libs/"
"$tc/bin/llvm-strip" "$libs"/*.so
install -m 644 "$virgl_src/COPYING" "$assets/COPYING-virglrenderer"

# ---- guest side: the sysroot, Mesa's Venus driver and the layer ----
for entry in "${SYSROOT_PKGS[@]}"; do
  tar -xJf "$(pkg_file "${entry% *}")" -C sysroot --exclude=.PKGINFO --exclude=.BUILDINFO \
    --exclude=.MTREE --exclude=.INSTALL --exclude='usr/bin/*' --exclude='usr/share/man/*' \
    --exclude='usr/share/doc/*' --exclude='usr/share/locale/*' 2>/dev/null || true
done
sysroot=$work/sysroot

tar -xJf downloads/mesa.tar.xz -C src
mesa_src=src/mesa-$MESA_VERSION
for p in "$here"/mesa-patches/*.patch; do patch -d "$mesa_src" -p1 -s --no-backup-if-mismatch < "$p"; done

cat > cross-guest.ini <<EOF
[binaries]
c = ['$tc/bin/clang', '--target=aarch64-unknown-linux-gnu', '--sysroot=$sysroot']
cpp = ['$tc/bin/clang++', '--target=aarch64-unknown-linux-gnu', '--sysroot=$sysroot']
ar = '$tc/bin/llvm-ar'
strip = '$tc/bin/llvm-strip'
pkg-config = 'pkg-config'
[built-in options]
c_args = ['-mno-outline-atomics']
cpp_args = ['-mno-outline-atomics']
c_link_args = ['-fuse-ld=lld', '-Wl,-z,max-page-size=16384']
cpp_link_args = ['-fuse-ld=lld', '-Wl,-z,max-page-size=16384']
[properties]
sys_root = '$sysroot'
pkg_config_libdir = '$sysroot/usr/lib/pkgconfig:$sysroot/usr/share/pkgconfig'
[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF

meson setup build-mesa "$mesa_src" --cross-file cross-guest.ini -Dbuildtype=release \
  -Dvulkan-drivers=virtio -Dgallium-drivers= -Dplatforms=x11,wayland -Dglx=disabled -Degl=disabled \
  -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Dvideo-codecs= \
  -Dvulkan-layers= -Dtools= -Dzstd=enabled -Dexpat=enabled -Dlibunwind=disabled -Dvalgrind=disabled \
  -Dlmsensors=disabled -Dbuild-tests=false >/dev/null
ninja -C build-mesa -j "$JOBS" src/virtio/vulkan/libvulkan_virtio.so >/dev/null
install -m 644 build-mesa/src/virtio/vulkan/libvulkan_virtio.so "$assets/"
"$tc/bin/llvm-strip" "$assets/libvulkan_virtio.so"
install -m 644 "$mesa_src/docs/license.rst" "$assets/COPYING-mesa"

"$tc/bin/clang" --target=aarch64-unknown-linux-gnu --sysroot="$sysroot" -fuse-ld=lld -shared -fPIC \
  -O2 -mno-outline-atomics -Wall -Wextra -Werror -Wno-unused-function -fvisibility=hidden -nostdlib -I"$here/layer" \
  -Wl,-soname,libVkLayer_droiddeck_venus.so -Wl,-z,max-page-size=16384 \
  -o "$assets/libVkLayer_droiddeck_venus.so" "$here/layer/venus_layer.c" -lc
"$tc/bin/llvm-strip" "$assets/libVkLayer_droiddeck_venus.so"
echo "venus bridge: $out"
