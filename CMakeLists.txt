cmake_minimum_required(VERSION 3.16)

project(pbwire VERSION 0.1.14 LANGUAGES C)

option(PBW_BUILD_PROTOC "Build the optional .proto importer (needs libprotobuf)" OFF)
option(PBW_BUILD_TESTS "Build the self-checks" ON)

include(GNUInstallDirs)

# Same gate as the Makefile (CFLAGS=-Wall -Wextra -Werror -pedantic):
# warnings fail the build on every compiler. MSVC needs its own spelling.
if(MSVC)
    add_compile_options(/W4 /WX)
    # C4456 (a local hiding an outer local) is -Wshadow, which the
    # GCC/Clang gate does not enable either; keep MSVC to the same set.
    add_compile_options(/wd4456)
    # UTF-8 source and execution charset: universal-character names like
    # \u00e9 in narrow literals must compile to UTF-8 bytes, as they do
    # under GCC/Clang, or text/JSON goldens diverge on Windows.
    add_compile_options(/utf-8)
    # fopen is used for descriptor-set files; MSVC flags it as deprecated.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Werror -pedantic)
endif()

# Keep in sync with SRC in the Makefile, which stays the development gate.
set(PBW_CORE_SOURCES
    src/wire.c
    src/arena.c
    src/schema.c
    src/message.c
    src/encode.c
    src/decode.c
    src/ctx.c
    src/descriptor_set.c
    src/text.c
    src/json.c
    src/stream.c
    src/wkt.c)

