#!/bin/sh

set -eu
set -o pipefail

: "${AGREE_CODEC_PATENTS:=0}"
if [ "$AGREE_CODEC_PATENTS" != "1" ]; then
  cat <<'EOF'
WARNING: This build enables some video codecs that are patent encumbered.
Do NOT run this script unless you understand and accept any licensing or
patent obligations that apply in your jurisdiction, particularly if you
plan to redistribute binaries.

To proceed, set:
AGREE_CODEC_PATENTS=1 ./setup-debian-78-32.sh
EOF
  exit 1
fi

# Configurable vars
CROSS_FILE="meson-cross-i386.ini"
BUILD_DIR="build-32"
PREFIX="/usr/local"
LIBDIR="lib32"
INCLUDEDIR="include32"
MESON_ARGS="-Dgallium-drivers=crocus -Dvulkan-drivers=intel_hasvk -Dvideo-codecs=all"

export CFLAGS="-m32 ${CFLAGS:-}"
export CXXFLAGS="-m32 ${CXXFLAGS:-}"
export LDFLAGS="-m32 ${LDFLAGS:-}"

export PKG_CONFIG_PATH="/usr/lib/i386-linux-gnu/pkgconfig:/usr/local/${LIBDIR}/pkgconfig:${PKG_CONFIG_PATH:-}"

#export PKG_CONFIG_LIBDIR="/usr/lib/i386-linux-gnu/pkgconfig:/usr/local/${LIBDIR}/pkgconfig"

command -v meson >/dev/null 2>&1 || { echo "meson not found; install meson and retry." >&2; exit 2; }
command -v ninja >/dev/null 2>&1 || { echo "ninja not found; install ninja and retry." >&2; exit 2; }

if command -v dpkg >/dev/null 2>&1; then
  if ! dpkg --print-foreign-architectures | grep -q '^i386$'; then
    echo "i386 multiarch support not sane."
  fi
fi

cat <<EOF

Using:
  cross file: $CROSS_FILE
  build dir : $BUILD_DIR
  prefix    : $PREFIX
  libdir    : $LIBDIR
  includedir: $INCLUDEDIR

Environment:
  PKG_CONFIG_PATH: $PKG_CONFIG_PATH
  PKG_CONFIG_LIBDIR: ${PKG_CONFIG_LIBDIR:-"(unset)"}
  CFLAGS: $CFLAGS
  CXXFLAGS: $CXXFLAGS
  LDFLAGS: $LDFLAGS

Meson command about to run:
  meson setup --cross-file=$CROSS_FILE --prefix $PREFIX --libdir $LIBDIR --includedir $INCLUDEDIR --wipe $BUILD_DIR $MESON_ARGS

EOF

meson setup --cross-file="$CROSS_FILE" --prefix "$PREFIX" --libdir "$LIBDIR" --includedir "$INCLUDEDIR" --wipe $BUILD_DIR $MESON_ARGS
echo "If setup succeeded you can run meson compile -C $BUILD_DIR now."
