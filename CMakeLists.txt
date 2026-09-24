cmake_minimum_required(VERSION 3.24)
project(kagee VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dependencies are fetched into .deps/ by tools/fetch-deps.ps1:
#   OBS headers (libobs + frontend API) and the Qt package OBS ships with.
set(OBS_SOURCE_DIR "${CMAKE_SOURCE_DIR}/.deps/obs-studio" CACHE PATH "obs-studio source checkout (headers)")
set(KAGEE_QT_DIR "${CMAKE_SOURCE_DIR}/.deps/qt6" CACHE PATH "Qt package matching the target OBS")
set(KAGEE_INSTALL_DIR "$ENV{ProgramData}/obs-studio/plugins/kagee" CACHE PATH "Plugin install dir")

if(NOT EXISTS "${OBS_SOURCE_DIR}/libobs/obs.h" OR NOT EXISTS "${KAGEE_QT_DIR}/lib/cmake/Qt6")
  message(FATAL_ERROR "Dependencies missing. Run: powershell -ExecutionPolicy Bypass -File tools/fetch-deps.ps1")
endif()

list(APPEND CMAKE_PREFIX_PATH "${KAGEE_QT_DIR}")
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
set(CMAKE_AUTOMOC ON)

add_library(kagee MODULE
  src/plugin-main.c
  src/common.c
  src/stage-source.c
  src/adjust-layer.c
  src/fx-lens.c
  src/fx-retro.c
  src/fx-glow.c
  src/fx-blur.c
  src/fx-glitch.c
  src/fx-trail.c
  src/fx-grade.c
  src/fx-light.c
  src/dock/dock-main.cpp
  src/dock/kagee-dock.cpp
  src/dock/kagee-dock.hpp
  src/dock/prop-view.cpp
  src/dock/prop-view.hpp
)

target_include_directories(kagee PRIVATE
  "${CMAKE_SOURCE_DIR}/sdk/include"
  "${OBS_SOURCE_DIR}/libobs"
  "${OBS_SOURCE_DIR}/deps/w32-pthreads"
  "${OBS_SOURCE_DIR}/frontend/api"
)
target_compile_definitions(kagee PRIVATE _CRT_SECURE_NO_WARNINGS HAVE_OBSCONFIG_H
  KAGEE_VERSION="${PROJECT_VERSION}")
target_compile_options(kagee PRIVATE /W3 /utf-8 $<$<COMPILE_LANGUAGE:C>:/wd4204>)

# Import libraries for the OBS DLLs, generated from the export lists in sdk/lib/*.def.
set(KAGEE_IMPORT_LIBS
  "${CMAKE_SOURCE_DIR}/sdk/lib/obs.lib"
  "${CMAKE_SOURCE_DIR}/sdk/lib/w32-pthreads.lib"
  "${CMAKE_SOURCE_DIR}/sdk/lib/obs-frontend-api.lib")
add_custom_command(
  OUTPUT ${KAGEE_IMPORT_LIBS}
  COMMAND powershell -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tools/make-obs-lib.ps1"
  DEPENDS "${CMAKE_SOURCE_DIR}/sdk/lib/obs.def" "${CMAKE_SOURCE_DIR}/sdk/lib/w32-pthreads.def"
          "${CMAKE_SOURCE_DIR}/sdk/lib/obs-frontend-api.def"
  COMMENT "Generating OBS import libraries")
add_custom_target(obs_import_libs DEPENDS ${KAGEE_IMPORT_LIBS})
add_dependencies(kagee obs_import_libs)
target_link_libraries(kagee PRIVATE ${KAGEE_IMPORT_LIBS} Qt6::Core Qt6::Gui Qt6::Widgets)

install(TARGETS kagee LIBRARY DESTINATION "${KAGEE_INSTALL_DIR}/bin/64bit")
install(DIRECTORY data/ DESTINATION "${KAGEE_INSTALL_DIR}/data")
