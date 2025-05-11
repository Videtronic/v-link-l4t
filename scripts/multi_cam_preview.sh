#!/bin/bash

# Usage: ./multi_cam_preview.sh WIDTH HEIGHT FPS CAM_COUNT
# Example: ./multi_cam_preview.sh 3840 2160 30 2

if [ "$#" -ne 4 ]; then
    echo "Usage: $0 WIDTH HEIGHT FPS CAM_COUNT (1-6)"
    exit 1
fi

WIDTH=$1
HEIGHT=$2
FPS=$3
CAM_COUNT=$4

if ! [[ "$CAM_COUNT" =~ ^[1-6]$ ]]; then
    echo "Error: CAM_COUNT must be between 1 and 6"
    exit 1
fi

# Positioning grid for 6 cameras (3x2 layout)
X_POS=(0 640 1280 0 640 1280)
Y_POS=(0 0    0    480 480 480)

PIPELINE=""
COMPOSITOR_CONFIG=""

for ((i=0; i<CAM_COUNT; i++)); do
    PIPELINE+="nvarguscamerasrc sensor-id=$i ! "
    PIPELINE+="video/x-raw(memory:NVMM),width=$WIDTH,height=$HEIGHT,framerate=$FPS/1 ! "
    PIPELINE+="queue ! nvvidconv ! video/x-raw(memory:NVMM) ! queue ! mix.sink_$i "

    COMPOSITOR_CONFIG+="sink_$i::xpos=${X_POS[$i]} sink_$i::ypos=${Y_POS[$i]} "
    COMPOSITOR_CONFIG+="sink_$i::width=640 sink_$i::height=480 "
done

# Add compositor and final output
PIPELINE+="nvcompositor name=mix $COMPOSITOR_CONFIG ! "
PIPELINE+="queue ! video/x-raw(memory:NVMM),width=1920,height=960,framerate=$FPS/1 ! "
PIPELINE+="nvvidconv ! queue ! fpsdisplaysink video-sink=xvimagesink"

# Run the pipeline
echo "Launching pipeline with $CAM_COUNT cameras at ${WIDTH}x${HEIGHT} @$FPS FPS..."
echo gst-launch-1.0 $PIPELINE
gst-launch-1.0 $PIPELINE

