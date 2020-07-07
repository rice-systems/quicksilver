#!/bin/sh

CLEAN=~/boot/drop.sh
FRAG=~/boot/frag.sh
POLICY=""
dir=freebsd
remote=root@server
os=freebsd

ssh $remote "sysctl kern.ipc.maxsockbuf=157286400"
ssh $remote "sysctl kern.ipc.maxsockbuf"

start=`date +%s`
mkdir $dir
cd $dir
for level in 100 50 0
do
	mkdir frag-$level
	cd frag-$level
	for run in 1 2 3
	do
		echo Frag-$level: redis @ run $run

		echo "Step 1: drop cache, fragment memory"
		ssh $remote "$CLEAN"
		sleep 5
		ssh $remote "screen -d -m $FRAG 8192 $level"
		sleep 60

		echo "Step 2: use policy $POLICY, run server"
		ssh $remote "screen -d -m $POLICY"
		sleep 2
		ssh $remote "screen -d -m redis-server redis.conf"
		sleep 2

		../../cold_bench.sh $level
		sleep 1
		../../warm_bench.sh $level

		echo "Step 3: stop fragmenting; stop redis"
		ssh $remote 'kill $(pgrep fragment)'
		sleep 1
		ssh $remote 'kill $(pgrep redis-server)'
		sleep 1
	done
	cd ..
done
end=`date +%s`
echo Execution time was `expr $end - $start` seconds.