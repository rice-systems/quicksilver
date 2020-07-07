#!/bin/sh

# drop cache script
CLEAN=~/boot/drop.sh
# fragment script, replace with {system}-frag.sh
FRAG=~/boot/linux-frag.sh
# system script, replace with {system}.sh
POLICY=~/boot/linux.sh

result=~/replace-with-result-dir
script=~/replace-with-benchmark-script-dir
# save max memory size in ${script}/memory/${bench}, e.g. `echo 4096 > ${script}/memory/gups` for 8GB gups

date
for level in 100 50 0
do
	rm -rf ${result}/linux.profile

	cd $script
	for bench in gups pagerank svmcannotfit 
	do
		for run in 1 2 3
		do
			echo Frag-$level: $bench @ run $run

			$CLEAN
			sleep 5

			echo >> ${result}/linux.profile
			echo -o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o- >> ${result}/linux.profile
			echo $POLICY: $bench >> ${result}/linux.profile
			echo -o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o- >> ${result}/linux.profile

			# fragment memory, let the fragmented memory be inactive
			$FRAG $(cat ${script}/memory/${bench}) $level &
			sleep 60
			
			# enable policy and run
			$POLICY
			echo ---------------------------------------- >> ${result}/linux.profile
			./${bench}.sh >> ${result}/linux.profile 2>&1
			echo ---------------------------------------- >> ${result}/linux.profile

			echo -o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o- >> ${result}/linux.profile
			echo -o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o-o- >> ${result}/linux.profile
			echo >> ${result}/linux.profile

			echo stop fragmenting...
			sleep 1
			kill $(pgrep fragment)
			sleep 1
			kill $(pgrep hawkeye-profile)
			echo stopped

		done
	done

	mv ${result}/linux.profile ${result}/linux-fraglevel-${level}.profile
done
date