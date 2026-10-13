cmake_minimum_required(VERSION 3.15)
project(tender VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(X11 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)

# Wayland (optional). If present we build the headless wl_shm compositor backend.
pkg_check_modules(WAYLAND_SERVER wayland-server)
pkg_check_modules(WAYLAND_CLIENT wayland-client)
pkg_check_modules(WAYLAND_PROTOCOLS wayland-protocols)
pkg_check_modules(XKBCOMMON xkbcommon)
pkg_check_modules(EGL egl)
pkg_check_modules(GLESV2 glesv2)
find_program(WAYLAND_SCANNER wayland-scanner)

set(HAVE_WAYLAND OFF)
set(HAVE_DMABUF OFF)
if(WAYLAND_SERVER_FOUND AND WAYLAND_PROTOCOLS_FOUND AND WAYLAND_SCANNER)
    set(HAVE_WAYLAND ON)
    pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
    set(XDG_SHELL_XML ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml)
    set(DMABUF_XML ${WAYLAND_PROTOCOLS_DIR}/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml)
    set(WL_PROTO_DIR ${CMAKE_CURRENT_BINARY_DIR}/protocols)
    file(MAKE_DIRECTORY ${WL_PROTO_DIR})
    add_custom_command(
        OUTPUT ${WL_PROTO_DIR}/xdg-shell-server-protocol.h
               ${WL_PROTO_DIR}/xdg-shell-client-protocol.h
               ${WL_PROTO_DIR}/xdg-shell-protocol.c
        COMMAND ${WAYLAND_SCANNER} server-header ${XDG_SHELL_XML}
                ${WL_PROTO_DIR}/xdg-shell-server-protocol.h
        COMMAND ${WAYLAND_SCANNER} client-header ${XDG_SHELL_XML}
                ${WL_PROTO_DIR}/xdg-shell-client-protocol.h
        COMMAND ${WAYLAND_SCANNER} private-code ${XDG_SHELL_XML}
                ${WL_PROTO_DIR}/xdg-shell-protocol.c
        DEPENDS ${XDG_SHELL_XML}
        COMMENT "Generating xdg-shell protocol"
    )
    set(WAYLAND_SOURCES
        src/wayland_server.cpp
        ${WL_PROTO_DIR}/xdg-shell-protocol.c
    )

    if(EXISTS ${DMABUF_XML})
        add_custom_command(
            OUTPUT ${WL_PROTO_DIR}/linux-dmabuf-server-protocol.h
                   ${WL_PROTO_DIR}/linux-dmabuf-client-protocol.h
                   ${WL_PROTO_DIR}/linux-dmabuf-protocol.c
            COMMAND ${WAYLAND_SCANNER} server-header ${DMABUF_XML}
                    ${WL_PROTO_DIR}/linux-dmabuf-server-protocol.h
            COMMAND ${WAYLAND_SCANNER} client-header ${DMABUF_XML}
                    ${WL_PROTO_DIR}/linux-dmabuf-client-protocol.h
            COMMAND ${WAYLAND_SCANNER} private-code ${DMABUF_XML}
                    ${WL_PROTO_DIR}/linux-dmabuf-protocol.c
            DEPENDS ${DMABUF_XML}
            COMMENT "Generating linux-dmabuf protocol"
        )
        list(APPEND WAYLAND_SOURCES ${WL_PROTO_DIR}/linux-dmabuf-protocol.c)
        if(EGL_FOUND AND GLESV2_FOUND)
            set(HAVE_DMABUF ON)
            list(APPEND WAYLAND_SOURCES src/egl_import.cpp)
        endif()
    endif()
    message(STATUS "Wayland backend enabled (dmabuf=${HAVE_DMABUF})")
else()
    set(WAYLAND_SOURCES src/wayland_stub.cpp)
    message(STATUS "Wayland backend disabled (missing wayland-server/protocols/scanner)")
endif()

include_directories(
    ${X11_INCLUDE_DIR}
    ${WAYLAND_SERVER_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GLESV2_INCLUDE_DIRS}
    ${WL_PROTO_DIR}
    src
)

add_executable(tender
    src/main.cpp
    src/display.cpp
    src/xvfb_server.cpp
    src/app.cpp
    src/capture.cpp
    src/kitty.cpp
    src/sixel.cpp
    src/ansi.cpp
    src/term.cpp
    src/input.cpp
    ${WAYLAND_SOURCES}
)

target_link_libraries(tender
    PRIVATE
    ${X11_LIBRARIES}
    ${X11_Xext_LIB}
    ${X11_Xfixes_LIB}
    ${WAYLAND_SERVER_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLESV2_LIBRARIES}
    ZLIB::ZLIB
    Threads::Threads
    dl
)

target_compile_definitions(tender PRIVATE
    HAVE_WAYLAND=$<BOOL:${HAVE_WAYLAND}>
    HAVE_DMABUF=$<BOOL:${HAVE_DMABUF}>
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(tender PRIVATE DEBUG)
endif()
