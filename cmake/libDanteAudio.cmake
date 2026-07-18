get_filename_component(_DANTE_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(CMAKE_SYSTEM_PROCESSOR)
    set(_DANTE_SDK_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
else()
    set(_DANTE_SDK_ARCH "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()

if(_DANTE_SDK_ARCH MATCHES "^(x86_64|amd64|AMD64)$")
    set(_DANTE_SDK_PLATFORM "x86_64-linux")
elseif(_DANTE_SDK_ARCH MATCHES "^(aarch64|arm64)$")
    set(_DANTE_SDK_PLATFORM "aarch64-linux")
else()
    message(FATAL_ERROR "libDanteAudio.cmake: no prebuilt libDanteAudio.a for architecture '${_DANTE_SDK_ARCH}'")
endif()

add_library(DanteAudio STATIC IMPORTED GLOBAL)
set_target_properties(DanteAudio PROPERTIES
    IMPORTED_LOCATION "${_DANTE_SDK_ROOT}/lib/${_DANTE_SDK_PLATFORM}/libDanteAudio.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_DANTE_SDK_ROOT}/include"
)
target_link_libraries(DanteAudio INTERFACE pthread rt)
