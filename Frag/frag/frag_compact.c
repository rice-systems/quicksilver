#include <strings.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#define GB 1ULL*1024*1024*1024
#define CACHE_SIZE 8ULL*1024*1024
#define SUPER   2ULL*1024*1024
#define SMALL   1ULL*512*1024

int main(int argc, char* argv[])
{
    clock_t t, total_t; 
    double time_taken;

    system("echo 1 > /proc/sys/vm/overcommit_memory");
    char* a = mmap(0, GB * 30 + 20*SUPER, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    char* b = mmap(0, GB * 30 + 20*SUPER, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

    int budget = 30;
    if(argc == 2)
        budget = atoi(argv[1]);

    /* pre-touch */
    while(((uintptr_t) a & ((1 << 21) - 1)) != 0)
        a += 4096;
    while(((uintptr_t) b & ((1 << 21) - 1)) != 0)
        b += 4096;

    t = clock();
    memset(a, 1, GB * 30);
    t = clock() - t;

    double delta = ((double) t) / ((double) CLOCKS_PER_SEC);

    printf("Time of touching 30 GB in A in %.2f ms\n", delta * 1000);

    for(unsigned long long i = 0; i < 1 * 512; i ++)
        munmap(a + GB * 0 + i * SUPER, SUPER - 4096);

    for(unsigned long long i = 0; i < 3 * 512; i ++)
        *(int *) (b + i * SUPER) = 0xfff;
    printf("Step 1: Trigger memory compaction under memory pressure\n");
    return 0;
}