cmake_minimum_required(VERSION 3.20)
# macOS: the bundle must load on macOS 15 whatever macOS and SDK built it (the published v0.2.1 was
# built on macOS 26 and says minos 26.0). Ignored on other platforms; must come before project().
set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS version")
project(LTXAlphaGenOFX LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)  # OFX 1.4 Support uses throw() specs and auto_ptr
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LTXAG_DEV_BUILD "Prototype/test build: Prototype page, bypassable licence gate" ON)
# Release version (v0.1 = first release 2026-10-11; 0.1.1 = SETUP REQUIRED on the image output;
# 0.1.2 = runtime on uv's own pinned Python + C compiler check; 0.1.3 = Chunk Size, Merge, ROI;
# 0.1.4 = macOS on Apple Silicon (PyTorch-MPS), link to existing models, Setup refresh, Cancel;
# 0.1.5 = fp16 transformer on Macs where it is faster, system test before releases;
# 0.2.0 = MATTE MISSING for processed nodes (71), 6K refused with advice (72), install.sh messages;
# 0.2.1 = Frame Hold for fast motion (74) with Auto per-chunk holds from measured motion (75);
# 1.0.0 = Input Colour Auto / Bypass (76), first release by CI (77), macOS 15 minimum).
# Also the OFX plugin version (major.minor) and the release package name.
set(LTXAG_VERSION "1.0.0")
string(REPLACE "." ";" LTXAG_VERSION_PARTS "${LTXAG_VERSION}")
list(GET LTXAG_VERSION_PARTS 0 LTXAG_VERSION_MAJOR)
list(GET LTXAG_VERSION_PARTS 1 LTXAG_VERSION_MINOR)
# Build identity for the log's environment snapshot (decision 39). -DLTXAG_GIT=<hash> overrides
# it (scripts/package_release.sh builds from a copy without .git).
if(NOT LTXAG_GIT)
  execute_process(COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse --short HEAD
    OUTPUT_VARIABLE LTXAG_GIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT LTXAG_GIT)
  set(LTXAG_GIT "nogit")
endif()
set(OFX_DIR ${CMAKE_SOURCE_DIR}/third_party/openfx)
set(BUNDLE_NAME LTXAlphaGen)
set(BUNDLE_DIR ${CMAKE_BINARY_DIR}/${BUNDLE_NAME}.ofx.bundle)

# OpenFX 1.4 Support library (vendored, BSD). It still uses std::auto_ptr.
add_library(ofx_support STATIC
  ${OFX_DIR}/Support/Library/ofxsCore.cpp
  ${OFX_DIR}/Support/Library/ofxsImageEffect.cpp
  ${OFX_DIR}/Support/Library/ofxsInteract.cpp
  ${OFX_DIR}/Support/Library/ofxsLog.cpp
  ${OFX_DIR}/Support/Library/ofxsMultiThread.cpp
  ${OFX_DIR}/Support/Library/ofxsParams.cpp
  ${OFX_DIR}/Support/Library/ofxsProperty.cpp
  ${OFX_DIR}/Support/Library/ofxsPropertyValidation.cpp)
target_include_directories(ofx_support PUBLIC ${OFX_DIR}/include ${OFX_DIR}/Support/include)
target_compile_options(ofx_support PRIVATE -w -fvisibility=hidden)

# OS-specific sources behind ofx/src/platform.h (decision 64), the libraries they need, and the
# bundle's binary folder (OFX: Linux-x86-64; MacOS, as the installed Mac plugins use).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(LTXAG_PLATFORM_SOURCES ofx/src/platform_linux.cpp ofx/src/platform_linux_nvml.cpp)
  set(LTXAG_PLATFORM_LIBS dl)
  set(LTXAG_ARCH_DIR Linux-x86-64)
elseif(APPLE)
  enable_language(OBJCXX)
  set(LTXAG_PLATFORM_SOURCES ofx/src/platform_macos.cpp ofx/src/platform_macos_metal.mm)
  set(LTXAG_PLATFORM_LIBS "-framework Metal" "-framework Foundation")
  set(LTXAG_ARCH_DIR MacOS)
else()
  message(FATAL_ERROR "No platform layer for ${CMAKE_SYSTEM_NAME} (ofx/src/platform.h)")
