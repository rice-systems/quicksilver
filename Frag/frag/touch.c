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

int a[2197152000];

int main(int argc, char* argv[])
{
    clock_t t, total_t; 
    double time_taken;

    /* smart start: assume we have at least 29GB free memory */
    int budget = 4000;
    printf("touch 4000 superpages and wait 15s...\n");
    for(unsigned long long i = 0; i < budget * SUPER / 4; i += SUPER/4)
    {
        /* occupy 64KB per superpage */
        a[i] = 777;
    }

    sleep(15);
    printf("Step 2: trigger background memory compactions\n");

    return 0;
}