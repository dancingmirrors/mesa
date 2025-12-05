# libvdpau-va-gl - VDPAU driver with VA-API/OpenGL backend
#
# Original work Copyright 2013-2014 Rinat Ibragimov
# libvdpau-va-gl is distributed under the terms of the LGPLv3. See COPYING.LGPLv3.
#
# This is integrated into Mesa to be built and installed alongside hasvk,
# providing VDPAU support for Gen7/7.5/8 Intel hardware without requiring
# a separate installation of libvdpau-va-gl.

# Check for required dependencies
dep_glib = dependency('glib-2.0', required : false)
dep_libva_x11 = dependency('libva-x11', required : false)
dep_gl = dependency('gl', required : false)
dep_glu = dependency('glu', required : false)
dep_x11 = dependency('x11', required : false)

with_libvdpau_va_gl = (
  dep_glib.found() and
  dep_libva_x11.found() and
  dep_gl.found() and
  dep_x11.found()
)

if with_libvdpau_va_gl
  message('Building libvdpau-va-gl VDPAU driver')

  # Build the shader bundle tool
  shader_bundle_tool = executable(
    'shader-bundle-tool',
    'glsl/shader-bundle-tool.c',
    native : true,
  )

  # Generate shaders.c and shaders.h from GLSL files
  shader_files = files(
    'glsl/NV12_RGBA.glsl',
    'glsl/YV12_RGBA.glsl',
    'glsl/red_to_alpha_swizzle.glsl',
  )

  shaders_ch = custom_target(
    'vdpau_va_gl_shaders',
    output : ['shaders.h', 'shaders.c'],
    input : shader_files,
    command : [shader_bundle_tool, '@OUTPUT0@', '@OUTPUT1@', '@INPUT@'],
  )

  # Source files for libvdpau_va_gl
  libvdpau_va_gl_sources = files(
    'api-bitmap-surface.c',
    'api-csc-matrix.c',
    'api-device.c',
    'api-output-surface.c',
    'api-presentation-queue.c',
    'api-video-decoder.c',
    'api-video-mixer.c',
    'api-video-surface.c',
    'entry.c',
    'trace.c',
    'reverse-constant.c',
    'handle-storage.c',
    'bitstream.c',
    'h264-parse.c',
    'globals.c',
    'watermark.c',
    'ctx-stack.c',
  )

  libvdpau_va_gl_deps = [
    dep_glib,
    dep_libva_x11,
    dep_gl,
    dep_x11,
    dep_thread,
    dep_hasvk_bundled_vdpau,  # Link against bundled libvdpau (higher priority)
  ]

  if dep_glu.found()
    libvdpau_va_gl_deps += dep_glu
  endif

  # Build the shared library - installed to $libdir/vdpau/
  libvdpau_va_gl = shared_library(
    'vdpau_va_gl',
    [libvdpau_va_gl_sources, shaders_ch],
    include_directories : include_directories('.'),
    dependencies : libvdpau_va_gl_deps,
    c_args : ['-fvisibility=hidden', '-std=gnu99'],
    link_args : ['-lrt'],
    version : '1',
    install : true,
    install_dir : get_option('libdir') / 'vdpau',
    # Use RPATH to find bundled libvdpau in parent directory
    install_rpath : '$ORIGIN/..',
  )

  # Create a symlink in $libdir pointing to the vdpau subdir
  # This allows VDPAU to find the driver without needing VDPAU_DRIVER_PATH
  # $libdir/libvdpau_va_gl.so -> vdpau/libvdpau_va_gl.so
  install_symlink(
    'libvdpau_va_gl.so',
    install_dir : get_option('libdir'),
    pointing_to : 'vdpau' / 'libvdpau_va_gl.so',
  )

else
  message('Skipping libvdpau-va-gl (missing dependencies: glib-2.0, libva-x11, gl, x11)')
endif
