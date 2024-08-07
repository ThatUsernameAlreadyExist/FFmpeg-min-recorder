FFmpeg-min-recorder README
-------------

Minimalistic version of FFmpeg for record h264/h265-video archive in mkv format.  
Support splitting videostream to multiple files, prerecord, postrecord, loop recording with removing of old files.  
Building for mips:  
    - unpack MIPS toolchain to 'mips-gcc' folder near 'FFmpeg-min-recorder' folder  
    - ./configure_ffmpeg_min_recorder_linux_mips.sh  
    - make  
   
Usage:   
  
./ffmpeg -r 25 -vcodec h264 -i rtsp://0.0.0.0/unicast -vcodec copy -acodec copy -map 0 -dn \  
 -f extsegment -s_stop 0 -s_max_prerecord_bytes 1000000 -s_prerecord_ms 1000 -s_postrecord_ms 5000 -s_max_file_duration_ms 30000 \  
 -s_control_file /tmp/record_control -s_reserved_disk_space_mb 1000 /DCIM/video/  
   
where:  
-r 25        - stream framerate  
-vcodec h264 - for h265 used '-vcodec hevc'  
/DCIM/video/ - full path to folder for video files.   
   
Parameters:  
-s_stop [0,1]:              start with deactivated/stopped recording  
-s_max_prerecord_bytes :    max prerecord buffer size in bytes  
-s_prerecord_ms:            max prerecord buffer duration in milliseconds  
-s_postrecord_ms:           min postrecord duration in milliseconds (timeout before stopping recording)  
-s_max_file_duration_ms:    max video file duration in milliseconds  
-s_reserved_disk_space_mb:  min free disk space in megabytes, control removing of old files (0 - disable checking of free disk space and removing of old files).  
-s_only_keyframes[0,1]:     save only key frames(low fps). Note: not all players can play resulting files.  
-s_control_file:            full path to record control file. If file contain '0' - recording will be stopped, '1' - start recording. Example:  
    "flock -x /tmp/record_control echo "1" > /tmp/record_control" - start recording  
    "flock -x /tmp/record_control echo "0" > /tmp/record_control" - stop recording  
    

