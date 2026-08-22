cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED WASAFE_VCD_VERSION)
    set(WASAFE_VCD_VERSION 0.0.0)
endif()

project(wasafe_vcd
    VERSION ${WASAFE_VCD_VERSION}
    DESCRIPTION "${WASAFE_VCD_DESCRIPTION}"
    HOMEPAGE_URL "${WASAFE_VCD_HOMEPAGE_URL}"
    LANGUAGES CXX)

if(NOT PROJECT_VERSION_MINOR)
    set(PROJECT_VERSION_MINOR 0)
endif()
if(NOT PROJECT_VERSION_PATCH)
    set(PROJECT_VERSION_PATCH 0)
endif()

# ---------------------------------------------------------------------------
# Опции сборки
# ---------------------------------------------------------------------------
option(WASAFE_VCD_INSTALL "Генерировать правила установки" ${PROJECT_IS_TOP_LEVEL})
option(WASAFE_VCD_BUILD_TESTS "Собирать тесты" ${PROJECT_IS_TOP_LEVEL})
option(WASAFE_VCD_BUILD_BENCHMARKS "Собирать бенчмарки" OFF)

# ---------------------------------------------------------------------------
# Общие настройки компилятора
# ---------------------------------------------------------------------------
# Решения уровня всего дерева сборки: при подключении через add_subdirectory
# раскладку и тип сборки задаёт родительский проект.
if(PROJECT_IS_TOP_LEVEL)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Тип сборки" FORCE)
    endif()

    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)

    include(CTest)
endif()

include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Зависимости
# ---------------------------------------------------------------------------
find_package(wasafe CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Основная библиотека
# ---------------------------------------------------------------------------
if(WASAFE_VCD_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()

add_subdirectory(src)

if(WASAFE_VCD_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Установка публичных заголовков
# ---------------------------------------------------------------------------
if(WASAFE_VCD_INSTALL)
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} PATTERN "*.in" EXCLUDE)
endif()
