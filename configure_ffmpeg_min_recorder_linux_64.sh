#!/bin/sh

./configure \
--cpu=x86-64 \
--disable-yasm \
--target-os=linux \
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
--enable-demuxer=hevc \
--enable-parser=hevc \
--enable-decoder=hevc \
--enable-protocol=file

