cmake_minimum_required(VERSION 3.10)

project(ImageViewer)

find_package(lcms2 CONFIG REQUIRED)
find_package(libjpeg-turbo CONFIG REQUIRED)

find_library(LCMS2_FAST_FLOAT_LIB NAMES lcms2_fast_float)
find_library(LCMS2_THREADED_LIB  NAMES lcms2_threaded)

add_executable(image-viewer WIN32 main.cpp decode_jpeg.cpp decode_gdiplus.cpp decode_wic.cpp)
target_compile_features(image-viewer PRIVATE cxx_std_17)
target_compile_definitions(image-viewer PRIVATE UNICODE _UNICODE)
target_link_libraries(image-viewer PRIVATE
    lcms2::lcms2
    ${LCMS2_FAST_FLOAT_LIB}
    ${LCMS2_THREADED_LIB}
    libjpeg-turbo::turbojpeg
    gdiplus comctl32)
