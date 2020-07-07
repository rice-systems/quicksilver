#!/bin/sh
# relax dirty
sysctl vm.relax=1
# async
sysctl vm.reserv.enable_prezero=1
sysctl vm.reserv.pop_threshold=64
sysctl vm.reserv.pop_budget=200000
sysctl vm.reserv.wakeup_time=1
sysctl vm.reserv.wakeup_frequency=1
# sync
sysctl vm.enable_syncpromo=0
sysctl vm.reserv.sync_popthreshold=64
# adj
sysctl vm.enable_adjpromo=0
sysctl vm.adjdist=4

# budget = 1GB/s, 5% utilization
sysctl vm.reserv.migrate_budget=262144
sysctl vm.reserv.inactive_thre=5000
# sleep 1

# compact
sysctl vm.reserv.enable_compact=1