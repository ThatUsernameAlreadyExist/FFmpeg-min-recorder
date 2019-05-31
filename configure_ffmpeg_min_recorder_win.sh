#!/bin/sh

./configure \
--cpu=i686 \
--disable-yasm \
--target-os=mingw32 \
--extra-ldflags="-L/usr/lib" \
--extra-cflags="-I/usr/include" \
--disable-runtime-cpudetect \
--enable-small \
--disable-everything \
--disable-iconv \
--disable-protocols \
--disable-doc \
--disable-ffserver \
--disable-ffplay \
--disable-ffprobe \
--enable-static \
--enable-memalign-hack \
--enable-muxer=matroska \
--disable-shared \
--enable-demuxer=sdp \
--enable-demuxer=rtsp \
--enable-demuxer=rtp \
--enable-protocol=rtmp \
--enable-protocol=rtp \
--enable-protocol=tcp \
--disable-pthreads \
--disable-w32threads \
--disable-safe-bitstream-reader \
--enable-demuxer=h264 \
--enable-muxer=extsegment \
--enable-parser=h264 \
--enable-decoder=h264 \
--enable-protocol=file

