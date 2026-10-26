#!/bin/bash

BLOCK_SIZE=512
NUM_BLOCKS=32768

if [ $# -lt 1 ]; then
    echo "Usage: $0 <path_to_image>"
    exit 1
fi

echo $1

dd if=/dev/zero of=$1 bs=$BLOCK_SIZE count=$NUM_BLOCKS status=progress