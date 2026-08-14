project('wmd', 'c', version: '0.1.0', default_options: ['c_std=c11', 'warning_level=2'])

cc = meson.get_compiler('c')
gtk = dependency('gtk+-3.0', version: '>=3.24')
layer_shell = dependency('gtk-layer-shell-0', version: '>=0.6')
heif = dependency('libheif', version: '>=1.12')
gst = dependency('gstreamer-1.0', version: '>=1.20')
gst_app = dependency('gstreamer-app-1.0', version: '>=1.20')
gst_video = dependency('gstreamer-video-1.0', version: '>=1.20')
m_dep = cc.find_library('m', required: true)

conf = configuration_data()
conf.set_quoted('WMD_VERSION', meson.project_version())
configure_file(output: 'wmd-build-config.h', configuration: conf)

sources = files(
  'src/main.c', 'src/config.c', 'src/ipc.c', 'src/image.c',
  'src/wallpaper.c', 'src/collection.c', 'src/transitions.c', 'src/renderer.c', 'src/video.c', 'src/daemon.c',
  'src/gui/common.c', 'src/gui/switcher.c', 'src/gui/carousel.c',
  'src/gui/three_d.c', 'src/gui/layouts.c', 'src/gui/alpha.c',
)

executable('wmd', sources,
  include_directories: include_directories('src', 'src/gui'),
  dependencies: [gtk, layer_shell, heif, gst, gst_app, gst_video, m_dep],
  install: true,
)

install_data('examples/switcher.conf', install_dir: get_option('datadir') / 'wmd')
install_data('examples/collection.conf', install_dir: get_option('datadir') / 'wmd')
