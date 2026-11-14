# cmake -DDIR=<pkg>/plugin/usd -P PluginRpath.cmake (Linux): points the module plugins copied from vcpkg at the package root.
# vcpkg leaves them $ORIGIN/../../lib, its own lib folder; the package keeps the libraries beside OpenUSDCore instead, so a
# plugin needing one nothing loaded yet (hdEmbree and libembree4) found nothing.
file(GLOB _plugins "${DIR}/*.so")
foreach(_plugin IN LISTS _plugins)
    file(RPATH_SET FILE "${_plugin}" NEW_RPATH "$ORIGIN:$ORIGIN/../..")
endforeach()
