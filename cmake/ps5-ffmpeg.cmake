# FFmpeg for the PS5, built from source at configure time.
#
# ffmpeg-core has no prebuilt for the console. Its "freebsd" build is made on Linux and imports glibc
# (__memcpy_chk, open64, getauxval), which the console's libc does not export. This builds the same FFmpeg
# release as ffmpeg-core's headers (7.1.1), with the same components (external/ffmpeg/ffmpeg.patch), and
# provides the `ffmpeg` target that external/ffmpeg provides elsewhere.

set(FFMPEG_PS5_VERSION 7.1.1)
set(FFMPEG_PS5_ARCHIVE "${CMAKE_BINARY_DIR}/external/ffmpeg-${FFMPEG_PS5_VERSION}.tar.gz")
set(FFMPEG_PS5_SOURCE "${CMAKE_BINARY_DIR}/external/ffmpeg-src")
set(FFMPEG_PS5_INSTALL "${CMAKE_BINARY_DIR}/external/ffmpeg-ps5")
set(FFMPEG_PS5_LIBRARIES avfilter avformat avcodec swresample swscale avutil)

set(FFMPEG_PS5_BUILT TRUE)
foreach(library IN LISTS FFMPEG_PS5_LIBRARIES)
	if(NOT EXISTS "${FFMPEG_PS5_INSTALL}/lib/lib${library}.a")
		set(FFMPEG_PS5_BUILT FALSE)
	endif()
endforeach()

if(NOT FFMPEG_PS5_BUILT)
	if(NOT EXISTS "${FFMPEG_PS5_ARCHIVE}")
		message(STATUS "Downloading FFmpeg ${FFMPEG_PS5_VERSION} source...")
		file(DOWNLOAD https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n${FFMPEG_PS5_VERSION}.tar.gz
			"${FFMPEG_PS5_ARCHIVE}"
			SHOW_PROGRESS
			EXPECTED_HASH SHA256=f117507dc501f2a6c11f9241d8d0c3213846cfad91764361af37befd6b6c523d)
	endif()

	file(REMOVE_RECURSE "${FFMPEG_PS5_SOURCE}")
	file(MAKE_DIRECTORY "${FFMPEG_PS5_SOURCE}")
	execute_process(COMMAND tar xf "${FFMPEG_PS5_ARCHIVE}" -C "${FFMPEG_PS5_SOURCE}" --strip-components=1)

	# 7.1.1 builds h2645_sei.o for the H.264 decoder without aom_film_grain.o, which it calls; only the HEVC
	# decoder, which Vita3K leaves out, brings that in
	file(READ "${FFMPEG_PS5_SOURCE}/libavcodec/Makefile" FFMPEG_PS5_MAKEFILE)
	string(REPLACE "OBJS-$(CONFIG_H264_SEI)                += h264_sei.o h2645_sei.o"
		"OBJS-$(CONFIG_H264_SEI)                += h264_sei.o h2645_sei.o aom_film_grain.o"
		FFMPEG_PS5_MAKEFILE "${FFMPEG_PS5_MAKEFILE}")
	file(WRITE "${FFMPEG_PS5_SOURCE}/libavcodec/Makefile" "${FFMPEG_PS5_MAKEFILE}")

	find_program(FFMPEG_PS5_NM NAMES llvm-nm llvm-nm-18 nm REQUIRED)

	# Only what Vita3K uses, as ffmpeg-core configures it
	set(FFMPEG_PS5_COMPONENTS
		--disable-everything
		--enable-decoder=aac --enable-decoder=aac_latm --enable-decoder=atrac3 --enable-decoder=atrac3p
		--enable-decoder=atrac9 --enable-decoder=mp3 --enable-decoder=pcm_s16le --enable-decoder=pcm_s8
		--enable-decoder=h264 --enable-decoder=mpeg4 --enable-decoder=mpeg2video --enable-decoder=mjpeg
		--enable-decoder=mjpegb
		--enable-encoder=pcm_s16le --enable-encoder=ffv1 --enable-encoder=mpeg4 --enable-encoder=mjpeg
		--enable-muxer=avi
		--enable-demuxer=h264 --enable-demuxer=m4v --enable-demuxer=mp3 --enable-demuxer=mpegvideo
		--enable-demuxer=mpegps --enable-demuxer=mjpeg --enable-demuxer=mov --enable-demuxer=avi
		--enable-demuxer=aac --enable-demuxer=pmp --enable-demuxer=oma --enable-demuxer=pcm_s16le
		--enable-demuxer=pcm_s8 --enable-demuxer=wav
		--enable-parser=h264 --enable-parser=mpeg4video --enable-parser=mpegaudio --enable-parser=mpegvideo
		--enable-parser=mjpeg --enable-parser=aac --enable-parser=aac_latm
		--enable-protocol=file
		--enable-bsf=mjpeg2jpeg)

	message(STATUS "Building FFmpeg ${FFMPEG_PS5_VERSION} for the PS5...")
	# The console's userland is FreeBSD. x86asm needs nasm, which the build host may not have
	execute_process(
		COMMAND ./configure "--prefix=${FFMPEG_PS5_INSTALL}"
			--enable-cross-compile --target-os=freebsd --arch=x86_64
			"--cc=${CMAKE_C_COMPILER}" "--cxx=${CMAKE_CXX_COMPILER}" "--ar=${CMAKE_AR}" "--ranlib=${CMAKE_RANLIB}"
			"--nm=${FFMPEG_PS5_NM}" "--strip=${CMAKE_STRIP}"
			--enable-static --disable-shared --enable-pic --disable-doc --disable-programs --disable-avdevice
			--enable-runtime-cpudetect --disable-autodetect --disable-x86asm
			${FFMPEG_PS5_COMPONENTS}
		WORKING_DIRECTORY "${FFMPEG_PS5_SOURCE}"
		OUTPUT_QUIET
		RESULT_VARIABLE FFMPEG_PS5_RESULT)
	if(FFMPEG_PS5_RESULT EQUAL 0)
		execute_process(COMMAND make -j${CPU_COUNT} install
			WORKING_DIRECTORY "${FFMPEG_PS5_SOURCE}"
			OUTPUT_QUIET
			RESULT_VARIABLE FFMPEG_PS5_RESULT)
	endif()
	if(NOT FFMPEG_PS5_RESULT EQUAL 0)
		message(FATAL_ERROR "Building FFmpeg for the PS5 failed (${FFMPEG_PS5_RESULT}); see ${FFMPEG_PS5_SOURCE}/ffbuild/config.log")
	endif()
endif()

add_library(ffmpeg INTERFACE)
# ffmpeg-core's headers, which add the internal ones Vita3K uses (libavcodec/codec_internal.h) to the release's
target_include_directories(ffmpeg INTERFACE "${CMAKE_SOURCE_DIR}/external/ffmpeg/include")
foreach(library IN LISTS FFMPEG_PS5_LIBRARIES)
	target_link_libraries(ffmpeg INTERFACE "${FFMPEG_PS5_INSTALL}/lib/lib${library}.a")
endforeach()
