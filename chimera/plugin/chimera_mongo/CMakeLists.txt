# Built by the *server's* CMake, not ours: scripts/link-plugin.sh symlinks this
# directory into each server tree's plugin/ folder, which the server discovers
# automatically. That symlink is the only thing that ever touches those trees.
IF(WIN32 OR WITHOUT_SERVER)
  RETURN()
ENDIF()

# CMAKE_CURRENT_SOURCE_DIR points *through* the symlink, so resolve it to find
# the translator sitting next to us in the real chimera/ tree.
GET_FILENAME_COMPONENT(CHIMERA_PLUGIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
GET_FILENAME_COMPONENT(CHIMERA_DIR "${CHIMERA_PLUGIN_DIR}/../.." ABSOLUTE)
# Kept in step with _common.sh, which honors the same variable.
IF(DEFINED ENV{CHIMERA_OUT})
  SET(CHIMERA_TRANSLATOR_BUILD "$ENV{CHIMERA_OUT}/translator")
ELSE()
  SET(CHIMERA_TRANSLATOR_BUILD "${CHIMERA_DIR}/translator/build")
ENDIF()
SET(CHIMERA_TRANSLATOR_LIB "${CHIMERA_TRANSLATOR_BUILD}/libchimera_translator.a")

IF(NOT EXISTS "${CHIMERA_TRANSLATOR_LIB}")
  MESSAGE(WARNING
    "chimera_mongo: translator library not built - run chimera/scripts/build-translator.sh first. "
    "Skipping plugin.")
  RETURN()
ENDIF()

FIND_PACKAGE(PkgConfig REQUIRED)
PKG_CHECK_MODULES(CHIMERA_BSON QUIET bson2)
IF(NOT CHIMERA_BSON_FOUND)
  PKG_CHECK_MODULES(CHIMERA_BSON REQUIRED libbson-1.0)
ENDIF()

INCLUDE_DIRECTORIES(
  ${CHIMERA_DIR}/translator/include
  ${CHIMERA_BSON_INCLUDE_DIRS}
)
LINK_DIRECTORIES(${CHIMERA_BSON_LIBRARY_DIRS})

MYSQL_ADD_PLUGIN(chimera_mongo
  chimera_mongo.cc
  listener.cc
  wire.cc
  commands.cc
  sql.cc
  collection.cc
  cursor.cc
  oplog.cc
  projection.cc
  sqlgateway.cc
  mongogateway.cc
  mongogateway_udf.cc
  MODULE_ONLY
  COMPONENT Server
  LINK_LIBRARIES ${CHIMERA_TRANSLATOR_LIB} ${CHIMERA_BSON_LIBRARIES}
)

# The one version accommodation in the whole project: 10.11 builds the server as
# C++11, and the translator needs C++17 (std::optional, std::variant). Raising
# the standard for this target alone is safe because the plugin ABI is plain C
# structs, and it means no server tree is ever patched (ground rule 2).
IF(TARGET chimera_mongo)
  SET_TARGET_PROPERTIES(chimera_mongo PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

  # mysys symbols (my_thread_init, my_thread_end) live in the mariadbd binary,
  # not in any library a plugin can link. Apple's linker stopped implying
  # dynamic lookup for bundles, so say it outright; dyld resolves them against
  # the host executable at load time, exactly as it does on ELF platforms.
  IF(APPLE)
    TARGET_LINK_OPTIONS(chimera_mongo PRIVATE -Wl,-undefined,dynamic_lookup)
  ENDIF()
ENDIF()
