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
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)

# Default: qpdf and pugixml are the shared libraries of the distribution (pkg-config). Nothing is downloaded.
#
# DOX_STATIC_DEPS=ON builds qpdf and pugixml from their pinned upstream release archives and links them, libstdc++ and
# libgcc statically. The process then loads 4 shared objects (libc, libm, libz, libjpeg) instead of 25: a distribution
# libqpdf.so pulls in GnuTLS, OpenSSL and their dependencies, whose relocation takes about 2 ms in every process, more
# than validating a small document. qpdf is built with its native crypto provider only (RC4, AES, MD5 and SHA-2 are
# deterministic: every provider computes the same bytes). The archives are downloaded at configure time into
# DOX_DEPS_DIR and verified against the SHA-256 pinned here before they are unpacked; an archive that is already there
# with the right hash is not downloaded again, so an offline build can provide them.
option(DOX_STATIC_DEPS "Build qpdf and pugixml from pinned upstream sources and link them statically (downloads at configure time)" OFF)
if(DOX_STATIC_DEPS)
  set(DOX_QPDF_VERSION 12.4.2)
  set(DOX_QPDF_SHA256 8a58af5b6141319287c1883bec8bd1bd545b7567b7fc5e6ce5d25a1c85f36397)
  set(DOX_PUGIXML_VERSION 1.16)
  set(DOX_PUGIXML_SHA256 4cee1ca4aad395170f4c7a07824f3bdd41f28316c6e1e1090a1425b278ec0b4b)
  set(DOX_DEPS_DIR ${CMAKE_CURRENT_BINARY_DIR}/deps CACHE PATH "Where the pinned source archives are kept and unpacked")

  function(dox_fetch name url sha256)   # download (unless present), verify, unpack into ${DOX_DEPS_DIR}/src/${name}
    set(archive ${DOX_DEPS_DIR}/${name}.tar.gz)
    set(stamp ${DOX_DEPS_DIR}/src/${name}.${sha256}.unpacked)
    if(NOT EXISTS ${stamp})
      message(STATUS "Fetching ${url}")
      file(DOWNLOAD ${url} ${archive} EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON STATUS status)
      list(GET status 0 code)
      if(NOT code EQUAL 0)
        message(FATAL_ERROR "DOX_STATIC_DEPS: cannot fetch ${url}: ${status}\n"
                            "For an offline build, put the archive at ${archive} (SHA-256 ${sha256}).")
      endif()
      file(REMOVE_RECURSE ${DOX_DEPS_DIR}/src/${name})
      file(ARCHIVE_EXTRACT INPUT ${archive} DESTINATION ${DOX_DEPS_DIR}/src)
      file(TOUCH ${stamp})
    endif()
  endfunction()
  dox_fetch(qpdf-${DOX_QPDF_VERSION}
            https://github.com/qpdf/qpdf/releases/download/v${DOX_QPDF_VERSION}/qpdf-${DOX_QPDF_VERSION}.tar.gz
            ${DOX_QPDF_SHA256})
  dox_fetch(pugixml-${DOX_PUGIXML_VERSION}
            https://github.com/zeux/pugixml/releases/download/v${DOX_PUGIXML_VERSION}/pugixml-${DOX_PUGIXML_VERSION}.tar.gz
            ${DOX_PUGIXML_SHA256})

  # qpdf: its own build, static library only, native crypto only. Its other dependencies stay the system's zlib and
  # libjpeg. Sections are kept separate so that the link drops what dox-validate does not use (writer, job, JSON).
  pkg_check_modules(JPEG REQUIRED IMPORTED_TARGET libjpeg)
  enable_language(C)   # qpdf has C sources; its build uses the same compilers as this one
  include(ExternalProject)
  set(DOX_QPDF_SOURCE ${DOX_DEPS_DIR}/src/qpdf-${DOX_QPDF_VERSION})
  set(DOX_QPDF_BUILD ${DOX_DEPS_DIR}/build/qpdf-${DOX_QPDF_VERSION})
  set(DOX_QPDF_LIB ${DOX_QPDF_BUILD}/libqpdf/libqpdf.a)
  ExternalProject_Add(dox_qpdf_build
    SOURCE_DIR ${DOX_QPDF_SOURCE}
    BINARY_DIR ${DOX_QPDF_BUILD}
    PREFIX ${DOX_DEPS_DIR}/ep
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release
               -DCMAKE_INSTALL_PREFIX=${DOX_DEPS_DIR}/install   # never installed; only the static library is built
               -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
               -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
               "-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG -ffunction-sections -fdata-sections"
               "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG -ffunction-sections -fdata-sections"
               -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON
               -DREQUIRE_CRYPTO_NATIVE=ON -DUSE_IMPLICIT_CRYPTO=OFF
               -DBUILD_DOC=OFF -DINSTALL_EXAMPLES=OFF
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target libqpdf
    BUILD_BYPRODUCTS ${DOX_QPDF_LIB}
    INSTALL_COMMAND "")
  add_library(dox_qpdf STATIC IMPORTED)
  set_target_properties(dox_qpdf PROPERTIES
    IMPORTED_LOCATION ${DOX_QPDF_LIB}
    INTERFACE_INCLUDE_DIRECTORIES ${DOX_QPDF_SOURCE}/include
    INTERFACE_LINK_LIBRARIES "PkgConfig::JPEG;PkgConfig::ZLIB")
  add_dependencies(dox_qpdf dox_qpdf_build)

  add_library(dox_pugixml STATIC ${DOX_DEPS_DIR}/src/pugixml-${DOX_PUGIXML_VERSION}/src/pugixml.cpp)
  target_include_directories(dox_pugixml PUBLIC ${DOX_DEPS_DIR}/src/pugixml-${DOX_PUGIXML_VERSION}/src)
  target_compile_options(dox_pugixml PRIVATE -ffunction-sections -fdata-sections)

  set(DOX_QPDF_TARGET dox_qpdf)
  set(DOX_PUGIXML_TARGET dox_pugixml)
