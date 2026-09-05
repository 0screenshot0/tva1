cmake_minimum_required(VERSION 3.16)
project(tva_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    file(GLOB _qt_windows_roots
        "C:/Qt/*/msvc*_64"
        "C:/Qt/*/mingw_64"
        "C:/Qt/*/llvm-mingw_64"
    )
    if(_qt_windows_roots)
        list(SORT _qt_windows_roots)
        list(REVERSE _qt_windows_roots)
        list(APPEND CMAKE_PREFIX_PATH ${_qt_windows_roots})
    endif()
endif()

add_library(tva_core
    src/tva_core.cpp
    src/tva_core.h
)

add_executable(tva_fit
    src/main.cpp
)
target_link_libraries(tva_fit PRIVATE tva_core)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

add_executable(tva_gui
    src/gui.cpp
)
target_link_libraries(tva_gui PRIVATE tva_core Qt${QT_VERSION_MAJOR}::Widgets)

if(WIN32)
    get_filename_component(_qt_prefix "${Qt${QT_VERSION_MAJOR}_DIR}/../../.." ABSOLUTE)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_prefix}/bin")
    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET tva_gui POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}" --compiler-runtime --no-translations "$<TARGET_FILE:tva_gui>"
            COMMENT "Deploying Qt runtime files for tva_gui"
        )
    endif()
endif()

find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
    target_link_libraries(tva_core PUBLIC SQLite::SQLite3)
    target_compile_definitions(tva_core PUBLIC TVA_USE_SQLITE=1)
endif()
