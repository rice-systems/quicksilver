#!/bin/sh
# hawkeye fragmentation script

sleep 1
# switch to Linux's default
~/boot/hawkeye-thp.sh
# trigger sync memory compaction
~/boot/frag_compact.x
# trigger async memory compaction
~/boot/frag_background.x
~/boot/fragment_level.x $1 $2
