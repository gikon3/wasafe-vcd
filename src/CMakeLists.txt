add_library(wasafe_vcd)
add_library(wasafe::wasafe_vcd ALIAS wasafe_vcd)

set_target_properties(wasafe_vcd PROPERTIES
    OUTPUT_NAME wasafe-vcd
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

target_compile_features(wasafe_vcd PUBLIC cxx_std_23)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(wasafe_vcd PUBLIC WASAFE_VCD_STATIC_DEFINE)
endif()

target_link_libraries(wasafe_vcd PUBLIC wasafe::wasafe)

target_sources(wasafe_vcd
    PRIVATE
    emitter.cpp
    reader.cpp
    var_registry.cpp
    vcd_syntax.cpp
    scanner.cpp
    writer.cpp)

include(GenerateExportHeader)
generate_export_header(wasafe_vcd
    BASE_NAME WASAFE_VCD
    EXPORT_MACRO_NAME WASAFE_VCD_API
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/export/wasafe_vcd/export.hpp)

configure_file(
    ${PROJECT_SOURCE_DIR}/include/wasafe_vcd/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/export/wasafe_vcd/version.hpp
    @ONLY)

configure_file(
    ${PROJECT_SOURCE_DIR}/include/wasafe_vcd/config.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/export/wasafe_vcd/config.hpp
    @ONLY)

target_include_directories(wasafe_vcd
    PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/export>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})

if(WASAFE_VCD_INSTALL)
    install(TARGETS wasafe_vcd
        EXPORT wasafe_vcd-targets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/export/wasafe_vcd/export.hpp
        ${CMAKE_CURRENT_BINARY_DIR}/export/wasafe_vcd/config.hpp
        ${CMAKE_CURRENT_BINARY_DIR}/export/wasafe_vcd/version.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/wasafe_vcd)

    # ---------------------------------------------------------------------
    # CMake-пакет для потребителей: find_package(wasafe_vcd CONFIG)
    # ---------------------------------------------------------------------
    set(WASAFE_VCD_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/wasafe_vcd)

    install(EXPORT wasafe_vcd-targets
        NAMESPACE wasafe::
        FILE wasafe_vcd-targets.cmake
        DESTINATION ${WASAFE_VCD_CMAKE_DIR})

    include(CMakePackageConfigHelpers)

    configure_package_config_file(
        ${PROJECT_SOURCE_DIR}/cmake/wasafe_vcd-config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/wasafe_vcd-config.cmake
        INSTALL_DESTINATION ${WASAFE_VCD_CMAKE_DIR})

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/wasafe_vcd-config-version.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/wasafe_vcd-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/wasafe_vcd-config-version.cmake
        DESTINATION ${WASAFE_VCD_CMAKE_DIR})
endif()
