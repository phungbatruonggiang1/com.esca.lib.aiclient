cmake_minimum_required(VERSION 3.16)
project(esca-aiclient VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
find_package(nlohmann_json REQUIRED)

# One library, because there is one AI server with one API. Each feature
# links it rather than carrying its own copy of the transport.
add_library(esca-aiclient SHARED src/client.cpp)
target_include_directories(esca-aiclient PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(esca-aiclient PUBLIC PkgConfig::CURL nlohmann_json::nlohmann_json)
set_target_properties(esca-aiclient PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0)

install(TARGETS esca-aiclient LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/esca DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
