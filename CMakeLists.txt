cmake_minimum_required(VERSION 3.20)
project(dox_validate VERSION 0.2.0 DESCRIPTION "EqualDoX PDF Validator" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(QPDF REQUIRED IMPORTED_TARGET libqpdf>=12)
pkg_check_modules(PUGIXML REQUIRED IMPORTED_TARGET pugixml)
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)

# The rule catalogue (catalog/*.json) is embedded at build time; it is the single source of truth for rule ids,
# titles, severities, standards references and the requirements that need human evaluation.
set(DOX_CATALOG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/catalog CACHE PATH "Directory with rules.json, wcag2.json and matrix.json")
set(EQX_GEN ${CMAKE_CURRENT_BINARY_DIR}/gen/catalog_data.cpp)
add_custom_command(
  OUTPUT ${EQX_GEN}
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_catalog.py
          ${DOX_CATALOG_DIR}/rules.json ${DOX_CATALOG_DIR}/wcag2.json ${DOX_CATALOG_DIR}/matrix.json ${EQX_GEN}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_catalog.py
          ${DOX_CATALOG_DIR}/rules.json
          ${DOX_CATALOG_DIR}/wcag2.json
          ${DOX_CATALOG_DIR}/matrix.json
  COMMENT "Generating the embedded rule catalogue")

file(GLOB_RECURSE EQX_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM EQX_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)

add_library(eqx_core STATIC ${EQX_SOURCES} ${EQX_GEN})
target_include_directories(eqx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(eqx_core PUBLIC DOX_VALIDATE_VERSION="${PROJECT_VERSION}")
target_link_libraries(eqx_core PUBLIC PkgConfig::QPDF PkgConfig::PUGIXML PkgConfig::ZLIB)
target_compile_options(eqx_core PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_executable(dox-validate ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
target_link_libraries(dox-validate PRIVATE eqx_core Threads::Threads)
install(TARGETS dox-validate RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# License and third-party notices travel with every installed binary (MPL-2.0, BSD-3-Clause, Apache-2.0 notices).
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY LICENSES DESTINATION ${CMAKE_INSTALL_DOCDIR})

# Legacy name used by existing EqualDoX tooling: a link to dox-validate in the build tree (not installed).
# `ninja eqx-validate` builds it (and dox-validate).
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/eqx-validate
  COMMAND ${CMAKE_COMMAND} -E create_symlink dox-validate ${CMAKE_CURRENT_BINARY_DIR}/eqx-validate
  DEPENDS dox-validate
  COMMENT "Linking eqx-validate -> dox-validate")
add_custom_target(legacy-name-link ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/eqx-validate)

# Unit tests (fast, no external tools): tests/unit/*.cpp, run with `ctest` or ./eqx-unit-tests.
# EQX_FIXTURES_DIR: optional directory of additional fixture PDFs; tests that need it are skipped without it.
set(EQX_FIXTURES_DIR "" CACHE PATH "Optional directory with additional fixture PDFs for unit tests")

option(DOX_BUILD_TESTS "Build the unit tests" ON)
file(GLOB EQX_UNIT_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.cpp)
if(DOX_BUILD_TESTS AND EQX_UNIT_TESTS)
  add_executable(eqx-unit-tests ${EQX_UNIT_TESTS})
  target_link_libraries(eqx-unit-tests PRIVATE eqx_core)
  if(EQX_FIXTURES_DIR)
    target_compile_definitions(eqx-unit-tests PRIVATE EQX_FIXTURES_DIR="${EQX_FIXTURES_DIR}")
  endif()
  enable_testing()
  add_test(NAME unit COMMAND eqx-unit-tests)
endif()
