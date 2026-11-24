#!/bin/bash
# Mesa 26.3.1 for the lapis iOS: gallium panfrost (G615 / v11 needs Mesa
# >= 26; 25.0, the last without CL kernels, stops at v10) as liblapis_gallium
# over the kernel's LapisGPU (tools/mesa/lapis-drm as libdrm), plus
# tools/lapis_galliumtest linked against it. Runs on macOS with Xcode
# (CI: .github/workflows/lapis-mesa26.yml).
# Since 25.1 panfrost compiles CL kernels at build time: first the build
# machine's own mesa_clc / vtn_bindgen2 / panfrost_compile (with Homebrew's
# LLVM, libclc, SPIRV-LLVM-Translator), then the iOS cross build uses them.
#
#   tools/mesa/build-mesa26-ios.sh [WORKDIR]       -> WORKDIR/out
set -euo pipefail
VERSION=26.3.1
SHA256=4e4e438eac7b840a9383988cbb7c08dad8041da544b9319fc26d319fa7348961
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
W=$(mkdir -p "${1:-$ROOT/build/mesa26-ios}" && cd "${1:-$ROOT/build/mesa26-ios}" && pwd)
cd "$W"

[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
echo "$SHA256  mesa-$VERSION.tar.xz" | shasum -a 256 -c -s
rm -rf mesa-$VERSION native build out
tar xJf mesa-$VERSION.tar.xz
for p in "$ROOT"/tools/mesa/patches26/*.patch; do
	patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p"
done
cp -r "$ROOT/tools/mesa/lapis-drm" mesa-$VERSION/src/lapis-drm

if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	venv/bin/pip -q install mako pyyaml packaging meson ninja
fi
export PATH=$W/venv/bin:$PATH

COMMON=(-Dplatforms= -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled
	-Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dzstd=disabled -Dxmlconfig=disabled
	-Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled -Dlibunwind=disabled
	-Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-va=disabled -Dlmsensors=disabled
	-Dbuild-tests=false -Dgallium-drivers=panfrost)

echo "== 1: the build machine's CL tools"
LLVM=$(brew --prefix llvm)
PATH=$LLVM/bin:$PATH PKG_CONFIG_PATH=$(brew --prefix)/lib/pkgconfig:$LLVM/lib/pkgconfig \
	meson setup native mesa-$VERSION --buildtype=release "${COMMON[@]}" \
	-Dllvm=enabled -Dmesa-clc=enabled -Dprecomp-compiler=enabled > native-setup.log 2>&1 \
	|| { tail -60 native-setup.log; exit 1; }
if ! PATH=$LLVM/bin:$PATH ninja -C native src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 \
	src/panfrost/clc/panfrost_compile > native-ninja.log 2>&1; then
	grep -E "error:|FAILED" native-ninja.log | head -40; tail -30 native-ninja.log; exit 1
fi
mkdir -p tools
cp native/src/compiler/clc/mesa_clc native/src/compiler/spirv/vtn_bindgen2 native/src/panfrost/clc/panfrost_compile tools/
export PATH=$W/tools:$PATH

echo "== 2: iOS"
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
	--prefix=/usr/local/lapis --libdir=lib --buildtype=release -Db_ndebug=true "${COMMON[@]}" \
	-Dllvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system > meson-setup.log 2>&1 \
	|| { tail -60 meson-setup.log; exit 1; }
if ! ninja -C build src/gallium/targets/lapis/liblapis_gallium.dylib > ninja.log 2>&1; then
	grep -E "error:|FAILED|Undefined|referenced from" ninja.log | head -80
	tail -40 ninja.log
	exit 1
fi

mkdir -p out
LIB=build/src/gallium/targets/lapis/liblapis_gallium.dylib
cp "$LIB" out/
"$CC" -arch arm64 -isysroot "$SDK" -miphoneos-version-min=16.0 -O2 -Wall \
	-o out/lapis_galliumtest "$ROOT/tools/lapis_galliumtest/lapis_galliumtest.c" \
	"$LIB" -framework IOKit -framework CoreFoundation
# on the phone the library sits in the bbfw slot (tools/lapis_rootslot.sh)
install_name_tool -change @rpath/liblapis_gallium.dylib \
	"${LAPIS_GALLIUM_PATH:-/usr/standalone/update/ICE17.Release.bbfw}" out/lapis_galliumtest
for f in out/*; do codesign -s - -f "$f"; done
ls -la out
otool -L out/lapis_galliumtest out/liblapis_gallium.dylib | head -20
shasum -a 256 out/* | tee out/SHA256.txt
