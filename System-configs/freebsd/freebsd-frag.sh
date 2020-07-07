#!/bin/sh
# FreeBSD fragmentation script

# not necessary, but one must avoid using async-t
~/boot/sync-0.sh

# trigger sync memory compaction
~/boot/frag_compact.x
# trigger async memory compaction
~/boot/frag_background.x
~/boot/fragment_level.x $1 $2