else()
  pkg_check_modules(QPDF REQUIRED IMPORTED_TARGET libqpdf>=12)
  pkg_check_modules(PUGIXML REQUIRED IMPORTED_TARGET pugixml)
  set(DOX_QPDF_TARGET PkgConfig::QPDF)
  set(DOX_PUGIXML_TARGET PkgConfig::PUGIXML)
endif()

# The rule catalogue (catalog/*.json) is embedded at build time; it is the single source of truth for rule ids,
# titles, severities, standards references and the requirements that need human evaluation.
set(DOX_CATALOG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/catalog CACHE PATH "Directory with rules.json, wcag2.json and matrix.json")
set(EQX_GEN ${CMAKE_CURRENT_BINARY_DIR}/gen/catalog_data.cpp)
# Rule ids and families as constant expressions (included by rules/catalog.h as "catalog_index.h"): rule lookups by
# name and the ownership table are resolved at compile time.
set(EQX_GEN_INDEX ${CMAKE_CURRENT_BINARY_DIR}/gen/catalog_index.h)
add_custom_command(
  OUTPUT ${EQX_GEN} ${EQX_GEN_INDEX}
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_catalog.py
          ${DOX_CATALOG_DIR}/rules.json ${DOX_CATALOG_DIR}/wcag2.json ${DOX_CATALOG_DIR}/matrix.json ${EQX_GEN}
          ${EQX_GEN_INDEX}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_catalog.py
          ${DOX_CATALOG_DIR}/rules.json
          ${DOX_CATALOG_DIR}/wcag2.json
          ${DOX_CATALOG_DIR}/matrix.json
  COMMENT "Generating the embedded rule catalogue")

file(GLOB_RECURSE EQX_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM EQX_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)

add_library(eqx_core STATIC ${EQX_SOURCES} ${EQX_GEN} ${EQX_GEN_INDEX})
target_include_directories(eqx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_compile_definitions(eqx_core PUBLIC DOX_VALIDATE_VERSION="${PROJECT_VERSION}")
target_link_libraries(eqx_core PUBLIC ${DOX_QPDF_TARGET} ${DOX_PUGIXML_TARGET} PkgConfig::ZLIB)
target_compile_options(eqx_core PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Start-up: the dynamic loader's work is a visible part of a small document's run time.
#   --gc-sections           unused functions and tables are not linked in (fewer pages to map and relocate)
#   -z pack-relative-relocs the relative relocations of the position-independent executable in the compact RELR
#                           format (binutils >= 2.38; the binary then needs glibc >= 2.36 at run time)
include(CheckLinkerFlag)
target_compile_options(eqx_core PRIVATE -ffunction-sections -fdata-sections)
set(DOX_LINK_OPTIONS -Wl,--gc-sections)
check_linker_flag(CXX -Wl,-z,pack-relative-relocs DOX_HAVE_RELR)
if(DOX_HAVE_RELR)
  list(APPEND DOX_LINK_OPTIONS -Wl,-z,pack-relative-relocs)
endif()
if(DOX_STATIC_DEPS)
  # Without libstdc++.so and libgcc_s.so the loader has about 3000 fewer symbols to bind. Needs the static libstdc++
  # (part of GCC on Arch and Debian, the package libstdc++-static on Fedora).
  set(CMAKE_REQUIRED_LINK_OPTIONS -static-libstdc++ -static-libgcc)
  include(CheckCXXSourceCompiles)
  check_cxx_source_compiles("#include <string>\nint main(int c, char** v) { return static_cast<int>(std::string(v[0]).size()) - c; }"
                            DOX_HAVE_STATIC_LIBSTDCXX)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
  if(DOX_HAVE_STATIC_LIBSTDCXX)
    list(APPEND DOX_LINK_OPTIONS -static-libstdc++ -static-libgcc)
  else()
    message(WARNING "DOX_STATIC_DEPS: no static libstdc++ found; libstdc++ stays a shared library")
  endif()
endif()

add_executable(dox-validate ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
target_link_libraries(dox-validate PRIVATE eqx_core Threads::Threads)
target_link_options(dox-validate PRIVATE ${DOX_LINK_OPTIONS})
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
  target_link_options(eqx-unit-tests PRIVATE ${DOX_LINK_OPTIONS})
  if(EQX_FIXTURES_DIR)
    target_compile_definitions(eqx-unit-tests PRIVATE EQX_FIXTURES_DIR="${EQX_FIXTURES_DIR}")
  endif()
  enable_testing()
  add_test(NAME unit COMMAND eqx-unit-tests)
endif()
