if (NOT DEFINED FFMPEG_CORE_NAME)
	set(FFMPEG_CORE_NAME ffmpeg)
endif()

add_library(${FFMPEG_CORE_NAME} INTERFACE)

# Detect OS
if (WIN32 AND NOT MINGW)
	set(FFMPEG_PREBUILTS_NAME "ffmpeg-windows")
elseif (APPLE)
	set(FFMPEG_PREBUILTS_NAME "ffmpeg-macos")
elseif (UNIX)
	set(FFMPEG_PREBUILTS_NAME "ffmpeg-linux")
elseif (MINGW)
	set(FFMPEG_PREBUILTS_NAME "ffmpeg-mingw")
else ()
	message(FATAL_ERROR "Unsupported OS.")
endif ()

# Detect Architecture
if(NOT DEFINED ARCHITECTURE)
	message(FATAL_ERROR "ARCHITECTURE variable is not set up")
elseif (ARCHITECTURE STREQUAL "x86_64")
	set(FFMPEG_PREBUILTS_NAME "${FFMPEG_PREBUILTS_NAME}-x64.zip")
elseif(ARCHITECTURE STREQUAL "arm64")
	set(FFMPEG_PREBUILTS_NAME "${FFMPEG_PREBUILTS_NAME}-arm64.zip")
elseif(APPLE AND "x86_64" IN_LIST ARCHITECTURE AND "arm64" IN_LIST ARCHITECTURE) # macOS universal
	set(FFMPEG_PREBUILTS_NAME "${FFMPEG_PREBUILTS_NAME}-universal.zip")
else ()
	message(FATAL_ERROR "Unsupported architecture.")
endif()

# Prebuilt FFmpeg release tag. Pinned: this directory is vendored, so asking git for HEAD
# would return the parent repository's commit and break the download.
set(FFMPEG_GIT_SHA "9ac4cfd195f1")

# An existing package can be tested before publishing it.
set(FFMPEG_PREBUILT_DIR "" CACHE PATH "Directory containing an unpacked FFmpeg package")
if(NOT FFMPEG_PREBUILT_DIR)
	set(FFMPEG_PREBUILT_DIR "${CMAKE_BINARY_DIR}/externals/ffmpeg-${FFMPEG_GIT_SHA}")
	set(FFMPEG_ZIP_PATH "${CMAKE_BINARY_DIR}/externals/${FFMPEG_GIT_SHA}-${FFMPEG_PREBUILTS_NAME}")
	if(NOT EXISTS "${FFMPEG_ZIP_PATH}")
		message(STATUS "Downloading FFmpeg libraries...")
		file(DOWNLOAD https://github.com/KytyPS5/ext-ffmpeg-core/releases/download/${FFMPEG_GIT_SHA}/${FFMPEG_PREBUILTS_NAME}
			"${FFMPEG_ZIP_PATH}" SHOW_PROGRESS STATUS FILE_STATUS)
		list(GET FILE_STATUS 0 STATUS_CODE)
		if(NOT STATUS_CODE EQUAL 0)
			file(REMOVE "${FFMPEG_ZIP_PATH}")
			message(FATAL_ERROR "Could not download FFmpeg ${FFMPEG_GIT_SHA}: ${FILE_STATUS}")
		endif()
	endif()
	if(NOT EXISTS "${FFMPEG_PREBUILT_DIR}/include/libavcodec/avcodec.h")
		file(MAKE_DIRECTORY "${FFMPEG_PREBUILT_DIR}")
		execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf "${FFMPEG_ZIP_PATH}"
			WORKING_DIRECTORY "${FFMPEG_PREBUILT_DIR}" RESULT_VARIABLE FFMPEG_EXTRACT_RESULT)
		if(NOT FFMPEG_EXTRACT_RESULT EQUAL 0)
			message(FATAL_ERROR "Could not extract FFmpeg libraries")
		endif()
	endif()
endif()
set(FFMPEG_LIB_PATH "${FFMPEG_PREBUILT_DIR}/lib")
if(NOT EXISTS "${FFMPEG_PREBUILT_DIR}/include/libavcodec/avcodec.h")
	message(FATAL_ERROR "FFmpeg package headers are missing")
endif()
target_include_directories(${FFMPEG_CORE_NAME} INTERFACE "${FFMPEG_PREBUILT_DIR}/include")
install(DIRECTORY "${FFMPEG_PREBUILT_DIR}/share/ffmpeg/" DESTINATION "licenses/ffmpeg")

set(LIB_PREFIX "lib")
set(LIB_EXT "a")
if (WIN32 AND NOT MINGW)
	set(LIB_PREFIX "")
	set(LIB_EXT "lib")
	target_link_libraries(${FFMPEG_CORE_NAME} INTERFACE
		"psapi;strmiids;uuid;oleaut32;shlwapi;ws2_32;ole32;user32;bcrypt")
elseif (APPLE)
	target_link_libraries(${FFMPEG_CORE_NAME} INTERFACE
		"-framework CoreServices" "-framework CoreFoundation" "-framework AudioUnit"
		"-framework AudioToolbox" "-framework CoreAudio" "-framework CoreMedia"
		"-framework VideoToolbox" "-framework CoreVideo" "-framework Security")
endif ()

target_link_libraries(${FFMPEG_CORE_NAME} INTERFACE
	"${FFMPEG_LIB_PATH}/${LIB_PREFIX}avformat.${LIB_EXT}"
	"${FFMPEG_LIB_PATH}/${LIB_PREFIX}avcodec.${LIB_EXT}"
	"${FFMPEG_LIB_PATH}/${LIB_PREFIX}swscale.${LIB_EXT}"
	"${FFMPEG_LIB_PATH}/${LIB_PREFIX}avutil.${LIB_EXT}"
	"${FFMPEG_LIB_PATH}/${LIB_PREFIX}avfilter.${LIB_EXT}"
	"${FFMPEG_LIB_PATH}/${LIB_PREFIX}swresample.${LIB_EXT}")

if(UNIX AND NOT APPLE)
	find_package(Threads REQUIRED)
	target_link_libraries(${FFMPEG_CORE_NAME} INTERFACE m Threads::Threads ${CMAKE_DL_LIBS})
endif()
