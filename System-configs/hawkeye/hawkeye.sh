#!/bin/bash
# default hawkeye config

echo 0 > /sys/kernel/mm/transparent_hugepage/khugepaged/promotion_metric
echo 0 > /sys/kernel/mm/transparent_hugepage/khugepaged/max_cpu
echo 1 > /sys/kernel/mm/transparent_hugepage/khugepaged/defrag
echo 10000 > /sys/kernel/mm/transparent_hugepage/khugepaged/scan_sleep_millisecs

kill $(pgrep hawkeye-profile)
sleep 1
# ~/boot/hawkeye-profile.x -u root -i 10 &
~/boot/hawkeye-profile-nothreshold.x -u root -i 10 &