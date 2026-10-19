# Modified by: gengxun + Codex
# Date: 2026-10-19
# Summary: Build the standalone small-body DSK renderer without mission-specific modules or data.

cmake_minimum_required(VERSION 3.20)
project(smallbody_renderer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/stage" CACHE PATH
      "Installation prefix" FORCE)
endif()

find_package(ISIS9 REQUIRED)
find_package(Eigen3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(TIFF REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

find_library(ISIS9_BULLET_COLLISION_LIBRARY NAMES BulletCollision
  HINTS "${ISIS9_ROOT_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(ISIS9_BULLET_DYNAMICS_LIBRARY NAMES BulletDynamics
  HINTS "${ISIS9_ROOT_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(ISIS9_LINEAR_MATH_LIBRARY NAMES LinearMath
  HINTS "${ISIS9_ROOT_DIR}/lib" NO_DEFAULT_PATH REQUIRED)

add_library(smallbody_first_hit STATIC
  cpp/first_hit/FirstHitRayModel.cpp)
target_include_directories(smallbody_first_hit PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/cpp/first_hit"
  "${ISIS9_ROOT_DIR}/include/bullet")
target_link_libraries(smallbody_first_hit PUBLIC ISIS9::isis
  "${ISIS9_BULLET_DYNAMICS_LIBRARY}"
  "${ISIS9_BULLET_COLLISION_LIBRARY}"
  "${ISIS9_LINEAR_MATH_LIBRARY}")

add_library(smallbody_dsk_rendering STATIC
  cpp/rendering/BulletDskRayTracer.cpp
  cpp/rendering/DskMetadata.cpp
  cpp/rendering/DskFrameRenderer.cpp
  cpp/rendering/IlluminationStateResolver.cpp
  cpp/rendering/LambertHeadlight.cpp
  cpp/rendering/OpenCvRadtanCamera.cpp
  cpp/rendering/PlateAlbedoBuilder.cpp
  cpp/rendering/PlateAlbedoTable.cpp
  cpp/rendering/Radtan5Distortion.cpp
  cpp/rendering/RenderCameraFactory.cpp
  cpp/rendering/RenderDataContract.cpp
  cpp/rendering/SolarLambert.cpp
  cpp/rendering/TiffSingleBandRaster.cpp
  cpp/rendering/TiffProductWriter.cpp
  cpp/rendering/TsaiPinholeCamera.cpp
  cpp/rendering/SurfaceAppearance.cpp)
target_include_directories(smallbody_dsk_rendering PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/cpp/rendering")
target_link_libraries(smallbody_dsk_rendering PUBLIC
  smallbody_first_hit ISIS9::isis Eigen3::Eigen
  nlohmann_json::nlohmann_json TIFF::TIFF)

add_executable(smallbody_dsk_render cpp/apps/smallbody_dsk_render.cpp)
target_link_libraries(smallbody_dsk_render PRIVATE smallbody_dsk_rendering
  ISIS9::isis nlohmann_json::nlohmann_json TIFF::TIFF)
set_target_properties(smallbody_dsk_render PROPERTIES
  INSTALL_RPATH "${ISIS9_ROOT_DIR}/lib")
install(TARGETS smallbody_dsk_render RUNTIME DESTINATION bin)

add_executable(smallbody_illumination_resolve
  cpp/apps/smallbody_illumination_resolve.cpp)
target_link_libraries(smallbody_illumination_resolve PRIVATE
  smallbody_dsk_rendering)
install(TARGETS smallbody_illumination_resolve RUNTIME DESTINATION bin)

enable_testing()
set(_test_sources
  test_dsk_frame_renderer
  test_lambert_headlight
  test_opencv_radtan_camera
  test_radtan5_distortion
  test_render_data_contract
  test_render_products
  test_solar_lambert
  test_tiff_single_band_raster
  test_tsai_pinhole_camera)
foreach(_test IN LISTS _test_sources)
  add_executable(${_test} "${CMAKE_CURRENT_SOURCE_DIR}/tests/${_test}.cpp")
  target_link_libraries(${_test} PRIVATE smallbody_dsk_rendering)
  target_compile_options(${_test} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME ${_test} COMMAND ${_test})
endforeach()

set(_dsk_fixture "${CMAKE_CURRENT_BINARY_DIR}/asymmetric-one.bds")
set(_dsk_fixture_two "${CMAKE_CURRENT_BINARY_DIR}/asymmetric-two.bds")
add_custom_command(OUTPUT "${_dsk_fixture}"
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_asymmetric_type2_dsk.py"
          "${_dsk_fixture}"
  DEPENDS tests/generate_asymmetric_type2_dsk.py VERBATIM)
add_custom_command(OUTPUT "${_dsk_fixture_two}"
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_asymmetric_type2_dsk.py"
          "${_dsk_fixture_two}" --segments 2
  DEPENDS tests/generate_asymmetric_type2_dsk.py VERBATIM)
add_custom_target(render_test_dsk DEPENDS "${_dsk_fixture}" "${_dsk_fixture_two}")

add_executable(test_bullet_dsk_renderer tests/test_bullet_dsk_renderer.cpp)
add_dependencies(test_bullet_dsk_renderer render_test_dsk)
target_link_libraries(test_bullet_dsk_renderer PRIVATE smallbody_dsk_rendering)
target_compile_definitions(test_bullet_dsk_renderer PRIVATE
  ONE_SEGMENT_DSK="${_dsk_fixture}"
  TWO_SEGMENT_DSK="${_dsk_fixture_two}")
add_test(NAME bullet_dsk_renderer COMMAND test_bullet_dsk_renderer)

add_executable(test_smallbody_dsk_render_cli tests/test_smallbody_dsk_render_cli.cpp)
add_dependencies(test_smallbody_dsk_render_cli render_test_dsk)
target_link_libraries(test_smallbody_dsk_render_cli PRIVATE
  smallbody_dsk_rendering ISIS9::isis nlohmann_json::nlohmann_json TIFF::TIFF)
target_compile_definitions(test_smallbody_dsk_render_cli PRIVATE
  SMALLBODY_DSK_RENDER_EXE="$<TARGET_FILE:smallbody_dsk_render>"
  ONE_SEGMENT_DSK="${_dsk_fixture}")
add_test(NAME smallbody_dsk_render_cli COMMAND test_smallbody_dsk_render_cli)

install(DIRECTORY python/smallbody_renderer DESTINATION share/smallbody-renderer)
install(PROGRAMS python/render_batch.py DESTINATION bin)
