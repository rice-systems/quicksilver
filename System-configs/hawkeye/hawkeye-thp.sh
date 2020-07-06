#!/bin/bash
# switch to Linux's default (need to patch hawkeye source code)

kill $(pgrep hawkeye-profile)
echo 10000 > /sys/kernel/mm/transparent_hugepage/khugepaged/scan_sleep_millisecs
echo 60000 > /sys/kernel/mm/transparent_hugepage/khugepaged/alloc_sleep_millisecs
echo 3 > /sys/kernel/mm/transparent_hugepage/khugepaged/promotion_metric
echo 0 > /sys/kernel/mm/transparent_hugepage/khugepaged/max_cpu
echo 1 > /sys/kernel/mm/transparent_hugepage/khugepaged/defrag