# libva bundled for Mesa hasvk
# This is a fork of libva that prevents probing for intel-media-driver
# first on systems where it wouldn't work (e.g., hasvk's hardware).
#
# This bundled version takes priority over system-installed libva.

# VA-API version from the fork
va_api_major_version = 1
va_api_minor_version = 23
va_api_micro_version = 0

va_api_version = '@0@.@1@.@2@'.format(va_api_major_version,
				      va_api_minor_version,
				      va_api_micro_version)

# libva version from the fork
libva_major_version = 2
libva_minor_version = 23
libva_micro_version = 0
libva_version = '@0@.@1@.@2@.1'.format(libva_major_version,
				     libva_minor_version,
				     libva_micro_version)

# Library version number
libva_interface_bias = va_api_major_version + 1
libva_interface_age = 0
libva_binary_age = 100 * va_api_minor_version + va_api_micro_version - libva_interface_age

libva_lt_current = 100 * va_api_minor_version + va_api_micro_version + libva_interface_bias
libva_lt_revision = libva_interface_age
libva_lt_age = libva_binary_age - libva_interface_age

libva_lt_current = libva_lt_current - libva_lt_age

libva_lt_version = '@0@.@1@.@2@'.format(libva_lt_current,
					libva_lt_age,
					libva_lt_revision)

# Installation directories
# Install in Mesa's libdir to take priority over system libva
sysconfdir = get_option('prefix') / get_option('sysconfdir')

# Set driverdir to the same location as intel-vaapi-driver
# This ensures libva searches for i965_drv_video.so in the correct location
driverdir = get_option('prefix') / get_option('libdir') / 'dri'

# Determine architecture suffix for multilib header installation
# The libva headers contain architecture-specific types (uintptr_t, unsigned long)
# that differ between 32-bit and 64-bit builds. To support multilib installations,
# we install architecture-specific headers with a suffix (e.g., va-32.h, va-64.h)
# and provide architecture-neutral wrapper headers that include the correct version.
pointer_size = cc.sizeof('void*')
if pointer_size == 8
  arch_suffix = '-64'
elif pointer_size == 4
  arch_suffix = '-32'
else
  error('Unsupported pointer size: @0@'.format(pointer_size))
endif

message('Building libva for @0@-bit architecture (arch_suffix=@1@)'.format(pointer_size * 8, arch_suffix))

configinc = include_directories('.')

# Find dependencies
libva_dl_dep = cc.find_library('dl', required : false)

# DRM support (required for hasvk)
WITH_DRM = true
dep_libdrm_for_libva = dependency('libdrm', version : '>= 2.4.75', required : true)

# X11 support (needed for hasvk)
WITH_X11 = false
if with_platform_x11
  x11_dep_for_libva = dependency('x11', required : false)
  xext_dep_for_libva = dependency('xext', required : false)
  xfixes_dep_for_libva = dependency('xfixes', required : false)
  
  WITH_X11 = (x11_dep_for_libva.found() and xext_dep_for_libva.found() and xfixes_dep_for_libva.found())
  
  if WITH_X11
    x11_xcb_dep_for_libva = dependency('x11-xcb', required : false)
    xcb_dep_for_libva = dependency('xcb', required : false)
    xcb_dri3_dep_for_libva = dependency('xcb-dri3', required : false)
    WITH_X11 = (x11_xcb_dep_for_libva.found() and xcb_dep_for_libva.found() and xcb_dri3_dep_for_libva.found())
  endif
endif

# GLX support (optional)
WITH_GLX = false
if WITH_X11
  gl_dep_for_libva = dependency('gl', required : false)
  WITH_GLX = gl_dep_for_libva.found()
endif

