cmake_minimum_required(VERSION 3.12)

project(obs-audio-latency-comparator VERSION 0.1.0)

set(PLUGIN_AUTHOR "Norihiro Kamae")

# Replace `com.example.obs-plugin-template` with a unique Bundle ID for macOS releases
# (used both in the installer and when submitting the installer for notarization)
set(MACOS_BUNDLEID "net.nagater.obs-audio-latency-comparator")
set(ID_PREFIX "net.nagater.obs-audio-latency-comparator.")
set(MACOS_PACKAGE_UUID "B95319C8-08C2-4A9F-8129-756C11254BBC")
set(MACOS_INSTALLER_UUID "A4323033-BEFC-426A-B6FD-D7A3F1F3C605")
set(PLUGIN_URL "https://github.com/norihiro/obs-audio-latency-comparator") # TODO: Set the URL once released

# Replace `me@contoso.com` with the maintainer email address you want to put in Linux packages
set(LINUX_MAINTAINER_EMAIL "norihiro@nagater.net")

# TAKE NOTE: No need to edit things past this point

find_package(libobs REQUIRED)
include(cmake/ObsPluginHelpers.cmake)

configure_file(
	src/plugin-macros.h.in
	../src/plugin-macros.generated.h
)
configure_file(
	installer/installer-Windows.iss.in
	../installer/installer-Windows.generated.iss
)

configure_file(
	ci/ci_includes.sh.in
	../ci/ci_includes.generated.sh
)
configure_file(
	ci/ci_includes.cmd.in
	../ci/ci_includes.generated.cmd
)

set(PLUGIN_SOURCES
	src/plugin-main.c
	src/audio-latency-comparator.c
	src/audio-hook-buffer.c
)

add_library(${CMAKE_PROJECT_NAME} MODULE ${PLUGIN_SOURCES})

target_link_libraries(${CMAKE_PROJECT_NAME}
	OBS::libobs
)

if(OS_WINDOWS)
	# Enable Multicore Builds and disable FH4 (to not depend on VCRUNTIME140_1.DLL when building with VS2019)
	if (MSVC)
		add_definitions(/MP /d2FH4-)
	endif()

	target_link_libraries(${CMAKE_PROJECT_NAME} OBS::w32-pthreads)
endif()

if(OS_LINUX)
	target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE -Wall -Wextra)
	target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -Wl,-z,defs)
	if(ENABLE_COVERAGE)
		target_compile_options(${PROJECT_NAME} PRIVATE -coverage)
		target_link_options(${PROJECT_NAME} PRIVATE -coverage)
	endif()
endif()

if(OS_MACOS)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -fvisibility=default")

	set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES PREFIX "")
	set(MACOSX_PLUGIN_GUI_IDENTIFIER "${MACOS_BUNDLEID}")
	set(MACOSX_PLUGIN_BUNDLE_VERSION "${CMAKE_PROJECT_VERSION}")
	set(MACOSX_PLUGIN_SHORT_VERSION_STRING "1")
endif()

setup_plugin_target(${CMAKE_PROJECT_NAME})

configure_file(installer/installer-macOS.pkgproj.in installer-macOS.generated.pkgproj)