endif()

# Pure logic, shared with the unit tests.
add_library(ltxag_core STATIC ofx/src/chunk_plan.cpp ofx/src/vram_estimate.cpp ofx/src/roi.cpp ofx/src/motion.cpp)
target_include_directories(ltxag_core PUBLIC ofx/src)

add_library(${BUNDLE_NAME} MODULE
  ofx/src/plugin.cpp
  ofx/src/log.cpp
  ofx/src/user_config.cpp
  ${LTXAG_PLATFORM_SOURCES}
  ofx/src/transport.cpp
  ofx/src/process.cpp
  ofx/src/cache.cpp
  ofx/src/marker.cpp
  ofx/src/env_info.cpp)
target_link_libraries(${BUNDLE_NAME} PRIVATE ofx_support ltxag_core ${LTXAG_PLATFORM_LIBS} pthread)
# ofxsSupportPrivate.h: the raw property suite and the instance lookup for the OpenFX 1.5 colour
# request, which the 1.4 Support library does not know (Input Colour Auto).
target_include_directories(${BUNDLE_NAME} PRIVATE ${OFX_DIR}/Support/Library)
target_compile_definitions(${BUNDLE_NAME} PRIVATE LTXAG_DEV_BUILD=$<BOOL:${LTXAG_DEV_BUILD}>
  LTXAG_VERSION="${LTXAG_VERSION}" LTXAG_GIT="${LTXAG_GIT}"
  LTXAG_VERSION_MAJOR=${LTXAG_VERSION_MAJOR} LTXAG_VERSION_MINOR=${LTXAG_VERSION_MINOR})
target_compile_options(${BUNDLE_NAME} PRIVATE -Wall -Wextra -Wno-deprecated-declarations -Wno-deprecated
  -fvisibility=hidden)
