#!/bin/sh

# put all benchmarks under /benchmarks
# enforce a failed umount -- this will drop all file caches
umount /benchmarks