# Wayland support (optional)
WITH_WAYLAND = false
if with_platform_wayland
  wayland_dep_for_libva = dependency('wayland-client', version : '>= 1.11.0', required : false)
  wayland_scanner_dep_for_libva = dependency('wayland-scanner', version : '>= 1.15',
                                              required : false, native : true)
  if wayland_scanner_dep_for_libva.found()
    wl_scanner_for_libva = find_program(wayland_scanner_dep_for_libva.get_variable(pkgconfig: 'wayland_scanner'))
  endif
  WITH_WAYLAND = wayland_dep_for_libva.found() and wayland_scanner_dep_for_libva.found()
endif

# Common C args
libva_c_args = []
if cc.has_function('secure_getenv')
  libva_c_args += ['-DHAVE_SECURE_GETENV']
endif

# Disable symbol versioning for bundled libva
# We're building fresh and don't need backward compatibility with old symbols
# This also avoids compiler issues with asm() directives at file scope
libva_c_args += ['-DVA_COMPAT_DISABLED']

# Suppress missing-prototypes warnings for bundled libva
# Some deprecated functions (vaBufferInfo, vaLockSurface, vaUnlockSurface)
# don't have prototypes in headers but are still implemented for compatibility
if cc.has_argument('-Wno-missing-prototypes')
  libva_c_args += ['-Wno-missing-prototypes']
endif

# Generate va_version.h
version_cfg = configuration_data()
version_cfg.set('VA_API_MAJOR_VERSION', va_api_major_version)
version_cfg.set('VA_API_MINOR_VERSION', va_api_minor_version)
version_cfg.set('VA_API_MICRO_VERSION', va_api_micro_version)
version_cfg.set('VA_API_VERSION', va_api_version)

version_file = configure_file(
  input : 'va/va_version.h.in',
  output : 'va_version.h',
  configuration : version_cfg)

# Core libva sources
# Exclude va_compat.c since we build with VA_COMPAT_DISABLED
# (no symbol versioning needed for fresh builds, and hasvk doesn't support Windows anyway)
libva_sources = files(
  'va/va.c',
  'va/va_str.c',
  'va/va_trace.c',
)

libva_headers = files(
  'va/va_compat.h',
  'va/va_dec_hevc.h',
  'va/va_dec_jpeg.h',
  'va/va_dec_vp8.h',
  'va/va_dec_vp9.h',
  'va/va_dec_av1.h',
  'va/va_dec_vvc.h',
  'va/va_drmcommon.h',
  'va/va_egl.h',
  'va/va_enc_hevc.h',
  'va/va_enc_h264.h',
  'va/va_enc_jpeg.h',
  'va/va_enc_vp8.h',
  'va/va_fei.h',
  'va/va_fei_h264.h',
  'va/va_fei_hevc.h',
  'va/va_enc_mpeg2.h',
  'va/va_enc_vp9.h',
  'va/va_enc_av1.h',
  'va/va_str.h',
  'va/va_tpi.h',
  'va/va_prot.h',
  'va/va_vpp.h',
)

# Headers with architecture-specific types that need multilib wrappers
# These headers will be installed with architecture suffix (e.g., va-64.h)
libva_headers_arch_specific = files(
  'va/va.h',
  'va/va_backend.h',
  'va/va_backend_prot.h',
  'va/va_backend_vpp.h',
)

libva_headers_priv = files(
  'va/sysdeps.h',
  'va/va_internal.h',
  'va/va_trace.h',
)

# Symbol versioning
libva_sym = 'va/libva.syms'
libva_sym_arg = '-Wl,-version-script,' + meson.current_source_dir() / libva_sym

libva_link_args = []
libva_link_depends = []
if cc.links('void vaCreateSurfaces_0_32_0(void) {} void vaCreateSurfaces() {}',
            name: '-Wl,--version-script', args: ['-shared', libva_sym_arg])
  libva_link_args = libva_sym_arg
  libva_link_depends = files(libva_sym)
endif

# Install headers
# Architecture-neutral headers (no pointer-size-dependent types)
install_headers(libva_headers, version_file,
                install_dir : get_option('prefix') / get_option('includedir') / 'va')