# Export only the two OFX entry points. libstdc++ is linked dynamically: Flame
# 2027.1 itself (and Sapphire) use the system /lib64/libstdc++.so.6 (GCC 11),
# so building with Rocky 9's system GCC 11 matches the host exactly.
# macOS: the vendored osxSymbols list; ld's default for undefined symbols is already "error".
if(APPLE)
  target_link_options(${BUNDLE_NAME} PRIVATE
    -Wl,-exported_symbols_list,${OFX_DIR}/Support/include/osxSymbols)
  # Minimal bundle description, as most installed Mac OFX bundles carry one.
  file(WRITE ${BUNDLE_DIR}/Contents/Info.plist
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
  <key>CFBundleExecutable</key><string>${BUNDLE_NAME}.ofx</string>
  <key>CFBundleIdentifier</key><string>com.janklier.ltx.alphagen</string>
  <key>CFBundleName</key><string>LTX Alpha Gen</string>
  <key>CFBundlePackageType</key><string>BNDL</string>
  <key>CFBundleVersion</key><string>${LTXAG_VERSION}</string>
  <key>CFBundleShortVersionString</key><string>${LTXAG_VERSION}</string>
</dict>
</plist>
")
else()
  target_link_options(${BUNDLE_NAME} PRIVATE
    -Wl,--version-script=${OFX_DIR}/Support/include/linuxSymbols -Wl,--no-undefined)
endif()
set_target_properties(${BUNDLE_NAME} PROPERTIES
  PREFIX "" SUFFIX ".ofx"
  LIBRARY_OUTPUT_DIRECTORY ${BUNDLE_DIR}/Contents/${LTXAG_ARCH_DIR})

# Python worker and Model Page helper go into the bundle's Resources
# (refreshed on every build, not only when the .ofx relinks).
add_custom_target(bundle_resources ALL
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${BUNDLE_DIR}/Contents/Resources
  COMMAND ${CMAKE_COMMAND} -E make_directory ${BUNDLE_DIR}/Contents/Resources/worker
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/worker/ltxag_worker
          ${BUNDLE_DIR}/Contents/Resources/worker/ltxag_worker
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/worker/dummy_worker.py
          ${CMAKE_SOURCE_DIR}/worker/job.py ${CMAKE_SOURCE_DIR}/worker/ltx_worker.py
          ${BUNDLE_DIR}/Contents/Resources/worker/
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/helper
          ${BUNDLE_DIR}/Contents/Resources/helper
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/helper/licence.txt
          ${BUNDLE_DIR}/Contents/Resources/licence.txt
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/flame_hook
          ${BUNDLE_DIR}/Contents/Resources/flame_hook
  DEPENDS ${BUNDLE_NAME})

enable_testing()
# Python used by the worker tests; needs numpy importable in isolated mode (-I),
# exactly as the plugin launches it. On Rocky 9 /usr/bin/python3 qualifies.
set(WORKER_TEST_PYTHON /usr/bin/python3 CACHE FILEPATH "Python for worker tests")
add_executable(test_chunk_plan ofx/tests/test_chunk_plan.cpp)
target_link_libraries(test_chunk_plan PRIVATE ltxag_core)
add_test(NAME chunk_plan COMMAND test_chunk_plan)
add_test(NAME worker_python
  COMMAND ${WORKER_TEST_PYTHON} -B -m unittest discover -s ${CMAKE_SOURCE_DIR}/worker/tests)

add_executable(test_worker_roundtrip ofx/tests/test_worker_roundtrip.cpp ofx/src/transport.cpp
  ofx/src/process.cpp ${LTXAG_PLATFORM_SOURCES})
target_link_libraries(test_worker_roundtrip PRIVATE ${LTXAG_PLATFORM_LIBS})
target_include_directories(test_worker_roundtrip PRIVATE ofx/src)
add_dependencies(test_worker_roundtrip bundle_resources)
add_test(NAME worker_roundtrip
  COMMAND test_worker_roundtrip ${WORKER_TEST_PYTHON} ${BUNDLE_DIR}/Contents/Resources/worker/dummy_worker.py)

add_executable(test_cache ofx/tests/test_cache.cpp ofx/src/cache.cpp ofx/src/transport.cpp
  ofx/src/user_config.cpp ofx/src/process.cpp ${LTXAG_PLATFORM_SOURCES})
target_link_libraries(test_cache PRIVATE ${LTXAG_PLATFORM_LIBS})
target_include_directories(test_cache PRIVATE ofx/src)
add_test(NAME cache COMMAND test_cache)

add_executable(test_roi ofx/tests/test_roi.cpp)
target_link_libraries(test_roi PRIVATE ltxag_core)
add_test(NAME roi COMMAND test_roi)

add_executable(test_marker_log ofx/tests/test_marker_log.cpp ofx/src/marker.cpp ofx/src/log.cpp
  ofx/src/user_config.cpp)
target_include_directories(test_marker_log PRIVATE ofx/src)
add_test(NAME marker_log COMMAND test_marker_log)

add_executable(test_vram_estimate ofx/tests/test_vram_estimate.cpp)
target_link_libraries(test_vram_estimate PRIVATE ltxag_core)
add_test(NAME vram_estimate COMMAND test_vram_estimate)

add_executable(test_motion ofx/tests/test_motion.cpp)
target_link_libraries(test_motion PRIVATE ltxag_core)
add_test(NAME motion COMMAND test_motion)

# System test host (systest/README.md): loads the bundle like Flame and runs end-to-end
# scenarios. Not a ctest: it needs the isolated HOME and fake windows of systest/run_systest.sh.
add_executable(ltxag_systest systest/host/systest.cpp systest/host/ofx_host.cpp ofx/src/process.cpp
  ${LTXAG_PLATFORM_SOURCES})
target_include_directories(ltxag_systest PRIVATE ofx/src systest/host ${OFX_DIR}/include)
target_link_libraries(ltxag_systest PRIVATE ${LTXAG_PLATFORM_LIBS} pthread)
add_dependencies(ltxag_systest ${BUNDLE_NAME} bundle_resources)

add_executable(test_load_bundle ofx/tests/test_load_bundle.cpp)
target_include_directories(test_load_bundle PRIVATE ${OFX_DIR}/include)
target_link_libraries(test_load_bundle PRIVATE dl)
add_dependencies(test_load_bundle ${BUNDLE_NAME})
add_test(NAME load_bundle
  COMMAND test_load_bundle ${BUNDLE_DIR}/Contents/${LTXAG_ARCH_DIR}/${BUNDLE_NAME}.ofx)
