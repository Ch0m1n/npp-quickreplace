cmake_minimum_required(VERSION 3.24)

project(
  NppQuickReplace
  VERSION 0.2.0
  DESCRIPTION "Fast, local text expansion for Notepad++"
  LANGUAGES CXX RC
)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(NppQuickReplaceCore STATIC
  src/ConfigStore.cpp
  src/RuleStore.cpp
)

target_include_directories(NppQuickReplaceCore
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_compile_definitions(NppQuickReplaceCore
  PRIVATE
    NOMINMAX
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
)

add_library(NppQuickReplace SHARED
  src/Plugin.cpp
  src/NppQuickReplace.rc
)

target_include_directories(NppQuickReplace
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/notepadpp
)

target_compile_definitions(NppQuickReplace
  PRIVATE
    NOMINMAX
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(NppQuickReplace PRIVATE NppQuickReplaceCore Shell32)

set_target_properties(NppQuickReplace PROPERTIES
  PREFIX ""
  OUTPUT_NAME "NppQuickReplace"
)

if(MSVC)
  foreach(target NppQuickReplaceCore NppQuickReplace)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
  endforeach()
endif()

if(BUILD_TESTING)
  add_executable(NppQuickReplaceTests tests/RuleStoreTests.cpp)
  target_include_directories(NppQuickReplaceTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  )
  target_link_libraries(NppQuickReplaceTests PRIVATE NppQuickReplaceCore)

  if(MSVC)
    target_compile_options(NppQuickReplaceTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
  endif()

  add_test(NAME RuleStoreTests COMMAND NppQuickReplaceTests)
endif()

install(TARGETS NppQuickReplace
  RUNTIME DESTINATION NppQuickReplace
  LIBRARY DESTINATION NppQuickReplace
)
install(FILES
  samples/config.json
  samples/replacements.json
  README.md
  LICENSE
  DESTINATION NppQuickReplace
)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME "NppQuickReplace")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "NppQuickReplace-${PROJECT_VERSION}-x64")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
include(CPack)

