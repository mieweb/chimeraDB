# The translator: the shared brain used by the wire plugin (M3+) and the SQL
# gateway (M7). Deliberately has no MariaDB dependency so it can be built and
# tested without a server.
cmake_minimum_required(VERSION 3.20)
project(chimera_translator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)
# brew ships mongo-c-driver 2.x (pkg name bson2); 1.x installs are still fine.
pkg_check_modules(BSON QUIET IMPORTED_TARGET bson2)
if(NOT BSON_FOUND)
  pkg_check_modules(BSON REQUIRED IMPORTED_TARGET libbson-1.0)
endif()
message(STATUS "chimera: using ${BSON_MODULE_NAME} ${BSON_VERSION}")

add_library(chimera_translator STATIC
  src/bson.cpp
  src/changestream.cpp
  src/codec.cpp
  src/error.cpp
  src/filter.cpp
  src/id.cpp
  src/mongosh.cpp
  src/pipeline.cpp
  src/project.cpp
  src/sort.cpp
  src/sqlguard.cpp
  src/update.cpp
)
target_include_directories(chimera_translator PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(chimera_translator PUBLIC PkgConfig::BSON)
target_compile_options(chimera_translator PRIVATE -Wall -Wextra)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tests/unit ${CMAKE_CURRENT_BINARY_DIR}/tests-unit)
endif()
