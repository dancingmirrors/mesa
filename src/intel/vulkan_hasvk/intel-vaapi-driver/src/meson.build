# Version information
intel_vaapi_driver_version = '2.4.0.1-hasvk'
intel_vaapi_driver_major_version = '2'
intel_vaapi_driver_minor_version = '4'
intel_vaapi_driver_micro_version = '0'
intel_vaapi_driver_pre_version = '1'

# Use bundled libva if available, otherwise fall back to system libva
# The bundled libva_hasvk_dep is provided by the libva subdirectory
if meson.is_subproject() == false or not is_variable('libva_hasvk_dep')
  # Fallback to system libva if bundled version is not available
  dep_libva = dependency('libva', version : '>= 1.1.0', required : true)
  va_api_major_version = '0'
  va_api_minor_version = '33'
  if dep_libva.type_name() == 'pkgconfig'
    va_api_version_array = dep_libva.version().split('.')
    va_api_major_version = va_api_version_array[0]
    va_api_minor_version = va_api_version_array[1]
  endif
else
  # Use bundled libva (preferred)
  dep_libva = libva_hasvk_dep
  # Use version from bundled libva
  va_api_major_version = '1'
  va_api_minor_version = '23'
endif

dep_libdrm = dependency('libdrm', version : '>= 2.4.52', required : true)
dep_libdrm_intel = dependency('libdrm_intel', required : true)

va_driver_init_func = '__vaDriverInit_@0@_@1@'.format(va_api_major_version,
                                                       va_api_minor_version)

# Determine installation directory
# libva searches $LIBDIR/dri for i965_drv_video.so
driverdir = get_option('prefix') / get_option('libdir') / 'dri'

# Configuration
config_cfg = configuration_data()
config_cfg.set('VERSION', intel_vaapi_driver_version)
config_cfg.set('VA_DRIVER_INIT_FUNC', va_driver_init_func)
config_cfg.set('INTEL_DRIVER_MAJOR_VERSION', intel_vaapi_driver_major_version)
config_cfg.set('INTEL_DRIVER_MINOR_VERSION', intel_vaapi_driver_minor_version)
config_cfg.set('INTEL_DRIVER_MICRO_VERSION', intel_vaapi_driver_micro_version)
config_cfg.set('INTEL_DRIVER_PRE_VERSION', intel_vaapi_driver_pre_version)
config_cfg.set10('HAVE_HYBRID_CODEC', false)

cc = meson.get_compiler('c')
if cc.has_function('log2f')
  config_cfg.set('HAVE_LOG2F', 1)
endif

config_file = configure_file(
  output : 'config.h',
  configuration : config_cfg)

# Version file
version_cfg = configuration_data()
version_cfg.set('INTEL_DRIVER_GIT_VERSION', intel_vaapi_driver_version)

version_file = configure_file(
  input : 'intel_version.h.in',
  output : 'intel_version.h',
  configuration : version_cfg)

# Source files
sources = files(
  'dso_utils.c',
  'gen6_mfc.c',
  'gen6_mfc_common.c',
  'gen6_mfd.c',
  'gen6_vme.c',
  'gen7_vme.c',
  'gen7_mfc.c',
  'gen7_mfd.c',
  'gen75_mfd.c',
  'gen75_mfc.c',
  'gen8_encoder_vp8.c',
  'gen8_mfc.c',
  'gen8_mfd.c',
  'gen8_vme.c',
  'gen9_encoder_vp8.c',
  'gen9_vme.c',
  'gen9_mfc.c',
  'gen9_mfc_hevc.c',
  'gen9_mfd.c',
  'gen9_vdenc.c',
  'gen75_picture_process.c',
  'gen75_vme.c',
  'gen75_vpp_gpe.c',
  'gen75_vpp_vebox.c',
  'gen9_post_processing.c',
  'i965_avc_bsd.c',
  'i965_avc_hw_scoreboard.c',
  'i965_avc_ildb.c',
  'i965_decoder_utils.c',
  'i965_device_info.c',
  'i965_drv_video.c',
  'i965_encoder.c',
  'i965_encoder_utils.c',
  'i965_encoder_vp8.c',
  'i965_media.c',
  'i965_media_h264.c',
  'i965_media_mpeg2.c',
  'i965_gpe_utils.c',
  'i965_post_processing.c',
  'i965_yuv_coefs.c',
  'gen8_post_processing.c',
  'i965_render.c',
  'i965_vpp_avs.c',
  'gen8_render.c',
  'gen9_render.c',
  'intel_batchbuffer.c',
  'intel_batchbuffer_dump.c',
  'intel_driver.c',
  'intel_memman.c',
  'object_heap.c',
  'intel_media_common.c',
  'vp8_probs.c',
  'vp9_probs.c',
  'vpx_quant.c',
  'gen9_vp9_encoder_kernels.c',
  'gen9_vp9_const_def.c',
  'gen9_vp9_encoder.c',
  'intel_common_vpp_internal.c',
  'i965_encoder_const_def.c',
  'i965_avc_const_def.c',
  'i965_avc_encoder_kernels.c',
  'i965_avc_encoder_common.c',
  'i965_avc_encoder.c',
  'gen9_hevc_enc_kernels_binary.c',
  'gen9_hevc_encoder.c',
  'gen9_hevc_enc_utils.c',
  'gen10_encoder_vp8.c',
  'gen10_hcp_common.c',
  'gen10_hevc_enc_kernels_binary.c',
  'gen10_hevc_enc_common.c',
  'gen10_hevc_encoder.c',
  'gen10_huc_common.c',
  'gen10_vdenc_common.c',
  'gen10_vdenc_vp9.c',
)

# Compiler flags
cflags = [
  '-DVA_DRIVERS_PATH="' + driverdir + '"',
  '-DHAVE_CONFIG_H',
  '-DPTHREADS',
  # Suppress warnings for bundled code
  '-Wno-unused-parameter',
  '-Wno-unused-variable',
  '-Wno-sign-compare',
  '-Wno-pointer-sign',
  '-Wno-missing-field-initializers',
  '-Wno-missing-prototypes',
]

# Dependencies
dep_dl = cc.find_library('dl', required : true)
dep_m = cc.find_library('m', required : false)
dep_threads = dependency('threads', required : true)

shared_deps = [
  dep_dl,
  dep_m,
  dep_threads,
  dep_libdrm,
  dep_libdrm_intel,
  dep_libva,
]

# Build the static library first
libi965_drv_video_static = static_library(
  'i965_drv_video_static',
  sources,
  config_file,
  version_file,
  c_args : cflags,
  include_directories : include_directories('.'),
  dependencies : shared_deps,
  gnu_symbol_visibility : 'hidden',
)

# Build the shared module (VA-API driver)
i965_drv_video = shared_module(
  'i965_drv_video',
  name_prefix : '',
  link_whole : libi965_drv_video_static,
  dependencies : shared_deps,
  install : true,
  install_dir : driverdir,
  install_tag : 'runtime',
)
