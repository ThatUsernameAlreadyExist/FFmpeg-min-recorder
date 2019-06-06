#!/bin/sh
TOOLCHAIN=$(pwd)/../mips-gcc/bin
CROSS_COMPILE=$TOOLCHAIN/mips-linux-gnu-

./configure \
--arch=mips \
--disable-yasm \
--disable-mipsfpu \
--disable-mips32r2 \
--disable-mipsdspr1 \
--disable-mipsdspr2 \
--target-os=linux \
--enable-cross-compile \
--cross-prefix=$CROSS_COMPILE \
--extra-ldflags="-muclibc -static" \
--extra-cflags="-muclibc" \
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
--enable-muxer=extsegment \
--disable-shared \
--enable-demuxer=sdp \
--enable-demuxer=rtsp \
--enable-demuxer=rtp \
--enable-protocol=rtmp \
--enable-protocol=rtp \
--enable-protocol=tcp \
--disable-safe-bitstream-reader \
--enable-demuxer=h264 \
--enable-parser=h264 \
--enable-decoder=h264 \
--enable-protocol=file

