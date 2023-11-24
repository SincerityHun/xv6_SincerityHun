#include "param.h"
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"
#include "fcntl.h"
#include "syscall.h"
#include "traps.h"
#include "memlayout.h"

#define NUM_CHILDREN 10
#define MEMORY_LOAD 5000
void memory_load()
{
    int i;
    for (i = 0; i < MEMORY_LOAD; i++)
    {
        if (sbrk(4096) == (char *)-1)
        { // Allocate 1 page at a time
            printf(1, "sbrk failed at iteration %d\n", i);
            break;
        }
    }
}

int main()
{
    int a, b;
    swapstat(&a, &b);
}
