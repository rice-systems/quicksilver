# System configurations

Configurations of systems evaluated in our ATC paper
[A comprehensive analysis of superpage mechanisms and policies](https://www.usenix.org/conference/atc20/presentation/zhu-weixi)

## hawkeye

- hawkeye.patch: A patch for commit 9b5a97 (https://github.com/apanwariisc/HawkEye) that enables default Linux THP
- hawkeye-thp.sh: The script to enable default Linux THP in HawkEye
- hawkeye-frag.sh: The fragmentation script to fragment memory in HawkEye
- hawkeye.sh: The script to enable default hawkeye settings
- hawkeye-aggressively-tuned.sh: The script to enable hawkeye* settings used in our ATC paper

## ingens

- ingens-thp.sh: The script to enable default Linux THP in Ingens
- ingens-frag.sh: The fragmentation script to fragment memory in Ingens
- ingens.sh: The script to enable default ingens settings
- ingens-aggressively-tuned.sh: The script to enable ingens* settings used in our ATC paper


## linux

- linux.sh: The script to enable default Linux settings
- linux-frag.sh: The fragmentation script to fragment memory in Linux
- drop.sh: The script to drop all file caches in Linux

## bench-script

- bench.sh: An example of benchmark script for Linux. 