# Install architecture-specific headers with suffix
# Note: Using configure_file with copy:true for compatibility with older Meson versions
# that don't support install_headers(rename:)

# Generate both 32-bit and 64-bit versions of each arch-specific header
# va.h
va_h_32 = configure_file(
  input : 'va/va.h',
  output : 'va-32.h',
  copy : true)
va_h_64 = configure_file(
  input : 'va/va.h',
  output : 'va-64.h',
  copy : true)
install_headers(va_h_32, va_h_64,
                install_dir : get_option('prefix') / get_option('includedir') / 'va')

# va_backend.h
va_backend_h_32 = configure_file(
  input : 'va/va_backend.h',
  output : 'va_backend-32.h',
  copy : true)
va_backend_h_64 = configure_file(
  input : 'va/va_backend.h',
  output : 'va_backend-64.h',
  copy : true)
install_headers(va_backend_h_32, va_backend_h_64,
                install_dir : get_option('prefix') / get_option('includedir') / 'va')

# va_backend_prot.h
va_backend_prot_h_32 = configure_file(
  input : 'va/va_backend_prot.h',
  output : 'va_backend_prot-32.h',
  copy : true)
va_backend_prot_h_64 = configure_file(
  input : 'va/va_backend_prot.h',
  output : 'va_backend_prot-64.h',
  copy : true)
install_headers(va_backend_prot_h_32, va_backend_prot_h_64,
                install_dir : get_option('prefix') / get_option('includedir') / 'va')

# va_backend_vpp.h
va_backend_vpp_h_32 = configure_file(
  input : 'va/va_backend_vpp.h',
  output : 'va_backend_vpp-32.h',
  copy : true)
va_backend_vpp_h_64 = configure_file(
  input : 'va/va_backend_vpp.h',
  output : 'va_backend_vpp-64.h',
  copy : true)
install_headers(va_backend_vpp_h_32, va_backend_vpp_h_64,
                install_dir : get_option('prefix') / get_option('includedir') / 'va')

# Install wrapper headers (without .wrapper suffix)
# Note: Using configure_file with copy:true for compatibility
# va.h wrapper
va_h_wrapper = configure_file(
  input : 'va/va.h.wrapper',
  output : 'va.h',
  copy : true)
install_headers(va_h_wrapper,
                install_dir : get_option('prefix') / get_option('includedir') / 'va')
# va_backend.h wrapper
va_backend_h_wrapper = configure_file(
  input : 'va/va_backend.h.wrapper',
  output : 'va_backend.h',
  copy : true)
install_headers(va_backend_h_wrapper,
                install_dir : get_option('prefix') / get_option('includedir') / 'va')
# va_backend_prot.h wrapper
va_backend_prot_h_wrapper = configure_file(
  input : 'va/va_backend_prot.h.wrapper',
  output : 'va_backend_prot.h',
  copy : true)
install_headers(va_backend_prot_h_wrapper,
                install_dir : get_option('prefix') / get_option('includedir') / 'va')
# va_backend_vpp.h wrapper
va_backend_vpp_h_wrapper = configure_file(
  input : 'va/va_backend_vpp.h.wrapper',
  output : 'va_backend_vpp.h',
  copy : true)
install_headers(va_backend_vpp_h_wrapper,
                install_dir : get_option('prefix') / get_option('includedir') / 'va')

# Collect all generated headers for build-time use
libva_generated_headers = [
  va_h_32,
  va_h_64,
  va_backend_h_32,
  va_backend_h_64,
  va_backend_prot_h_32,
  va_backend_prot_h_64,
  va_backend_vpp_h_32,
  va_backend_vpp_h_64,
  va_h_wrapper,
  va_backend_h_wrapper,
  va_backend_prot_h_wrapper,
  va_backend_vpp_h_wrapper,
]

# Create an include directory that points to the build directory where generated headers are
# This ensures our generated headers are found before any system-installed libva headers
libva_build_inc = include_directories('.')

