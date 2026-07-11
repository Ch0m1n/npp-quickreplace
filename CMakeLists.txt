cmake_minimum_required(VERSION 3.24)

project(
  NppQuickReplace
  VERSION 0.7.0
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
  src/CapturePattern.cpp
  src/SnippetTemplate.cpp
  src/ConfigStore.cpp
  src/DataPackage.cpp
  src/Localization.cpp
  src/RuleExchange.cpp
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
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)

add_library(NppQuickReplace SHARED
  src/GroupManager.cpp
  src/SettingsManager.cpp
  src/Plugin.cpp
  src/RuleManager.cpp
  src/NppQuickReplace.rc
  src/RuleTester.cpp
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
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)

target_link_libraries(NppQuickReplace PRIVATE NppQuickReplaceCore Shell32 Comctl32 UxTheme Comdlg32 Ole32)

set_target_properties(NppQuickReplace PROPERTIES
  PREFIX ""
  OUTPUT_NAME "NppQuickReplace"
)

if(MSVC)
  foreach(target NppQuickReplaceCore NppQuickReplace)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc /sdl)
  endforeach()
  target_link_options(NppQuickReplace PRIVATE /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /guard:cf)
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
  RUNTIME DESTINATION .
  LIBRARY DESTINATION .
)
install(FILES
  samples/config.json
  samples/replacements.json
  README.md
  CHANGELOG.md
  LICENSE
  THIRD_PARTY_NOTICES.md
  DESTINATION .
)
install(FILES
  docs/architecture.md
  docs/planning-ko.md
  docs/release-checklist.md
  DESTINATION docs
)
install(FILES
  third_party/nlohmann/LICENSE.MIT
  third_party/notepadpp/LICENSE.txt
  DESTINATION licenses
)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME "NppQuickReplace")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(NPPQR_PACKAGE_ARCH "x64")
if(CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
  set(NPPQR_PACKAGE_ARCH "arm64")
endif()
set(CPACK_PACKAGE_FILE_NAME "NppQuickReplace-${PROJECT_VERSION}-${NPPQR_PACKAGE_ARCH}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
include(CPack)

