#include "kernel/types.h"
#include "user/user.h"

int main()
{
    int fd1[2];
    int fd2[2];

    pipe(fd1);
    pipe(fd2);

    char buffer[16];

    if (fork() != 0)
    {
        // 父线程执行的代码
        write(fd1[1], "ping", strlen("ping"));
        read(fd2[0], buffer, 4);
        printf("%d: received %s\n", getpid(), buffer);
    }
    else
    {
        // 子线程执行的代码
        read(fd1[0], buffer, 4);
        printf("%d: received %s\n", getpid(), buffer);
        write(fd2[1], "pong", strlen("pong"));
    }

    exit(0);
}