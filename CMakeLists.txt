cmake_minimum_required(VERSION 3.24)
project(QRScreen LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "QRScreen is a Windows-only application.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build the application and ZXing with the static MSVC runtime. The resulting
# executable does not require the Visual C++ Redistributable to be installed.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
endif()

# QR-only, decoder-only ZXing build keeps the executable smaller.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ZXING_READERS ON CACHE BOOL "" FORCE)
set(ZXING_WRITERS OFF CACHE STRING "" FORCE)
set(ZXING_C_API OFF CACHE BOOL "" FORCE)
set(ZXING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZXING_EXAMPLES_QT OFF CACHE BOOL "" FORCE)
set(ZXING_BLACKBOX_TESTS OFF CACHE BOOL "" FORCE)
set(ZXING_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(ZXING_TEST_DOTNET OFF CACHE BOOL "" FORCE)
set(ZXING_TEST_GO OFF CACHE BOOL "" FORCE)
set(ZXING_TEST_INSTALL OFF CACHE BOOL "" FORCE)
set(ZXING_PYTHON_MODULE OFF CACHE BOOL "" FORCE)
set(ZXING_ENABLE_1D OFF CACHE BOOL "" FORCE)
set(ZXING_ENABLE_AZTEC OFF CACHE BOOL "" FORCE)
set(ZXING_ENABLE_DATAMATRIX OFF CACHE BOOL "" FORCE)
set(ZXING_ENABLE_MAXICODE OFF CACHE BOOL "" FORCE)
set(ZXING_ENABLE_PDF417 OFF CACHE BOOL "" FORCE)
set(ZXING_ENABLE_QRCODE ON CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
    zxing
    URL https://github.com/zxing-cpp/zxing-cpp/archive/refs/tags/v3.1.1.tar.gz
    URL_HASH SHA256=7286b1e6ade66fe82b7c8208b4595deeb55d6486b410834fdc65702f46650542
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(zxing)

add_executable(QRScreen WIN32 src/main.cpp)
target_compile_definitions(QRScreen PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(QRScreen PRIVATE ZXing::ZXing user32 gdi32 shell32 msimg32)

if(MSVC)
    target_compile_options(QRScreen PRIVATE /W4 /utf-8 /permissive-)
endif()

set_target_properties(QRScreen PROPERTIES
    OUTPUT_NAME "QRScreen"
    WIN32_EXECUTABLE TRUE
)
