# Removes every locale .pak from LOCALES_DIR except en-US.pak.
# Invoked as a POST_BUILD step (cmake -P) so the packaged output keeps only the
# locale CEF actually needs (~44 MB of unused locales otherwise).
if(NOT DEFINED LOCALES_DIR OR NOT IS_DIRECTORY "${LOCALES_DIR}")
    return()
endif()

file(GLOB _paks "${LOCALES_DIR}/*.pak")
foreach(_pak ${_paks})
    get_filename_component(_name "${_pak}" NAME)
    if(NOT _name STREQUAL "en-US.pak")
        file(REMOVE "${_pak}")
    endif()
endforeach()
