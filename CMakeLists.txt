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

message(STATUS "QT_PATH = ${QT_PATH}")
message(STATUS "QT_STATIC = ${QT_STATIC}")

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
    # Delay-load our bcrypt.dll so it doesn't shadow System32\bcrypt.dll at startup
    target_link_options(InitfsTools PRIVATE /DELAYLOAD:bcrypt.dll)
    target_link_libraries(InitfsTools PRIVATE delayimp)
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
    "C:/Program Files/OpenSSL-Win64/include"
)

# Static build does not use the DLL import definition
if(QT_STATIC)
    target_compile_definitions(InitfsTools PRIVATE QSCINTILLA_STATIC)
else()
    target_compile_definitions(InitfsTools PRIVATE QSCINTILLA_DLL)
endif()

# QScintilla
set(QSCINTILLA_LIB "${QT_PATH}/lib/qscintilla2_qt6.lib")

if(NOT QT_STATIC)
    # Always use release QScintilla DLL — no debug variant exists
    if(EXISTS "${QT_PATH}/bin/qscintilla2_qt6.dll")
        set(QSCINTILLA_DLL "${QT_PATH}/bin/qscintilla2_qt6.dll")
    else()
        set(QSCINTILLA_DLL "C:/Users/Luca/Documents/_InitfsConverterEXE/C++/Qt_6.10_Backport/bin/qscintilla2_qt6.dll")
    endif()
endif()

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

# Static build — import Qt plugins and link required Windows system libs
if(QT_STATIC)
    # Force static CRT to match static Qt which was built with /MT
    set_property(TARGET InitfsTools PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

    # Release VS debugger working directory points to dist/
    set_target_properties(InitfsTools PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:InitfsTools>/dist"
    )

    qt_import_plugins(InitfsTools
        INCLUDE
            Qt::QWindowsIntegrationPlugin
            Qt::QWindowsVistaStylePlugin
            Qt::QICOPlugin
            Qt::QJpegPlugin
            Qt::QGifPlugin
    )

    target_link_libraries(InitfsTools PRIVATE
        dwmapi uxtheme opengl32 gdi32 comdlg32 oleaut32 imm32
        winmm ws2_32 ole32 user32 advapi32 shell32 shlwapi
        version setupapi wtsapi32 userenv netapi32 winspool
    )
endif()

if(WIN32)
    set_target_properties(InitfsTools PROPERTIES WIN32_EXECUTABLE TRUE)

    if(NOT QT_STATIC)
        target_link_libraries(InitfsTools PRIVATE dwmapi uxtheme)

        # Copy Qt DLLs next to the exe
        foreach(QT_DLL Qt6Core Qt6Gui Qt6Widgets Qt6Concurrent Qt6Network Qt6PrintSupport)
            add_custom_command(TARGET InitfsTools POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${QT_PATH}/bin/${QT_DLL}.dll"
                        "$<TARGET_FILE_DIR:InitfsTools>/${QT_DLL}.dll"
                COMMENT "Copying ${QT_DLL}.dll..."
            )
        endforeach()

        if(EXISTS "${QSCINTILLA_DLL}")
            add_custom_command(TARGET InitfsTools POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${QSCINTILLA_DLL}"
                        "$<TARGET_FILE_DIR:InitfsTools>/qscintilla2_qt6.dll"
                COMMENT "Copying qscintilla2_qt6.dll..."
            )
        endif()

        # Copy Qt plugins into Dependencies/
        foreach(PLUGIN_DIR platforms styles imageformats iconengines tls networkinformation generic)
            add_custom_command(TARGET InitfsTools POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                        "$<TARGET_FILE_DIR:InitfsTools>/Dependencies/${PLUGIN_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${QT_PATH}/plugins/${PLUGIN_DIR}"
                        "$<TARGET_FILE_DIR:InitfsTools>/Dependencies/${PLUGIN_DIR}"
                COMMENT "Copying plugin dir: ${PLUGIN_DIR}..."
            )
        endforeach()

        # qt.conf so Qt finds plugins inside Dependencies/
        set(QT_CONF_PATH "${CMAKE_CURRENT_BINARY_DIR}/qt.conf")
        file(WRITE "${QT_CONF_PATH}" "[Paths]\nPlugins=Dependencies\n")
        add_custom_command(TARGET InitfsTools POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${QT_CONF_PATH}"
                    "$<TARGET_FILE_DIR:InitfsTools>/qt.conf"
            COMMENT "Copying qt.conf..."
        )
    endif()

    # Always copy bcrypt.dll next to the exe (both debug and release)
    add_custom_command(TARGET InitfsTools POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:initfs_bcrypt>"
                "$<TARGET_FILE_DIR:InitfsTools>/bcrypt.dll"
        COMMENT "Copying bcrypt.dll next to exe..."
    )

endif()

# Install rules — release only gets a clean dist/
if(QT_STATIC)
    install(TARGETS InitfsTools
        RUNTIME DESTINATION .
    )

    install(FILES "$<TARGET_FILE:initfs_bcrypt>"
        DESTINATION "."
        RENAME "bcrypt.dll"
    )

    # Post-build: auto-install into dist/ for release only
    add_custom_command(TARGET InitfsTools POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            --install "${CMAKE_BINARY_DIR}"
            --prefix "$<TARGET_FILE_DIR:InitfsTools>/dist"
            --config $<CONFIG>
        COMMENT "Installing clean dist to dist/..."
    )
endif()