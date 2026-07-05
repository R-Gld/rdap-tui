cmake_minimum_required(VERSION 3.25)

project(
  rdap-tui
  VERSION 0.0.1
  DESCRIPTION "A terminal client for the Registration Data Access Protocol"
  LANGUAGES CXX
)

option(RDAP_BUILD_TESTS "Build unit tests" ON)
option(RDAP_ENABLE_NETWORK_TESTS "Build opt-in tests that access the Internet" OFF)
option(RDAP_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)
option(RDAP_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(CURL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ftxui CONFIG REQUIRED)
find_package(Threads REQUIRED)

function(rdap_set_project_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
    if(RDAP_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(
      ${target}
      PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
    if(RDAP_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(RDAP_ENABLE_SANITIZERS)
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

add_library(
  rdap_core
  src/bootstrap_registry.cpp
  src/curl_http_client.cpp
  src/domain_name.cpp
  src/rdap_client.cpp
)
target_include_directories(rdap_core PUBLIC include)
target_link_libraries(rdap_core PUBLIC CURL::libcurl nlohmann_json::nlohmann_json)
rdap_set_project_options(rdap_core)

add_executable(rdap-tui src/main.cpp src/tui_app.cpp)
target_link_libraries(
  rdap-tui
  PRIVATE
    rdap_core
    ftxui::screen
    ftxui::dom
    ftxui::component
    Threads::Threads
)
target_compile_definitions(rdap-tui PRIVATE RDAP_TUI_VERSION="${PROJECT_VERSION}")
rdap_set_project_options(rdap-tui)

include(CTest)
if(BUILD_TESTING AND RDAP_BUILD_TESTS)
  find_package(Catch2 3 CONFIG REQUIRED)

  add_executable(
    rdap_tests
    tests/bootstrap_registry_test.cpp
    tests/domain_name_test.cpp
    tests/rdap_client_test.cpp
  )
  target_link_libraries(rdap_tests PRIVATE rdap_core Catch2::Catch2WithMain)
  rdap_set_project_options(rdap_tests)

  include(Catch)
  catch_discover_tests(rdap_tests)

  if(RDAP_ENABLE_NETWORK_TESTS)
    add_executable(rdap_network_smoke tests/network_smoke.cpp)
    target_link_libraries(rdap_network_smoke PRIVATE rdap_core)
    rdap_set_project_options(rdap_network_smoke)
    add_test(NAME rdap.network.smoke COMMAND rdap_network_smoke)
    set_tests_properties(rdap.network.smoke PROPERTIES LABELS network TIMEOUT 30)
  endif()
endif()
