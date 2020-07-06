#!/bin/bash
# aggressively tuned ingens config

echo 1 > /sys/kernel/mm/transparent_hugepage/ingens/deferred_mode
echo always > /sys/kernel/mm/transparent_hugepage/enabled
echo always > /sys/kernel/mm/transparent_hugepage/defrag
echo 1 > /sys/kernel/mm/transparent_hugepage/khugepaged/defrag
echo 0 > /sys/kernel/mm/transparent_hugepage/ingens/util_threshold
echo 100 > /sys/kernel/mm/transparent_hugepage/ingens/compact_sleep_millisecs
