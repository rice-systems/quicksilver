#!/bin/bash
# aggressively tuned hawkeye config

echo 0 > /sys/kernel/mm/transparent_hugepage/khugepaged/promotion_metric
echo 100 > /sys/kernel/mm/transparent_hugepage/khugepaged/max_cpu
echo 1 > /sys/kernel/mm/transparent_hugepage/khugepaged/defrag
#/home/weixi/x86-MMU-Profiler/run_agg.sh
sleep 1
kill $(pgrep hawkeye-profile)
sleep 1
# ~/boot/hawkeye-profile-100MB.x -u root -i 1 &
~/boot/hawkeye-profile-nothreshold.x -u root -i 1 &
