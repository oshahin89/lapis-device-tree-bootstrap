#!/bin/bash
# Mesa 25.0.7 for the lapis iOS: gallium panfrost (panthor backend, Mali-G615
# v11) behind OSMesa, over the kernel's LapisGPU (tools/mesa/lapis-drm as
# libdrm), plus tools/lapis_osmesatest linked against it. Runs on macOS with
# Xcode (CI: .github/workflows/lapis-mesa.yml); the cross set-up follows
# hakios's tools/mk-mesa.sh (MIT, the HakiOS authors).
#
#   tools/mesa/build-mesa-ios.sh [WORKDIR]       -> WORKDIR/out
set -euo pipefail
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
W=$(mkdir -p "${1:-$ROOT/build/mesa-ios}" && cd "${1:-$ROOT/build/mesa-ios}" && pwd)
cd "$W"

[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
echo "$SHA256  mesa-$VERSION.tar.xz" | shasum -a 256 -c -s
rm -rf mesa-$VERSION build out
tar xJf mesa-$VERSION.tar.xz
for p in "$ROOT"/tools/mesa/patches/*.patch; do
	patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p"
done
cp -r "$ROOT/tools/mesa/lapis-drm" mesa-$VERSION/src/lapis-drm

if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	venv/bin/pip -q install mako pyyaml packaging meson ninja
fi
export PATH=$W/venv/bin:$PATH

SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --sdk iphoneos -f clang)
CXX=$(xcrun --sdk iphoneos -f clang++)
cat > cross-ios.ini <<INI
[binaries]
c = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=16.0', '-D_DARWIN_C_SOURCE']
cpp = ['$CXX', '-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=16.0', '-D_DARWIN_C_SOURCE']
objc = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=16.0', '-D_DARWIN_C_SOURCE']
ar = '$(xcrun -f ar)'
strip = '$(xcrun -f strip)'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI

meson setup build mesa-$VERSION --cross-file cross-ios.ini --wrap-mode=nofallback \
	--prefix=/usr/local/lapis --libdir=lib --buildtype=release -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers=panfrost,softpipe -Dvulkan-drivers= -Dglx=disabled \
	-Degl=disabled -Dgbm=disabled -Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled \
	-Dgles2=enabled -Dopengl=true -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled \
	-Dvalgrind=disabled -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled \
	-Dgallium-va=disabled -Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false 2>&1 | tee meson-setup.log
if ! ninja -C build > ninja.log 2>&1; then
	grep -E "error:|FAILED|Undefined|referenced from" ninja.log | head -80
	tail -40 ninja.log
	exit 1
fi

mkdir -p out
cp build/src/gallium/targets/osmesa/libOSMesa*.dylib build/src/mapi/shared-glapi/libglapi*.dylib out/ 2>/dev/null || true
OSMESA=$(ls build/src/gallium/targets/osmesa/libOSMesa.*.dylib | head -1)
"$CC" -arch arm64 -isysroot "$SDK" -miphoneos-version-min=16.0 -O2 -Wall \
	-I mesa-$VERSION/include -o out/lapis_osmesatest "$ROOT/tools/lapis_osmesatest/lapis_osmesatest.c" \
	"$OSMESA" -Wl,-rpath,@executable_path -framework IOKit -framework CoreFoundation
for f in out/*; do codesign -s - -f "$f"; done
ls -la out
otool -L out/lapis_osmesatest out/libOSMesa*.dylib | head -40
shasum -a 256 out/* | tee out/SHA256.txt
