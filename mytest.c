#include "types.h"
#include "user.h"
#include "stat.h"

int main()
{
    // 1. getpname TEST: 각 pid에 돌아가고 있는 process name 표시
    int i;
    for (i = 1; i < 11; i++)
    {
        printf(1, "%d: ", i);
        if (getpname(i))
            printf(1, "Wrong pid\n");
    }
    exit();
}