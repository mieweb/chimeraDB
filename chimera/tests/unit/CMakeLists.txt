# Translator unit tests — no server required. Built as part of
# chimera/translator's CMake project (see ../../translator/CMakeLists.txt).
find_package(doctest REQUIRED)

add_executable(translator_tests
  main.cpp
  test_codec.cpp
  test_id.cpp
  test_filter.cpp
  test_update.cpp
  test_query.cpp
  test_oplog.cpp
  test_changestream.cpp
  test_pipeline.cpp
  test_gateway.cpp
)
target_link_libraries(translator_tests PRIVATE chimera_translator doctest::doctest)
target_compile_options(translator_tests PRIVATE -Wall -Wextra)

include(doctest)
doctest_discover_tests(translator_tests)

# The storage adapter's SQL-result handling also needs no running server: this
# target supplies a test SqlSession in place of MariaDB's local connection.
add_executable(changestream_storage_tests
  main.cpp
  test_changestream_storage.cpp
  ../../plugin/chimera_mongo/changestream.cc
)
target_include_directories(changestream_storage_tests PRIVATE ../../plugin/chimera_mongo)
target_link_libraries(changestream_storage_tests PRIVATE chimera_translator doctest::doctest)
target_compile_options(changestream_storage_tests PRIVATE -Wall -Wextra)
doctest_discover_tests(changestream_storage_tests)

# Keep pruning's SQL adapter doubles separate from the change-stream doubles:
# this target links the actual oplog implementation and checks its transactions.
find_package(Threads REQUIRED)
add_executable(oplog_pruning_tests
  main.cpp
  test_oplog_pruning.cpp
  ../../plugin/chimera_mongo/oplog.cc
)
target_include_directories(oplog_pruning_tests PRIVATE ../../plugin/chimera_mongo)
target_link_libraries(oplog_pruning_tests PRIVATE chimera_translator doctest::doctest Threads::Threads)
target_compile_options(oplog_pruning_tests PRIVATE -Wall -Wextra)
doctest_discover_tests(oplog_pruning_tests)

# Authentication crypto/state and cursor isolation, independently of MariaDB.
find_package(OpenSSL REQUIRED)
add_executable(scram_tests
  main.cpp test_scram.cpp
  ../../plugin/chimera_mongo/scram.cc
  ../../plugin/chimera_mongo/cursor.cc
)
target_include_directories(scram_tests PRIVATE ../../plugin/chimera_mongo)
target_link_libraries(scram_tests PRIVATE chimera_translator doctest::doctest OpenSSL::Crypto Threads::Threads)
target_compile_options(scram_tests PRIVATE -Wall -Wextra)
doctest_discover_tests(scram_tests)

# Exercise creation authorization in the real collection adapter without a
# server. SQL and metadata helpers record side effects instead of running DDL.
add_executable(collection_creation_tests
  main.cpp
  test_collection_creation.cpp
  ../../plugin/chimera_mongo/collection.cc
)
target_include_directories(collection_creation_tests PRIVATE ../../plugin/chimera_mongo)
target_link_libraries(collection_creation_tests PRIVATE chimera_translator doctest::doctest)
target_compile_options(collection_creation_tests PRIVATE -Wall -Wextra)
doctest_discover_tests(collection_creation_tests)