# Build core libva library
libva_hasvk = shared_library(
  'va',
  sources : [libva_sources, libva_headers, libva_headers_arch_specific, libva_headers_priv, version_file, libva_generated_headers],
  soversion : host_machine.system() == 'windows' ? '' : libva_lt_current.to_string(),
  version : libva_lt_version,
  c_args : libva_c_args + [
    '-DSYSCONFDIR="' + sysconfdir + '"',
    '-DVA_DRIVERS_PATH="' + driverdir + '"',
  ],
  include_directories : configinc,
  link_args : libva_link_args,
  link_depends : libva_link_depends,
  install : true,
  install_dir : get_option('libdir'),
  # Use RPATH to ensure this library is found before system libraries
  install_rpath : '$ORIGIN',
  dependencies : [libva_dl_dep],
)

libva_hasvk_dep = declare_dependency(
  link_with : libva_hasvk,
  # Include build directory first so generated headers take priority over system headers
  # This is critical to ensure our bundled headers are used instead of any system-installed libva
  include_directories : [
    libva_build_inc,            # Build directory for generated headers (highest priority)
    configinc,                  # Source directory  
    include_directories('va')   # va source subdirectory
  ],
  sources : libva_generated_headers,
  # Add explicit -I flag for the build directory to override system paths
  # This is necessary because angle bracket includes (<va/header.h>) search system paths first
  compile_args : ['-I' + meson.current_build_dir()],
  dependencies : [libva_dl_dep],
)

# Build libva-drm (required for DRM support)
if WITH_DRM
  libva_drm_sources = files(
    'va/drm/va_drm.c',
    'va/drm/va_drm_auth.c',
    'va/drm/va_drm_utils.c',
  )

  libva_drm_headers = files(
    'va/drm/va_drm.h',
  )

  libva_drm_headers_priv = files(
    'va/drm/va_drm_auth.h',
    'va/drm/va_drm_auth_x11.h',
    'va/drm/va_drm_utils.h',
  )

  libva_drm_deps = [dep_libdrm_for_libva, libva_hasvk_dep]
  libva_drm_c_args = []
  
  if WITH_X11
    libva_drm_sources += files('va/drm/va_drm_auth_x11.c')
    libva_drm_c_args += [
      '-DHAVE_VA_X11',
      '-DLIBVA_MAJOR_VERSION=@0@'.format(libva_major_version),
    ]
    libva_drm_deps += [x11_dep_for_libva]
  endif

  # Add common c_args to drm library
  libva_drm_c_args += libva_c_args

  install_headers(libva_drm_headers,
                  install_dir : get_option('prefix') / get_option('includedir') / 'va')

  libva_drm_hasvk = shared_library(
    'va-drm',
    sources : [libva_drm_sources, libva_drm_headers, libva_drm_headers_priv],
    soversion : libva_lt_current.to_string(),
    version : libva_lt_version,
    c_args : libva_drm_c_args,
    install : true,
    install_dir : get_option('libdir'),
    install_rpath : '$ORIGIN',
    dependencies : libva_drm_deps,
  )

  libva_drm_hasvk_dep = declare_dependency(
    link_with : libva_drm_hasvk,
    include_directories : [configinc, include_directories('va')],
    dependencies : libva_drm_deps,
  )
endif

