#!/bin/sh

MESA_BUILD_DIR=$PWD
MESA_LIB_DIRS=$(find "$MESA_BUILD_DIR" -name '*.so*' -exec dirname {} \; | sort -u | paste -sd: -)

export LD_LIBRARY_PATH=$MESA_LIB_DIRS:$LD_LIBRARY_PATH
export LIBGL_DRIVERS_PATH=$MESA_BUILD_DIR/lib64/dri:$MESA_BUILD_DIR/lib/dri
export VK_ICD_FILENAMES=$MESA_BUILD_DIR/build/src/intel/vulkan_hasvk/intel_hasvk_devenv_icd.x86_64.json
export VK_ICD_FILENAMES=$VK_ICD_FILENAMES:$MESA_BUILD_DIR/build-32/src/intel/vulkan_hasvk/intel_hasvk_devenv_icd.i686.json

echo "LD_LIBRARY_PATH set to: $LD_LIBRARY_PATH"
echo "VK_ICD_FILENAMES set to: $VK_ICD_FILENAMES"

export INTEL_DEBUG=hasvk
export VDPAU_LOG=1
export VDPAU_TRACE=1

exec "$@"
