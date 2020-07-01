# Quicksilver
> Why Quicksilver? Quicksilver is a super hero known with fast speed. Quicksilver sash is an equipment in League of Legend that can removes all disables (memory fragmentation in superpage management)
>
Quicksilver is a novel transparent superpage management design based on FreeBSD, which supports both anonymous and file-backed superpages.

Details are in our ATC paper:
[A comprehensive analysis of superpage mechanisms and policies](https://www.usenix.org/conference/atc20/presentation/zhu-weixi)

## Branches
- r11.2: Original FreeBSD release 11.2 code. Patched with pmc counters for Intel Kabylake CPUs.
- quicksilver: Quicksilver source code
- master: Documents, scripts and benchmarks

## Components

Quicksilver.diff: The patch of quicksilver starting from FreeBSD release 11.2
Config: System tuning scripts for Quicksilver variants (Sync-1, Sync-64, Async-64, Async-256)
Benchmark: benchmarks used in our ATC paper
Frag: Micro fragmentation benchmark to replicate our memory fragmentation (Frag-50 and Frag-100)

## Quicksilver Kernel Build

Install FreeBSD kernel image first: FreeBSD-11.2-RELEASE-amd64-memstick.img. Then, clone Quicksilver source code to /usr/src. Use the following command to build and install the kernel:

    $make buildkernel -j9
    $make installkernel -j9
    $reboot

## Quicksilver Features

- Aggressive physical superpage allocation (reservations)
- Util-threshold based superpage preparation
	- Tunable threshold
	- 4KB at-a-time before threshold (Sync)
	- All-at-once upon threshold (Sync or Async)
	- Non-temporal bulk zeroing for synchronous all-at-once preparation
- Proactive memory defragmentation
	- Scan a partially populated reservation list every second
	- Reclaim inactive physical superpages by evicting individual 4KB pages

## Benchmarks

To be finished.