# Build libva-x11 (required for X11 support)
if WITH_X11
  libva_x11_sources = files(
    'va/x11/dri2_util.c',
    'va/x11/va_dri2.c',
    'va/x11/va_dri3.c',
    'va/drm/va_drm_utils.c',
    'va/x11/va_dricommon.c',
    'va/x11/va_fglrx.c',
    'va/x11/va_nvctrl.c',
    'va/x11/va_x11.c',
  )

  libva_x11_headers = files(
    'va/va_x11.h',
    'va/x11/va_dri2.h',
    'va/x11/va_dri3.h',
    'va/x11/va_dricommon.h',
  )

  libva_x11_headers_priv = files(
    'va/x11/va_dri2str.h',
    'va/x11/va_dri2tokens.h',
    'va/x11/va_dri3.h',
    'va/x11/va_fglrx.h',
    'va/x11/va_nvctrl.h',
  )

  install_headers(libva_x11_headers,
                  install_dir : get_option('prefix') / get_option('includedir') / 'va')

  libva_x11_deps = [
    dep_libdrm_for_libva,
    x11_dep_for_libva,
    xext_dep_for_libva,
    xfixes_dep_for_libva,
    x11_xcb_dep_for_libva,
    xcb_dep_for_libva,
    xcb_dri3_dep_for_libva,
    libva_hasvk_dep,
  ]

  libva_x11_hasvk = shared_library(
    'va-x11',
    sources : [libva_x11_sources, libva_x11_headers, libva_x11_headers_priv],
    soversion : libva_lt_current.to_string(),
    version : libva_lt_version,
    c_args : libva_c_args,  # Add common c_args including -Wno-missing-prototypes
    install : true,
    install_dir : get_option('libdir'),
    install_rpath : '$ORIGIN',
    dependencies : libva_x11_deps,
  )

  libva_x11_hasvk_dep = declare_dependency(
    link_with : libva_x11_hasvk,
    include_directories : [configinc, include_directories('va')],
    dependencies : libva_x11_deps,
  )
endif

# Build libva-glx (optional, for GLX support)
if WITH_GLX
  libva_glx_sources = files(
    'va/glx/va_glx.c',
    'va/glx/va_glx_impl.c',
  )

  libva_glx_headers = files(
    'va/glx/va_backend_glx.h',
    'va/glx/va_glx.h',
  )

  libva_glx_headers_priv = files(
    'va/glx/va_glx_impl.h',
    'va/glx/va_glx_private.h',
  )

  install_headers(libva_glx_headers,
                  install_dir : get_option('prefix') / get_option('includedir') / 'va')

  libva_glx_deps = [gl_dep_for_libva, libva_x11_hasvk_dep]

  libva_glx_hasvk = shared_library(
    'va-glx',
    sources : [libva_glx_sources, libva_glx_headers, libva_glx_headers_priv],
    soversion : libva_lt_current.to_string(),
    version : libva_lt_version,
    c_args : libva_c_args,  # Add common c_args including -Wno-missing-prototypes
    install : true,
    install_dir : get_option('libdir'),
    install_rpath : '$ORIGIN',
    dependencies : libva_glx_deps,
  )

  libva_glx_hasvk_dep = declare_dependency(
    link_with : libva_glx_hasvk,
    include_directories : [configinc, include_directories('va')],
    dependencies : libva_glx_deps,
  )
endif

