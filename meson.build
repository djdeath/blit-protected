project('blit-protected',
        ['c'],
        version : '0.1',
        license : 'MIT',
        default_options : ['c_std=c11'])

blit_protected = executable(
  'blit-protected',
  files('blit.c'),
  c_args : [ '-Wall' ],
  dependencies : [
    dependency('vulkan'),
    dependency('gdk-pixbuf-2.0'),
  ],
)
