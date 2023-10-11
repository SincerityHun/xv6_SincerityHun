#include "types.h"
#include "user.h"
#include "stat.h"

int main()
{
    // SYSTEMCALL TEST: getpname
    for (int i = 1; i < 11; i++)
    {
        printf(1, "%d: ", i);
        if (getpname(i))
            printf(1, "Wrong pid\n");
    }

    // SYSTEMCALL TEST: ps
    for(int i = 0; i< 11; i++)
    {
      printf(1,"%d params of ps\n",i);
      ps(i);
      printf(1,"\n");
    }

    // SYSTEMCALL TEST: getnice
    for(int i = 0; i < 11; i++)
    {
      printf(1,"%d params of getnice:",i);
      printf(1,"%d\n",getnice(i));
    }

    // SYSTEMCALL TEST: setnice
    for(int i = 0; i < 11; i++)
    {
      printf(1,"%d params of setnice:",i);
      printf(1,"%d\n",setnice(i,i+37));
    }

    // SYSTEMCALL TEST: ps
    for(int i = 0; i< 11; i++)
    {
      printf(1,"%d params of ps\n",i);
      ps(i);
      printf(1,"\n");
    }


    exit();
}
