#!/bin/bash
# default Linux's thp policy

echo always > /sys/kernel/mm/transparent_hugepage/enabled
echo always > /sys/kernel/mm/transparent_hugepage/defrag
echo 60000 > /sys/kernel/mm/transparent_hugepage/khugepaged/alloc_sleep_millisecs
echo 10000 > /sys/kernel/mm/transparent_hugepage/khugepaged/scan_sleep_millisecs
echo 4096 > /sys/kernel/mm/transparent_hugepage/khugepaged/pages_to_scan