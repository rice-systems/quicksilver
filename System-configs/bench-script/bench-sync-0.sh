#!/bin/sh

# For freebsd
~/boot/linux.sh
CLEAN=~/boot/drop.sh
FRAG=~/boot/freebsd-frag.sh
POLICY=~/boot/sync-0-compact.sh

result=~/replace-with-result-dir
script=~/replace-with-benchmark-script-dir
os=freebsd

start=`date +%s`
for level in 100 50 0
do
	rm -rf ${result}/${os}.profile

	cd $script
	for bench in gups pagerank svmcannotfit XSBench ann \
	canneal freqmine \
	602.gcc_s 605.mcf_s 631.deepsjeng_s 657.xz_s gups-4
	do
		for run in 1 2 3
		do
			echo Frag-$level: $bench @ run $run

			$CLEAN
			sleep 5

			echo >> ${result}/${os}.profile
			echo -o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o- >> ${result}/${os}.profile
			echo $POLICY: $bench >> ${result}/${os}.profile
			echo -o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o- >> ${result}/${os}.profile

			# fragment memory
			$FRAG $(cat ${script}/memory/${bench}) $level &
			sleep 60

			# check system before running
			# echo ---------------------------------------- >> ${result}/${os}.profile
			~/boot/system.sh >> ${result}/${os}.profile
			# echo ---------------------------------------- >> ${result}/${os}.profile
			
			# enable policy and run
			$POLICY
			echo ---------------------------------------- >> ${result}/${os}.profile
			./${bench}.sh >> ${result}/${os}.profile 2>&1
			echo ---------------------------------------- >> ${result}/${os}.profile

			# check system after running
			# echo ---------------------------------------- >> ${result}/${os}.profile
			~/boot/system.sh >> ${result}/${os}.profile
			# echo ---------------------------------------- >> ${result}/${os}.profile
			echo -o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o- >> ${result}/${os}.profile
			echo -o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o- >> ${result}/${os}.profile
			echo >> ${result}/${os}.profile

			echo stop fragmenting...
			sleep 1
			kill $(pgrep fragment)
			sleep 1
			echo stopped

		done
	done

	mv ${result}/${os}.profile ${result}/${os}-fraglevel-${level}.profile
done
end=`date +%s`
echo Execution time was `expr $end - $start` seconds.