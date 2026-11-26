# The BDN+XML renderer. Shares every source file with the CSRI plugin, so an
# exported subtitle is pixel-identical to Aegisub's preview.

vsfmrender = executable('vsfmrender',
    vsfm_src + files('bdnxml_writer.cpp', 'main.cpp') + version_rc.get('renderer', []),
    include_directories : inc,
    dependencies : deps,
    # The CLI needs its own symbols, unlike the plugin.
    gnu_symbol_visibility : 'default',
    # Keeps a static FreeType's symbols out of the executable's dynamic
    # symbol table, where fontconfig's calls into its own FreeType would
    # otherwise bind to them.
    link_args : plugin_link_args,
    install : true)
