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

FILE *buddy;
int parse_buddy();

#ifdef Linux

int parse_buddy()
{
    char junk[20];

    buddy = popen("cat /proc/buddyinfo", "r");
    int order[11], total_sp = 0;
    for(int zone = 0; zone < 3; zone ++)
    {
        fscanf(buddy, "%s %s %s %s", junk, junk, junk, junk);
        for(int i = 0; i < 11; i ++)
            fscanf(buddy, "%d", &order[i]);
        if(zone > 0)
            total_sp += order[9] + order[10] * 2;
    }
    pclose(buddy);
    return total_sp;
}

#else

int parse_buddy()
{
    char junk[10];

    buddy = popen("sysctl vm.phys_free", "r");
    fscanf(buddy, "%s\n%s 0:\n\n%s %s 0:\n\n%s %s %s %s\n%s %s 0 %s %s 1\n%s %s %s %s %s %s\n",
        junk, junk, junk, junk, junk, junk, junk, junk, junk, junk, junk, junk, junk, junk, junk, junk, junk, junk);
    int n0, n1, total_sp = 0, j;
    for(int order = 12; order >= 9; order --)
    {
        fscanf(buddy, "%d ( %dK) %s %d %s %d", &j, &j, &junk, &n0, &junk, &n1);
        total_sp += (n0 + n1) << (order - 9);
    }
    pclose(buddy);
    return total_sp;
}

#endif

int main(int argc, char* argv[])
{
    clock_t t, total_t; 
    double time_taken;

    system("echo 1 > /proc/sys/vm/overcommit_memory");

    char* a = mmap(0, GB * 30 + 20*SUPER, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    char* b = mmap(0, GB * 30 + 20*SUPER, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

    int level = 0, percentage = 100;
    if(percentage == 0)
        goto hold;
    level = (100 - percentage) * level / 100;

    /* pre-touch */
    while(((uintptr_t) a & ((1 << 21) - 1)) != 0)
        a += 4096;
    while(((uintptr_t) b & ((1 << 21) - 1)) != 0)
        b += 4096;

    /* smart start: assume we have at least 29GB free memory */
    int budget = 29 * 512;
    budget -= level;
    memset(a, 1, budget * SUPER);
    for(unsigned long long i = 0; i < budget; i ++)
    {
        /* occupy 64KB per superpage */
        munmap(a + i * SUPER, SUPER - 16 * 4096);
    }

    int remain = parse_buddy() - level;
    unsigned long long i = 0;
    if(remain > 0)
    {
        memset(b, 1, remain * SUPER);  
        for(i = 0; i < remain; i ++)
        {
            /* occupy 64KB per superpage */
            munmap(b + i * SUPER, SUPER - 16 * 4096);
        }
        printf("fragmentation succeeded\n");
    }
    else
        printf("fragmentation failed\n");

    remain = parse_buddy();
    printf("we have %d superpages remained to fragment\n", remain);

    remain -= level;
    while(remain > 0)
    {
        memset(b + i * SUPER, 1, SUPER);
        munmap(b + i * SUPER, SUPER - 16 * 4096);
        i ++;
        remain = parse_buddy() - level;
    }

hold:
    printf("fragmentation succeeded, remaining sp: %d\n", parse_buddy());
    sleep(1);
    int ret = system("~/boot/touch.x");
    printf("khugepaged exited: %d\n", ret);

    return 0;
}