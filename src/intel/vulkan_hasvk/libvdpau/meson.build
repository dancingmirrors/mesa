# libvdpau - VDPAU wrapper library
#
# Original work Copyright © 2008-2010 NVIDIA Corporation
# Original work Copyright © 2008 Red Hat, Inc.
# See COPYING for license information
#
# This is integrated into Mesa to be built and installed alongside hasvk,
# providing VDPAU support that takes priority over system-installed versions.

# Configure library and module directories
# Install libvdpau directly in Mesa's libdir for higher priority
libvdpau_libdir = get_option('libdir')
libvdpau_moduledir = get_option('libdir') / 'vdpau'
libvdpau_sysconfdir = get_option('sysconfdir')

# Create configuration data
libvdpau_cdata = configuration_data()
libvdpau_cdata.set_quoted('VDPAU_MODULEDIR', get_option('prefix') / libvdpau_moduledir)
libvdpau_cdata.set_quoted('VDPAU_SYSCONFDIR', get_option('prefix') / libvdpau_sysconfdir)
libvdpau_cdata.set_quoted('VDPAU_DOC_PATH', meson.current_source_dir())
libvdpau_cdata.set_quoted('VDPAU_HEADER_PATH', meson.current_source_dir() / 'include')

# Check for secure_getenv
if cc.has_function('secure_getenv')
  libvdpau_cdata.set10('HAVE_SECURE_GETENV', true)
  libvdpau_cdata.set10('_GNU_SOURCE', true)
elif cc.has_function('__secure_getenv')
  libvdpau_cdata.set10('HAVE___SECURE_GETENV', true)
  libvdpau_cdata.set10('_GNU_SOURCE', true)
endif

# Check for DRI2 support (optional)
libvdpau_dri2_deps = [
  dependency('dri2proto', version : '>= 2.2', required : false),
  dependency('xext', required : false),
]

libvdpau_use_dri2 = true
foreach dep : libvdpau_dri2_deps
  libvdpau_use_dri2 = libvdpau_use_dri2 and dep.found()
endforeach
libvdpau_cdata.set10('DRI2', libvdpau_use_dri2)

# Generate config.h for libvdpau source files
libvdpau_config_h = configure_file(
  output : 'config.h',
  configuration : libvdpau_cdata
)

# Include directories
libvdpau_inc = include_directories('.', 'include')

# Check for required dependencies
dep_libvdpau_x11 = dependency('x11', required : true)
dep_libvdpau_dl = cc.find_library('dl', required : false)
dep_libvdpau_threads = dependency('threads', required : true)

# Build the VDPAU wrapper library
libvdpau_src = files('src/vdpau_wrapper.c')

if libvdpau_use_dri2
  libvdpau_src += files('src/mesa_dri2.c')
endif

# Build as a shared library
libvdpau_hasvk = shared_library(
  'vdpau',
  sources : [libvdpau_src, libvdpau_config_h],
  include_directories : libvdpau_inc,
  dependencies : [dep_libvdpau_x11, dep_libvdpau_dl, dep_libvdpau_threads, libvdpau_dri2_deps],
  c_args : ['-DHAVE_CONFIG_H=1'],
  version : '1.0.0',
  install : true,
  install_dir : libvdpau_libdir,
  # Use RPATH to ensure this library is found before system libraries
  install_rpath : '$ORIGIN',
)

# Install headers
install_headers(
  'include/vdpau/vdpau.h',
  'include/vdpau/vdpau_x11.h',
  subdir : 'vdpau',
)

# Install configuration file
install_data(
  'src/vdpau_wrapper.cfg',
  install_dir : libvdpau_sysconfdir,
)

# Install license file
install_data(
  'COPYING',
  install_dir : get_option('datadir') / 'licenses' / 'mesa-hasvk-libvdpau',
)

# Create a pkg-config file for the bundled libvdpau
# Note: pkg is already imported in main meson.build
pkg.generate(
  libvdpau_hasvk,
  name : 'vdpau',
  description : 'The Video Decode and Presentation API for UNIX (Mesa hasvk bundled version)',
  version : '1.5',
  variables : ['moduledir=' + get_option('prefix') / libvdpau_moduledir],
)

# Export dependency for use by libvdpau-va-gl
dep_hasvk_bundled_vdpau = declare_dependency(
  link_with : libvdpau_hasvk,
  include_directories : libvdpau_inc,
)

# Message to indicate successful configuration
message('Bundled libvdpau configured for hasvk (will take priority over system libvdpau)')