add_library(pbwire ${PBW_CORE_SOURCES})
add_library(pbwire::pbwire ALIAS pbwire)
target_include_directories(pbwire
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(pbwire PUBLIC c_std_99)
set_target_properties(pbwire PROPERTIES EXPORT_NAME pbwire
    VERSION ${PROJECT_VERSION} SOVERSION 1)

# Optional: .proto text -> FileDescriptorSet, built on Google Protobuf.
# Nothing in the core links against it, and it links against nothing in the core.
if(PBW_BUILD_PROTOC)
    enable_language(CXX)
    # Prefer protobuf's own CMake package: it carries its abseil dependency,
    # which CMake's FindProtobuf module does not add.
    find_package(protobuf CONFIG QUIET)
    if(NOT TARGET protobuf::libprotobuf)
        find_package(Protobuf REQUIRED)
    endif()

    add_library(pbwire-protoc
        src/protoc/importer.cc
        src/protoc/descriptor_set.cc
        src/protoc/error.cc)
    add_library(pbwire::protoc ALIAS pbwire-protoc)
    target_include_directories(pbwire-protoc
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_compile_features(pbwire-protoc PUBLIC cxx_std_17)
    # importer.h/descriptor_set.h/error.h expose no protobuf types, but the
    # implementation needs libprotobuf (which has held compiler/importer since 36).
    target_link_libraries(pbwire-protoc PUBLIC protobuf::libprotobuf)
    set_target_properties(pbwire-protoc PROPERTIES
        CXX_EXTENSIONS OFF
        EXPORT_NAME protoc)
endif()

if(PBW_BUILD_TESTS)
    enable_testing()
    # The C struct codec and the embedded facade are separate sources the
    # Makefile compiles per test; as object libraries every target that needs
    # them links them once (MSVC has no -I/-o flags to hand them).
    add_library(pbwire_struct_runtime OBJECT bindings/c/runtime/pbw_struct.c)
    target_include_directories(pbwire_struct_runtime PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    # The same source again with the allocation hooks, so the struct codec's
    # NOMEM paths join the test_nofail sweep (the Makefile does this with a
    # second object file and the same -D renames).
    add_library(pbwire_struct_nofail OBJECT bindings/c/runtime/pbw_struct.c)
    target_include_directories(pbwire_struct_nofail PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/bindings/c/runtime)
    target_compile_definitions(pbwire_struct_nofail PRIVATE
        malloc=pbw_fail_malloc calloc=pbw_fail_calloc
        realloc=pbw_fail_realloc free=pbw_fail_free)
    add_library(pbwe_runtime OBJECT bindings/embedded/runtime/pbwe.c)
    target_include_directories(pbwe_runtime PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/bindings/embedded/runtime)
    # Optional on purpose: without pthreads (e.g. bare MSVC) test_threads is
    # skipped but everything else still builds, so Windows can configure.
    # The header check matters: FindThreads can report TRUE on MSVC while
    # pthread.h is absent, and the stress test includes it unconditionally.
    find_package(Threads)
    include(CheckIncludeFile)
    check_include_file(pthread.h HAVE_PTHREAD_H)

    add_executable(test_pbwire tests/test_pbwire.c)
    target_link_libraries(test_pbwire PRIVATE pbwire)
    # The self-check also exercises the internal limits and tag helpers.
    target_include_directories(test_pbwire PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME test_pbwire COMMAND test_pbwire)

    if(Threads_FOUND AND HAVE_PTHREAD_H)
        add_executable(test_threads tests/test_threads.c)
        target_link_libraries(test_threads PRIVATE pbwire Threads::Threads)
        add_test(NAME test_threads COMMAND test_threads)
    else()
        message(STATUS "Threads/pthreads unavailable: skipping test_threads")
    endif()

    add_executable(test_descriptor tests/test_descriptor.c)
    target_link_libraries(test_descriptor PRIVATE pbwire)
    add_test(NAME test_descriptor COMMAND test_descriptor)

    # The remaining suites the Makefile gate runs, so `ctest` covers the same
    # checks on every platform (including MSVC, where `make test` cannot run).
    # The optional protoc importer test below stays out unless PBW_BUILD_PROTOC
    # is ON (it needs libprotobuf).
    add_executable(test_malformed tests/test_malformed.c)
    target_link_libraries(test_malformed PRIVATE pbwire)
    add_test(NAME test_malformed COMMAND test_malformed)

    # Allocation-failure sweep: the core is built a second time with every
    # allocation site renamed to a counting hook (tests/test_nofail.c), so each
    # iteration fails exactly one allocation. test_embedded_nofail runs the
    # same hooked core through the embedded facade and asserts the
    # no-allocation steady state.
    add_library(pbwire_nofail STATIC ${PBW_CORE_SOURCES})
    target_include_directories(pbwire_nofail
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(pbwire_nofail PRIVATE
        malloc=pbw_fail_malloc calloc=pbw_fail_calloc
        realloc=pbw_fail_realloc free=pbw_fail_free)

    add_executable(test_nofail tests/test_nofail.c)
    target_include_directories(test_nofail PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/bindings/c/runtime)
    target_link_libraries(test_nofail PRIVATE pbwire_nofail
        $<TARGET_OBJECTS:pbwire_struct_nofail>)
    add_test(NAME test_nofail COMMAND test_nofail)

    add_executable(test_embedded_nofail
        bindings/embedded/tests/test_embedded.c
        bindings/embedded/runtime/pbwe.c)
    target_compile_definitions(test_embedded_nofail PRIVATE
        PBWE_HAVE_ALLOC_HOOKS)
    target_include_directories(test_embedded_nofail PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/bindings/embedded/runtime)
    target_link_libraries(test_embedded_nofail PRIVATE pbwire_nofail)
    add_test(NAME test_embedded_nofail COMMAND test_embedded_nofail)
    set_tests_properties(test_embedded_nofail PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(test_struct bindings/c/tests/test_struct.c)
    target_include_directories(test_struct PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/bindings/c/runtime
        ${CMAKE_CURRENT_SOURCE_DIR}/bindings/c/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/bindings/c/examples)
    target_link_libraries(test_struct PRIVATE pbwire
        $<TARGET_OBJECTS:pbwire_struct_runtime>)
    add_test(NAME test_struct COMMAND test_struct)
    set_tests_properties(test_struct PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(test_embedded bindings/embedded/tests/test_embedded.c)
    target_include_directories(test_embedded PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/bindings/embedded/runtime)
    target_link_libraries(test_embedded PRIVATE pbwire
        $<TARGET_OBJECTS:pbwe_runtime>)
    add_test(NAME test_embedded COMMAND test_embedded)
    set_tests_properties(test_embedded PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(example_addressbook bindings/c/examples/addressbook.c)
    target_include_directories(example_addressbook PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/bindings/c/runtime)
    target_link_libraries(example_addressbook PRIVATE pbwire
        $<TARGET_OBJECTS:pbwire_struct_runtime>)
    add_test(NAME example_addressbook COMMAND example_addressbook)
    set_tests_properties(example_addressbook PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(example_gateway bindings/c/examples/gateway.c)
    target_link_libraries(example_gateway PRIVATE pbwire)
    add_test(NAME example_gateway COMMAND example_gateway)
    set_tests_properties(example_gateway PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(example_embedded bindings/embedded/examples/roundtrip.c)
    target_include_directories(example_embedded PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/bindings/embedded/runtime)
    target_link_libraries(example_embedded PRIVATE pbwire
        $<TARGET_OBJECTS:pbwe_runtime>)
    add_test(NAME example_embedded COMMAND example_embedded)
    set_tests_properties(example_embedded PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    if(PBW_BUILD_PROTOC)
        add_executable(test_importer tests/protoc/test_importer.cc)
        target_link_libraries(test_importer PRIVATE pbwire-protoc pbwire)
        target_compile_definitions(test_importer PRIVATE
            PBW_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/protoc/data")
        add_test(NAME test_importer COMMAND test_importer)
    endif()
endif()

install(TARGETS pbwire
    EXPORT pbwireTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/pbwire.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(PBW_BUILD_PROTOC)
    install(TARGETS pbwire-protoc
        EXPORT pbwireTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY include/pbwire-protoc
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

install(EXPORT pbwireTargets
    NAMESPACE pbwire::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pbwire)

include(CMakePackageConfigHelpers)
if(PBW_BUILD_PROTOC)
    set(PBW_WITH_PROTOC TRUE)
else()
    set(PBW_WITH_PROTOC FALSE)
endif()
configure_package_config_file(cmake/pbwireConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/pbwireConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pbwire)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/pbwireConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/pbwireConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/pbwireConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pbwire)
