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
