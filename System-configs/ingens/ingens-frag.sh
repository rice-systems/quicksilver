#!/bin/sh
# ingens fragmentation script

~/boot/ingens-thp.sh
# trigger sync memory compaction
~/boot/frag_compact.x
# trigger async memory compaction
~/boot/frag_background.x
~/boot/fragment_level.x $1 $2
