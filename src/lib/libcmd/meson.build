libcmd_files = [ 'cmdinit.c' ]

# These files contains builtins for standard commands.
libcmd_files += [ 'basename.c', 'chmod.c', 'dirname.c', 'head.c', 'mkdir.c',
                  'logname.c', 'cat.c', 'cmp.c', 'uname.c', 'sync.c', 'cut.c' , 'wc.c', ]

libcmd_c_args = shared_c_args + [
    '-DUSAGE_LICENSE=""',
]

incdir = include_directories('../libast/include')

libcmd = library('cmd', libcmd_files, c_args: libcmd_c_args,
                 include_directories: [configuration_incdir, incdir],
                 link_with: libast,
                 install: get_option('default_library') == 'shared')