# Build libva-wayland (optional, for Wayland support)
if WITH_WAYLAND
  libva_wayland_sources = files(
    'va/wayland/va_wayland.c',
    'va/wayland/va_wayland_linux_dmabuf.c',
    'va/wayland/va_wayland_drm.c',
    'va/wayland/va_wayland_emgd.c',
    'va/drm/va_drm_utils.c',
  )

  libva_wayland_headers = files(
    'va/wayland/va_backend_wayland.h',
    'va/wayland/va_wayland.h',
  )

  libva_wayland_headers_priv = files(
    'va/wayland/va_wayland_linux_dmabuf.h',
    'va/wayland/va_wayland_drm.h',
    'va/wayland/va_wayland_emgd.h',
    'va/wayland/va_wayland_private.h',
  )

  # Generate wayland protocol files
  protocols = {
    'wayland-drm': 'va/wayland/wayland-drm.xml',
    'linux-dmabuf-v1': 'va/wayland/linux-dmabuf-v1.xml',
  }

  protocol_files = []
  foreach name, xml : protocols
    protocol_files += custom_target(
      name + '-client-protocol.c',
      output : name + '-client-protocol.c',
      input : xml,
      command : [wl_scanner_for_libva, 'private-code', '@INPUT@', '@OUTPUT@'])
    protocol_files += custom_target(
      name + '-client-protocol.h',
      output : name + '-client-protocol.h',
      input : xml,
      command : [wl_scanner_for_libva, 'client-header', '@INPUT@', '@OUTPUT@'])
  endforeach

  install_headers(libva_wayland_headers,
                  install_dir : get_option('prefix') / get_option('includedir') / 'va')

  libva_wayland_deps = [dep_libdrm_for_libva, wayland_dep_for_libva, libva_hasvk_dep]

  libva_wayland_hasvk = shared_library(
    'va-wayland',
    sources : [libva_wayland_sources, libva_wayland_headers, libva_wayland_headers_priv, protocol_files],
    soversion : libva_lt_current.to_string(),
    version : libva_lt_version,
    c_args : libva_c_args,  # Add common c_args including -Wno-missing-prototypes
    install : true,
    install_dir : get_option('libdir'),
    install_rpath : '$ORIGIN',
    dependencies : libva_wayland_deps,
  )

  libva_wayland_hasvk_dep = declare_dependency(
    link_with : libva_wayland_hasvk,
    include_directories : [configinc, include_directories('va')],
    dependencies : libva_wayland_deps,
  )
endif

# Install license and documentation
install_data(
  'COPYING',
  install_dir : get_option('datadir') / 'licenses' / 'mesa-hasvk-libva',
)

# Create pkg-config file for bundled libva
# The headers are now installed in the standard include directory
# with architecture-specific versions (e.g., va-32.h, va-64.h) and
# architecture-neutral wrapper headers that include the correct version.
pkg_vars = ['driverdir=' + driverdir]

# Determine pkg-config installation directory for multilib
# Install .pc files to libdir/pkgconfig to avoid conflicts
pkg_install_dir = get_option('prefix') / get_option('libdir') / 'pkgconfig'

pkg.generate(
  libva_hasvk,
  name : 'libva',
  description : 'Userspace Video Acceleration (VA) API (Mesa hasvk bundled version)',
  version : libva_version,
  variables : pkg_vars,
  install_dir : pkg_install_dir,
)

if WITH_DRM
  pkg.generate(
    libva_drm_hasvk,
    name : 'libva-drm',
    description : 'Userspace Video Acceleration (VA) API (DRM/KMS runtime) (Mesa hasvk bundled version)',
    version : libva_version,
    install_dir : pkg_install_dir,
  )
endif

if WITH_X11
  pkg.generate(
    libva_x11_hasvk,
    name : 'libva-x11',
    description : 'Userspace Video Acceleration (VA) API (X11 runtime) (Mesa hasvk bundled version)',
    version : libva_version,
    install_dir : pkg_install_dir,
  )
endif

if WITH_GLX
  pkg.generate(
    libva_glx_hasvk,
    name : 'libva-glx',
    description : 'Userspace Video Acceleration (VA) API (GLX runtime) (Mesa hasvk bundled version)',
    version : libva_version,
    install_dir : pkg_install_dir,
  )
endif

if WITH_WAYLAND
  pkg.generate(
    libva_wayland_hasvk,
    name : 'libva-wayland',
    description : 'Userspace Video Acceleration (VA) API (Wayland runtime) (Mesa hasvk bundled version)',
    version : libva_version,
    install_dir : pkg_install_dir,
  )
endif

# Message to indicate successful configuration
message('Bundled libva @0@ configured for hasvk (will take priority over system libva)'.format(libva_version))
message('  VA-API version: @0@'.format(va_api_version))
message('  Driver directory: @0@'.format(driverdir))
message('  DRM support: @0@'.format(WITH_DRM))
message('  X11 support: @0@'.format(WITH_X11))
message('  GLX support: @0@'.format(WITH_GLX))
message('  Wayland support: @0@'.format(WITH_WAYLAND))
