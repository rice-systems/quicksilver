#!/bin/sh

echo ---------------------------------- >> warm.profile
echo frag level $1
echo frag: $1 >> warm.profile
memtier_benchmark -s server --threads=8 \
--test-time=300 --ratio=5:5 --data-size-range=4096-4096 \
--clients=10 \
--pipeline=16 \
--key-maximum=5000000 --key-pattern=P:P >> warm.profile
