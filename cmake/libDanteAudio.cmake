get_filename_component(_DANTE_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

add_library(DanteAudio STATIC IMPORTED GLOBAL)
set_target_properties(DanteAudio PROPERTIES
    IMPORTED_LOCATION "${_DANTE_SDK_ROOT}/lib/libDanteAudio.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_DANTE_SDK_ROOT}/include"
)
target_link_libraries(DanteAudio INTERFACE pthread rt)
