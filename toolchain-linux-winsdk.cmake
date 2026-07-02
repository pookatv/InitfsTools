set(CMAKE_SYSTEM_NAME Windows)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)

set(CMAKE_C_COMPILER_FRONTEND_VARIANT MSVC)
set(CMAKE_CXX_COMPILER_FRONTEND_VARIANT MSVC)

set(WINDOWS_SDK_PATH "" CACHE PATH "Path to Windows SDK root")
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES WINDOWS_SDK_PATH)

message(STATUS "WINDOWS_SDK_PATH=${WINDOWS_SDK_PATH}")

if(NOT WINDOWS_SDK_PATH)
    if(DEFINED ENV{WINDOWS_SDK_PATH})
        set(WINDOWS_SDK_PATH $ENV{WINDOWS_SDK_PATH})
    else()
        find_program(_msbuild_exe msbuild)
        if(_msbuild_exe)
            get_filename_component(_msbuild_bin ${_msbuild_exe} DIRECTORY)
            get_filename_component(_candidate "${_msbuild_bin}/.." ABSOLUTE)
            if(NOT IS_DIRECTORY "${_candidate}/vc")
                get_filename_component(_candidate "${_candidate}/.." ABSOLUTE)
            endif()
            message(STATUS "Found Windows SDK Path in ${_candidate}")
            set(WINDOWS_SDK_PATH ${_candidate})
        else()
            set(WINDOWS_SDK_PATH /opt/msvc)
        endif()
    endif()
endif()

if(NOT EXISTS "${WINDOWS_SDK_PATH}/kits/10/Include")
    message(FATAL_ERROR "Invalid WINDOWS_SDK_PATH: ${WINDOWS_SDK_PATH}")
endif()

# Prevent host-package leakage: FindVulkan on Linux pulls /usr/include into
# WrapVulkanHeaders, which then gets injected into every target via Qt6::Gui
set(CMAKE_DISABLE_FIND_PACKAGE_Vulkan TRUE)

file(GLOB _winsdk_versions LIST_DIRECTORIES true
     "${WINDOWS_SDK_PATH}/kits/10/Include/*")

if(NOT _winsdk_versions)
    message(FATAL_ERROR "No Windows SDK versions found")
endif()

list(GET _winsdk_versions 0 _winsdk_dir)
get_filename_component(_winsdk_ver "${_winsdk_dir}" NAME)

add_compile_options(
    --target=x86_64-pc-windows-msvc
    -fms-compatibility
    -fms-extensions
    -fdelayed-template-parsing
    /winsysroot ${WINDOWS_SDK_PATH}
)

add_link_options(/winsysroot:${WINDOWS_SDK_PATH})

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Qt6 host tools — the Windows Qt install ships .exe tool binaries that need
# wine to run on Linux.  These are handled by wine wrapper scripts in run.sh,
# so CMAKE_CROSSCOMPILING_EMULATOR is intentionally not set here (AUTOMOC
# bypasses it and would try to run .exe files directly).

if(CMAKE_PREFIX_PATH AND NOT Qt6CoreTools_DIR)
    foreach(_qt_tools CoreTools GuiTools WidgetsTools DBusTools)
        set(_candidate "${CMAKE_PREFIX_PATH}/lib/cmake/Qt6${_qt_tools}")
        if(EXISTS "${_candidate}/Qt6${_qt_tools}Config.cmake")
            set(Qt6${_qt_tools}_DIR "${_candidate}" CACHE PATH "Path to Qt6 ${_qt_tools}")
            message(STATUS "Qt6 ${_qt_tools}: ${_candidate}")
        endif()
    endforeach()
endif()
