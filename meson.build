project('rn1-host', 'c', default_options: ['c=c99'])

src = [
  'hwdata.c',
  'map_memdisk.c',
  'map_opers.c',
  'mapping.c',
  'mcu_micronavi_docu.c',
  'rn1host.c',
  'routing.c',
  'tcp_comm.c',
  'tcp_parser.c',
  'uart.c',
]

add_project_arguments(
  '-DDELIVERY_BOY',
  '-DMAP_DIR="/home/pulu/rn1-host"',
  '-DSERIAL_DEV="/dev/serial0"',
  '-DMOTCON_PID_EXPERIMENT',
  '-Wall',
  '-Winline',
  '-g',
  language: 'c'
)

add_project_link_arguments(
  '-Wl,--allow-multiple-definition',
  language: 'c'
)

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: true)
thread_dep = dependency('threads')

common_dep = declare_dependency(
  dependencies: [m_dep, thread_dep]
)

executable('rn1-host', src, dependencies: common_dep)
