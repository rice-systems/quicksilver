# Benchmarks

Detailed settings and descriptions of benchmarks used in our ATC paper:
[A comprehensive analysis of superpage mechanisms and policies](https://www.usenix.org/conference/atc20/presentation/zhu-weixi)

Benchmarks are all compiled (except for GraphChi-PR and Redis) in Linux with additonal ```-static``` compiler option.
In FreeBSD, the same Linux binaries are profiled.

## How to emulate on FreeBSD

https://www.freebsd.org/doc/handbook/linuxemu-lbc-install.html

In FreeBSD, you can run Linux binaries by emulating Linux's system calls. To link binaries
with the same libaries, compile benchmarks with ```-static``` to avoid using dynamic libraries.

## SPEC-CPU 2017

gcc flags:
```-std=c99 -m64 -static -O3 -march=skylake -ffast-math -fopenmp -fno-strict-aliasing -fgnu89-inline```

g++ flags:
```-std=c++03 -m64  -static -O3 -march=skylake -ffast-math -fopenmp```

- gcc: 602.gcc_s
- mcf: 605.mcf_s
- DSjeng: 631.deepsjeng_s
- XZ: 657.xz_s

## PARSEC 

- canneal:
	- Compile with CXXFLAGS+=-static and version=pthreads
	- Perf command ```./src/canneal 8 15000 2000 ./inputs/2500000.nets 6000```

- freqmine:
	- Compile with CXXFLAGS+=-static
	- Perf command ```./inst/amd64-linux.gcc/bin/freqmine ./inputs/webdocs_250k.dat 11000```

## GUPS

- GUPS: https://github.com/alexandermerritt/gups (commit 15536d)
	- Replace all malloc() with aligned_alloc(2MB, ...)
	- Remove all MPI API calls
	- ```gcc -O2 -static gups_serial.c -o gups.x```
	- ```./gups.x 30 4194304 1024```

## GraphChi

- Graphchi-PR: https://github.com/GraphChi/graphchi-cpp (commit 6461c8)
	- compile it without adding ```-static``` flag
	- execthreads = 4
	- loadthreads = 4
	- niothreads = 4
	- membudget_mb = 8000
	- cachesize_mb = 4000
	- preload.max_megabytes = 8000
	- io.blocksize = 1048576
	- mmap = 1
	- Preprocess twitter-2010 data: ```./bin/example_apps/pagerank file twitter-2010/twitter-2010.txt niters 3```
	- Perf 3-iteration pagerank with the same command 

## SVM

- BlockSVM: https://www.csie.ntu.edu.tw/~cjlin/libsvmtools/cdblock/liblinear-cdblock-2.20.zip
	- Compile it by adding ```-static``` flag
	- Use blocksplit to partition kddb-raw-libsvm (https://www.csie.ntu.edu.tw/~cjlin/libsvmtools/datasets/binary.html) into 4 parts
	- Perf a model training: ```./blocktrain -a cookie -m 1 -O 10000 -s 1 -v 5 -M 1 kddb-raw-libsvm.4 model```

## XSBench

- XSBench: https://github.com/ANL-CESAR/XSBench (commit c325c3)
	- Compile it by adding ```-static``` flag
	- Perf ```./XSBench -t 8 -m history -s large -l 34 -p 5000000 -G unionized```

## ANN

- annoy: https://github.com/spotify/annoy (commit 8c5930)
	- Code in ```cd ./ann``` are based on annoy
	- Compile with ```make```
	- Build a random hash table with ```./build.x```
	- Perf random queries: ```./query.x```

## Redis

- redis 4.0.14
	- Compile with ```make MALLOC=jemalloc```
	- Benchmark redis server with memtier on a client machine connected with a 40Gbps NIC
	- Scripts and redis server configs are in ./redis folder