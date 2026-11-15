cmake_minimum_required(VERSION 3.18)
project(pedal_bridge C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Werror -fno-math-errno)

# --- Tampon partagé -----------------------------------------------------------
add_library(pb_shm STATIC common/pb_shm.c)
target_include_directories(pb_shm PUBLIC common)
set_target_properties(pb_shm PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- Bundle LV2 ---------------------------------------------------------------
set(PB_BUNDLE_DIR ${CMAKE_BINARY_DIR}/lv2/pedal-bridge.lv2)
add_library(pedal_bridge MODULE lv2/plugin.c lv2/send.c lv2/return.c)
target_link_libraries(pedal_bridge PRIVATE pb_shm m)
set_target_properties(pedal_bridge PROPERTIES
  PREFIX ""
  C_VISIBILITY_PRESET hidden
  LIBRARY_OUTPUT_DIRECTORY ${PB_BUNDLE_DIR})
target_link_options(pedal_bridge PRIVATE -Wl,-z,defs)
foreach(ttl manifest.ttl send.ttl return.ttl)
  configure_file(lv2/ttl/${ttl} ${PB_BUNDLE_DIR}/${ttl} COPYONLY)
endforeach()

# --- Outil de contrôle -------------------------------------------------------
add_executable(pb_inspect tools/pb_inspect.c)
target_link_libraries(pb_inspect PRIVATE pb_shm m)

# --- Client JACK --------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(JACK REQUIRED IMPORTED_TARGET jack)
find_library(ZITA_RESAMPLER zita-resampler REQUIRED)
add_executable(pedal_bridge_client client/pedal_bridge.cc)
set_target_properties(pedal_bridge_client PROPERTIES OUTPUT_NAME pedal_bridge)
target_link_libraries(pedal_bridge_client PRIVATE pb_shm PkgConfig::JACK ${ZITA_RESAMPLER})

# Maître du temps JACK (remplace celui, défectueux, d'Hydrogen 1.2.0-beta)
add_executable(pb_timebase tools/pb_timebase.c)
target_link_libraries(pb_timebase PRIVATE PkgConfig::JACK m)
add_executable(pb_transport tools/pb_transport.c)
target_link_libraries(pb_transport PRIVATE PkgConfig::JACK)
add_executable(pb_lag tools/pb_lag.c)
target_link_libraries(pb_lag PRIVATE PkgConfig::JACK m)
add_executable(pb_onsets tools/pb_onsets.c)
target_link_libraries(pb_onsets PRIVATE PkgConfig::JACK m)
add_executable(pb_slhit tools/pb_slhit.c)
target_link_libraries(pb_slhit PRIVATE PkgConfig::JACK)
add_executable(pb_slphase tools/pb_slphase.c)
target_link_libraries(pb_slphase PRIVATE PkgConfig::JACK m)

# --- Tests --------------------------------------------------------------------
enable_testing()
find_package(Threads REQUIRED)

add_executable(test_ring tests/test_ring.c)
target_link_libraries(test_ring PRIVATE pb_shm Threads::Threads)
add_test(NAME ring COMMAND test_ring)

add_executable(test_plugin tests/test_plugin.c)
target_link_libraries(test_plugin PRIVATE pb_shm m ${CMAKE_DL_LIBS})
# -rdynamic : le malloc intercepté du test doit aussi servir au .so chargé.
target_link_options(test_plugin PRIVATE -rdynamic)
add_test(NAME plugin COMMAND test_plugin $<TARGET_FILE:pedal_bridge> ${PB_BUNDLE_DIR}/)

add_executable(test_return tests/test_return.c)
target_link_libraries(test_return PRIVATE pb_shm m ${CMAKE_DL_LIBS})
target_link_options(test_return PRIVATE -rdynamic)
add_test(NAME return COMMAND test_return $<TARGET_FILE:pedal_bridge> ${PB_BUNDLE_DIR}/)

# Faux PiPedal pour les tests du client JACK (scripts/test-jack.sh).
add_executable(pb_fakehost tests/pb_fakehost.c)
target_link_libraries(pb_fakehost PRIVATE pb_shm m Threads::Threads ${CMAKE_DL_LIBS})
