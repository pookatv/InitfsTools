cmake_minimum_required(VERSION 3.20)
project(InitfsTools VERSION 2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT DEFINED QT_PATH OR QT_PATH STREQUAL "")
    if(DEFINED ENV{QTDIR})
        set(QT_PATH "$ENV{QTDIR}" CACHE PATH "Path to Qt installation" FORCE)
    elseif(WIN32)
        set(QT_PATH "C:/Qt/6.10.0/msvc2022_64" CACHE PATH "Path to Qt installation" FORCE)
    elseif(APPLE)
        set(QT_PATH "/usr/local/opt/qt6" CACHE PATH "Path to Qt installation" FORCE)
    else()
        set(QT_PATH "/usr/lib/qt6" CACHE PATH "Path to Qt installation" FORCE)
    endif()
endif()

set(OPENSSL_PATH "" CACHE PATH "Path to OpenSSL for Windows (e.g., /opt/openssl-windows)")

message(STATUS "QT_PATH = ${QT_PATH}")

set(CMAKE_PREFIX_PATH "${QT_PATH}")

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Concurrent Network)

file(GLOB_RECURSE SOURCES
    src/*.cpp
    src/*.h
    src/ui/*.ui
    src/core/*.cpp
    src/core/*.h
    src/core/deobfuscators/*.cpp
    src/core/deobfuscators/*.h
)

qt_add_executable(InitfsTools
    ${SOURCES}
    resources/resources.qrc
    resources/app.rc
)

set_target_properties(InitfsTools PROPERTIES qt_no_entrypoint TRUE)

if(MSVC)
    target_link_options(InitfsTools PRIVATE /entry:mainCRTStartup)
endif()

file(GLOB PAYLOAD_DATA_FILES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/payload_data/*.txt"
)

qt_add_resources(InitfsTools "payload_data_resources"
    PREFIX "/payload_data"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/payload_data"
    FILES ${PAYLOAD_DATA_FILES}
)

file(GLOB_RECURSE PRESET_DATA_FILES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/preset_data/*.txt"
)

qt_add_resources(InitfsTools "preset_data_resources"
    PREFIX "/preset_data"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/preset_data"
    FILES ${PRESET_DATA_FILES}
)

if(MSVC)
    target_compile_options(InitfsTools PRIVATE /EHsc /utf-8 /MP)
endif()

target_include_directories(InitfsTools PRIVATE
    src/
    src/core/
    src/core/deobfuscators/
    src/ui/
    "${QT_PATH}/include"
    "${QT_PATH}/include/QtCore"
    "${QT_PATH}/include/QtWidgets"
    "${QT_PATH}/include/Qsci"
)

if(NOT OPENSSL_PATH STREQUAL "")
    target_include_directories(InitfsTools PRIVATE "${OPENSSL_PATH}/include")
endif()

# QScintilla is always built as a static library in CI
target_compile_definitions(InitfsTools PRIVATE QSCINTILLA_STATIC)

set(QSCINTILLA_LIB "${QT_PATH}/lib/qscintilla2_qt6.lib")

if(NOT EXISTS "${QSCINTILLA_LIB}")
    message(FATAL_ERROR
        "QScintilla lib not found at: ${QSCINTILLA_LIB}\n"
        "Check that QScintilla was built and installed into ${QT_PATH}/lib"
    )
endif()

message(STATUS "QScintilla lib: ${QSCINTILLA_LIB}")

# OpenSSL
set(OPENSSL_CRYPTO_LIB "")
set(OPENSSL_SSL_LIB "")

foreach(candidate
    "${QT_PATH}/lib/libcrypto_static.lib"
    "${QT_PATH}/lib/libcrypto.lib"
    "${OPENSSL_PATH}/lib/libcrypto_static.lib"
    "${OPENSSL_PATH}/lib/libcrypto.lib"
    "C:/Program Files/OpenSSL-Win64/lib/VC/x64/MT/libcrypto_static.lib"
    "C:/Program Files/OpenSSL-Win64/lib/VC/x64/MT/libcrypto.lib"
)
    if(EXISTS "${candidate}" AND OPENSSL_CRYPTO_LIB STREQUAL "")
        set(OPENSSL_CRYPTO_LIB "${candidate}")
    endif()
endforeach()

foreach(candidate
    "${QT_PATH}/lib/libssl_static.lib"
    "${QT_PATH}/lib/libssl.lib"
    "${OPENSSL_PATH}/lib/libssl_static.lib"
    "${OPENSSL_PATH}/lib/libssl.lib"
    "C:/Program Files/OpenSSL-Win64/lib/VC/x64/MT/libssl_static.lib"
    "C:/Program Files/OpenSSL-Win64/lib/VC/x64/MT/libssl.lib"
)
    if(EXISTS "${candidate}" AND OPENSSL_SSL_LIB STREQUAL "")
        set(OPENSSL_SSL_LIB "${candidate}")
    endif()
endforeach()

message(STATUS "OpenSSL crypto: ${OPENSSL_CRYPTO_LIB}")
message(STATUS "OpenSSL ssl:    ${OPENSSL_SSL_LIB}")

# bcrypt DLL
add_library(initfs_bcrypt SHARED
    src/core/bcrypt/bcrypt.cpp
)

set_target_properties(initfs_bcrypt PROPERTIES
    OUTPUT_NAME "bcrypt"
    ARCHIVE_OUTPUT_NAME "initfs_bcrypt"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/initfs_bcrypt_build"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/initfs_bcrypt_build"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/initfs_bcrypt_build"
)

target_include_directories(initfs_bcrypt PRIVATE
    src/
    src/core/
    "${QT_PATH}/include"
    "${QT_PATH}/include/QtCore"
)

if(MSVC)
    target_compile_options(initfs_bcrypt PRIVATE /EHsc /utf-8)
    set_target_properties(initfs_bcrypt PROPERTIES
        LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/src/core/bcrypt/bcrypt.def\""
    )
endif()
# end bcrypt DLL

target_link_libraries(InitfsTools PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Network
    "${QSCINTILLA_LIB}"
    initfs_bcrypt
)

if(NOT OPENSSL_CRYPTO_LIB STREQUAL "")
    target_link_libraries(InitfsTools PRIVATE "${OPENSSL_CRYPTO_LIB}")
endif()

if(NOT OPENSSL_SSL_LIB STREQUAL "")
    target_link_libraries(InitfsTools PRIVATE "${OPENSSL_SSL_LIB}")
endif()

# Static OpenSSL requires these Windows system libs
if(WIN32 AND (NOT OPENSSL_CRYPTO_LIB STREQUAL "" OR NOT OPENSSL_SSL_LIB STREQUAL ""))
    target_link_libraries(InitfsTools PRIVATE crypt32 ws2_32)
endif()

# Force static CRT — no VC redistributable needed on target
set_property(TARGET InitfsTools PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

set_target_properties(InitfsTools PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:InitfsTools>"
)

target_link_libraries(InitfsTools PRIVATE
    dwmapi uxtheme opengl32 gdi32 comdlg32 oleaut32 imm32
    winmm ws2_32 ole32 user32 advapi32 shell32 shlwapi
    version setupapi wtsapi32 userenv netapi32 winspool
)

# qt.conf so Qt finds plugins inside Dependencies/
set(QT_CONF_PATH "${CMAKE_CURRENT_BINARY_DIR}/qt.conf")
file(WRITE "${QT_CONF_PATH}" "[Paths]\nPlugins=Dependencies\n")

if(WIN32)
    set_target_properties(InitfsTools PROPERTIES WIN32_EXECUTABLE TRUE)

    # Copy Qt DLLs, OpenSSL DLLs, bcrypt.dll, qt.conf into build dir
    add_custom_command(TARGET InitfsTools POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QT_PATH}/bin/Qt6Core.dll"         "$<TARGET_FILE_DIR:InitfsTools>/Qt6Core.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QT_PATH}/bin/Qt6Gui.dll"          "$<TARGET_FILE_DIR:InitfsTools>/Qt6Gui.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QT_PATH}/bin/Qt6Widgets.dll"      "$<TARGET_FILE_DIR:InitfsTools>/Qt6Widgets.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QT_PATH}/bin/Qt6Concurrent.dll"   "$<TARGET_FILE_DIR:InitfsTools>/Qt6Concurrent.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QT_PATH}/bin/Qt6Network.dll"      "$<TARGET_FILE_DIR:InitfsTools>/Qt6Network.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QT_PATH}/bin/Qt6PrintSupport.dll" "$<TARGET_FILE_DIR:InitfsTools>/Qt6PrintSupport.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QT_PATH}/bin/libcrypto-3-x64.dll" "$<TARGET_FILE_DIR:InitfsTools>/libcrypto-3-x64.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QT_PATH}/bin/libssl-3-x64.dll"    "$<TARGET_FILE_DIR:InitfsTools>/libssl-3-x64.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:initfs_bcrypt>"       "$<TARGET_FILE_DIR:InitfsTools>/bcrypt.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QT_CONF_PATH}"                    "$<TARGET_FILE_DIR:InitfsTools>/qt.conf"
        COMMENT "Copying runtime DLLs to build dir..."
    )

    # Copy Qt plugins into Dependencies/
    add_custom_command(TARGET InitfsTools POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:InitfsTools>/Dependencies/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${QT_PATH}/plugins/platforms" "$<TARGET_FILE_DIR:InitfsTools>/Dependencies/platforms"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:InitfsTools>/Dependencies/styles"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${QT_PATH}/plugins/styles" "$<TARGET_FILE_DIR:InitfsTools>/Dependencies/styles"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:InitfsTools>/Dependencies/imageformats"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${QT_PATH}/plugins/imageformats" "$<TARGET_FILE_DIR:InitfsTools>/Dependencies/imageformats"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:InitfsTools>/Dependencies/iconengines"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${QT_PATH}/plugins/iconengines" "$<TARGET_FILE_DIR:InitfsTools>/Dependencies/iconengines"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:InitfsTools>/Dependencies/tls"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${QT_PATH}/plugins/tls" "$<TARGET_FILE_DIR:InitfsTools>/Dependencies/tls"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:InitfsTools>/Dependencies/networkinformation"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${QT_PATH}/plugins/networkinformation" "$<TARGET_FILE_DIR:InitfsTools>/Dependencies/networkinformation"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:InitfsTools>/Dependencies/generic"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${QT_PATH}/plugins/generic" "$<TARGET_FILE_DIR:InitfsTools>/Dependencies/generic"
        COMMENT "Copying Qt plugins..."
    )
endif()

# Collect everything into dist/
install(TARGETS InitfsTools RUNTIME DESTINATION .)

install(FILES "$<TARGET_FILE:initfs_bcrypt>"
    DESTINATION "."
    RENAME "bcrypt.dll"
)

add_custom_command(TARGET InitfsTools POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        --install "${CMAKE_BINARY_DIR}"
        --prefix "$<TARGET_FILE_DIR:InitfsTools>/dist"
        --config $<CONFIG>
    COMMENT "Installing clean dist to dist/..."
)

# Copy runtime DLLs into dist/
add_custom_command(TARGET InitfsTools POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${QT_PATH}/bin/Qt6Core.dll"         "$<TARGET_FILE_DIR:InitfsTools>/dist/Qt6Core.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${QT_PATH}/bin/Qt6Gui.dll"          "$<TARGET_FILE_DIR:InitfsTools>/dist/Qt6Gui.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${QT_PATH}/bin/Qt6Widgets.dll"      "$<TARGET_FILE_DIR:InitfsTools>/dist/Qt6Widgets.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${QT_PATH}/bin/Qt6Concurrent.dll"   "$<TARGET_FILE_DIR:InitfsTools>/dist/Qt6Concurrent.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${QT_PATH}/bin/Qt6Network.dll"      "$<TARGET_FILE_DIR:InitfsTools>/dist/Qt6Network.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${QT_PATH}/bin/Qt6PrintSupport.dll" "$<TARGET_FILE_DIR:InitfsTools>/dist/Qt6PrintSupport.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${QT_PATH}/bin/libcrypto-3-x64.dll" "$<TARGET_FILE_DIR:InitfsTools>/dist/libcrypto-3-x64.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${QT_PATH}/bin/libssl-3-x64.dll"    "$<TARGET_FILE_DIR:InitfsTools>/dist/libssl-3-x64.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:initfs_bcrypt>"       "$<TARGET_FILE_DIR:InitfsTools>/dist/bcrypt.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${QT_CONF_PATH}"                    "$<TARGET_FILE_DIR:InitfsTools>/dist/qt.conf"
    COMMENT "Copying runtime DLLs to dist/..."
)

# Copy plugins into dist/Dependencies/
add_custom_command(TARGET InitfsTools POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:InitfsTools>/dist/Dependencies/platforms"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${QT_PATH}/plugins/platforms" "$<TARGET_FILE_DIR:InitfsTools>/dist/Dependencies/platforms"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:InitfsTools>/dist/Dependencies/styles"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${QT_PATH}/plugins/styles" "$<TARGET_FILE_DIR:InitfsTools>/dist/Dependencies/styles"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:InitfsTools>/dist/Dependencies/imageformats"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${QT_PATH}/plugins/imageformats" "$<TARGET_FILE_DIR:InitfsTools>/dist/Dependencies/imageformats"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:InitfsTools>/dist/Dependencies/iconengines"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${QT_PATH}/plugins/iconengines" "$<TARGET_FILE_DIR:InitfsTools>/dist/Dependencies/iconengines"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:InitfsTools>/dist/Dependencies/tls"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${QT_PATH}/plugins/tls" "$<TARGET_FILE_DIR:InitfsTools>/dist/Dependencies/tls"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:InitfsTools>/dist/Dependencies/networkinformation"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${QT_PATH}/plugins/networkinformation" "$<TARGET_FILE_DIR:InitfsTools>/dist/Dependencies/networkinformation"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:InitfsTools>/dist/Dependencies/generic"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${QT_PATH}/plugins/generic" "$<TARGET_FILE_DIR:InitfsTools>/dist/Dependencies/generic"
    COMMENT "Copying Qt plugins to dist/..."
)