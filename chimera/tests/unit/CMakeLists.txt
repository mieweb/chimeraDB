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
