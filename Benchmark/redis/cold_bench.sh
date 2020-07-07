#!/bin/sh

echo ---------------------------------- >> cold.profile
echo frag level $1
echo frag: $1 >> cold.profile
memtier_benchmark -s server --threads=8 \
--test-time=30 --ratio=1:0 --data-size-range=4096-4096 \
--clients=10 \
--pipeline=16 \
--key-maximum=5000000 --key-pattern=P:P >> cold.profile
