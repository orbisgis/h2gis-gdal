cmake_minimum_required(VERSION 3.16)
project(gdal_h2gis LANGUAGES CXX)

# H2GIS driver for GDAL/OGR, built as a plugin outside of the GDAL tree.
# The H2GIS native library (h2gis.so / .dylib / .dll, C API >= 3) is loaded
# at run time: it is not needed to build the driver.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# GDAL >= 3.5 provides a CMake configuration; older versions (Ubuntu 22.04 has
# GDAL 3.4) are found by the FindGDAL module of CMake, which also defines GDAL::GDAL.
find_package(GDAL CONFIG QUIET)
if(NOT GDAL_FOUND)
    find_package(GDAL MODULE REQUIRED)
endif()
message(STATUS "GDAL ${GDAL_VERSION}")

# GDAL loads ogr_<NAME> plugins from GDAL_DRIVER_PATH and calls RegisterOGR<NAME>
add_library(ogr_H2GIS MODULE
    src/h2gis_native.cpp
    src/ogrh2gisdriver.cpp
    src/ogrh2gislayer.cpp)
set_target_properties(ogr_H2GIS PROPERTIES PREFIX "")
if(APPLE)
    # GDAL looks for plugins with the .so extension on macOS too
    set_target_properties(ogr_H2GIS PROPERTIES SUFFIX ".so")
endif()
target_link_libraries(ogr_H2GIS PRIVATE GDAL::GDAL ${CMAKE_DL_LIBS})
