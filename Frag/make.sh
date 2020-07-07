#/bin/sh

platform=$(uname)

clang -O3 frag_compact.c -o frag_compact.x
clang -O3 -D${platform} frag_background.c -o frag_background.x
clang -O3 -D${platform} fragment_level.c -o fragment_level.x
clang -O3 touch.c -o touch.x
