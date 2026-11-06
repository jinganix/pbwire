cmake_minimum_required(VERSION 3.16)

project(pbwire VERSION 0.1.11 LANGUAGES C)

option(PBW_BUILD_PROTOC "Build the optional .proto importer (needs libprotobuf)" OFF)
option(PBW_BUILD_TESTS "Build the self-checks" ON)

include(GNUInstallDirs)

# Same gate as the Makefile (CFLAGS=-Wall -Wextra -Werror -pedantic):
# warnings fail the build on every compiler. MSVC needs its own spelling.
if(MSVC)
    add_compile_options(/W4 /WX)
    # UTF-8 source and execution charset: universal-character names like
    # \u00e9 in narrow literals must compile to UTF-8 bytes, as they do
    # under GCC/Clang, or text/JSON goldens diverge on Windows.
    add_compile_options(/utf-8)
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
    src/stream.c)

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
