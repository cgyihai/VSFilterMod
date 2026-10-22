# The BDN+XML renderer. Shares every source file with the CSRI plugin, so an
# exported subtitle is pixel-identical to Aegisub's preview.

vsfmrender = executable('vsfmrender',
    vsfm_src + files('bdnxml_writer.cpp', 'main.cpp'),
    include_directories : [inc] + sse2neon_inc,
    dependencies : deps,
    # The CLI needs its own symbols, unlike the plugin.
    gnu_symbol_visibility : 'default',
    install : true)
