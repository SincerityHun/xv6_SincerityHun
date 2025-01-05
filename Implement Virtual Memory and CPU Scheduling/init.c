// init: The initial user-level program

#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

char *argv[] = {"sh", 0}; // "sh" shell 프로그램을 실행하기 위한 인자 목록

int main(void)
{
  int pid, wpid;

  // 1. console 장치를 Read/Write mode로 열기
  if (open("console", O_RDWR) < 0)
  {
    mknod("console", 1, 1);  // 못 열면 새로운 "console"을 만들기
    open("console", O_RDWR); // 열기
  }
  dup(0); // stdout 파일 스크립터 복제
  dup(0); // stderr 파일 스크립터 복제

  for (;;)
  { // 쉘 프로세스 시작
    printf(1, "init: starting sh\n");
    printf(1, "Student ID: 2020315064\n");
    printf(1, "Name: Seonghun Jung\n");
    printf(1, "==========Booting Message==========\n");
    pid = fork();
    if (pid < 0)
    { // 애초부터 열기 실패한 경우
      printf(1, "init: fork failed\n");
      exit();
    }
    if (pid == 0)
    {                   // 현재 프로그램이 자식인 경우: shell 프로그램 시작!
      exec("sh", argv); // 여기서 잘 열리면 기존 자식 프로그램 종료!
      printf(1, "init: exec sh failed\n");
      exit();
    }
    while ((wpid = wait()) >= 0 && wpid != pid) // 현재 프로그램이 부모인 경우: 자식 프로세스 계속 기다리다가 pid 받아서 좀비인지 확인함!
      printf(1, "zombie!\n");
  }
}
