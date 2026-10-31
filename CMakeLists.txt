# RexCrash: crash and bug reports for rexglue projects, through Sentry, with a window that asks the player first.
#
#   include(path/to/RexCrash/CMakeLists.txt)
#   rexglue_setup_target(<target>)
#   rexcrash_attach(<target> [DSN <dsn>] [RELEASE <name>])
#
# The DSN is the Sentry project's ingest key. It ships inside the game, but keep it out of a public repo: leave
# DSN off and set REXCRASH_SENTRY_DSN, as a cache variable or in the environment, where the game is built. With
# no DSN the game builds and runs, and reporting stays off.
#
# sentry-native 0.16.4 is fetched on first configure; REXCRASH_SENTRY_SOURCE points at a local checkout instead.
# It builds with Sentry's native backend: like crashpad it catches the crash out of process, and unlike crashpad
# it builds with clang in its GNU mode on Windows. Three programs are staged beside the game: sentry-crash, the
# daemon that writes the crash, sentry-wer.dll on Windows, for fast-fail crashes that skip exception handlers, and
# rexcrashreporter, the window.

include_guard(GLOBAL)
include(FetchContent)

set(REXCRASH_SENTRY_SOURCE "" CACHE PATH "A sentry-native 0.16.4 checkout; empty fetches it")
set(REXCRASH_SENTRY_DSN "$ENV{REXCRASH_SENTRY_DSN}" CACHE STRING "The Sentry DSN baked into games that attach RexCrash without one")

set(SENTRY_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SENTRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SENTRY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SENTRY_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SENTRY_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(SENTRY_BACKEND "native" CACHE STRING "" FORCE)
if(WIN32)
  # Ships with Windows, unlike libcurl.
  set(SENTRY_TRANSPORT "winhttp" CACHE STRING "" FORCE)
endif()
if(REXCRASH_SENTRY_SOURCE)
  add_subdirectory(${REXCRASH_SENTRY_SOURCE} ${CMAKE_BINARY_DIR}/rexcrash_sentry EXCLUDE_FROM_ALL)
else()
  FetchContent_Declare(rexcrash_sentry GIT_REPOSITORY https://github.com/getsentry/sentry-native.git GIT_TAG 0.16.4 GIT_SHALLOW TRUE GIT_SUBMODULES "" EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(rexcrash_sentry)
endif()
foreach(sentry_target sentry sentry-crash sentry-wer)
  if(TARGET ${sentry_target})
    target_compile_options(${sentry_target} PRIVATE -w)
    # Found at runtime by their exact names, so no per-configuration postfix may reach them.
    foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL ${CMAKE_CONFIGURATION_TYPES})
      string(TOUPPER "${config}" config_upper)
      set_property(TARGET ${sentry_target} PROPERTY ${config_upper}_POSTFIX "")
    endforeach()
  endif()
endforeach()

# The window, on Windows: ImGui over Win32 and Direct3D 11, on the GPU or else on WARP, Windows' software
# rasteriser, since it runs because something already went wrong, maybe in the graphics driver. A plain Win32
# window covers Direct3D failing outright. Elsewhere there is no window yet, so crashes are only reported when the
# player has turned on sending without asking.
if(WIN32)
  set(_rexcrash_imgui ${CMAKE_CURRENT_LIST_DIR}/thirdparty/imgui)
  add_executable(rexcrashreporter WIN32 EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_LIST_DIR}/reporter/main.cpp ${CMAKE_CURRENT_LIST_DIR}/reporter/ui_imgui.cpp ${CMAKE_CURRENT_LIST_DIR}/reporter/ui_win32.cpp
    ${_rexcrash_imgui}/imgui.cpp ${_rexcrash_imgui}/imgui_draw.cpp ${_rexcrash_imgui}/imgui_tables.cpp ${_rexcrash_imgui}/imgui_widgets.cpp
    ${_rexcrash_imgui}/backends/imgui_impl_win32.cpp ${_rexcrash_imgui}/backends/imgui_impl_dx11.cpp
  )
  target_include_directories(rexcrashreporter PRIVATE ${_rexcrash_imgui} ${_rexcrash_imgui}/backends)
  # The -W entry points the reporter uses throughout.
  target_compile_definitions(rexcrashreporter PRIVATE UNICODE _UNICODE IMGUI_DISABLE_DEMO_WINDOWS)
  target_link_libraries(rexcrashreporter PRIVATE sentry d3d11 dxgi d3dcompiler dwmapi comctl32 shell32)
  set_target_properties(rexcrashreporter PROPERTIES CXX_STANDARD 20)
  foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${config}" config_upper)
    set_property(TARGET rexcrashreporter PROPERTY ${config_upper}_POSTFIX "")
  endforeach()
endif()

function(rexcrash_attach target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "DSN;RELEASE" "")
  set(dir ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
  target_sources(${target} PRIVATE ${dir}/src/rexcrash.cpp ${dir}/src/frame_stats.cpp ${dir}/src/system_report.cpp)
  target_include_directories(${target} PRIVATE ${dir}/include)
  target_link_libraries(${target} PRIVATE sentry)

  set(dsn "${arg_DSN}")
  if(NOT dsn)
    set(dsn "${REXCRASH_SENTRY_DSN}")
  endif()
  if(dsn)
    target_compile_definitions(${target} PRIVATE REXCRASH_SENTRY_DSN="${dsn}")
  else()
    message(STATUS "RexCrash: no Sentry DSN for ${target}, so reporting stays off; set REXCRASH_SENTRY_DSN to turn it on")
  endif()
  if(arg_RELEASE)
    target_compile_definitions(${target} PRIVATE REXCRASH_SENTRY_RELEASE="${arg_RELEASE}")
  endif()

  # Found beside the executable at runtime.
  foreach(helper sentry-crash sentry-wer rexcrashreporter)
    if(TARGET ${helper})
      add_dependencies(${target} ${helper})
      add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${helper}> $<TARGET_FILE_DIR:${target}> VERBATIM)
    endif()
  endforeach()
endfunction()
