#!/bin/sh

./configure \
    --disable-doc --disable-debug \
    --enable-version3 --enable-libdav1d \
    --enable-videotoolbox --enable-audiotoolbox \
    --enable-filter=yadif_videotoolbox --enable-filter=scale_vt --enable-filter=transpose_vt \
    --enable-decoder=rawvideo --enable-filter=color --enable-filter=lut --enable-filter=testsrc \
    --disable-avdevice \

