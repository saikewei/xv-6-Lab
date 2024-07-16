#include "kernel/types.h"
#include "user/user.h"

void new_prime_proc(int *old_pipe)
{
    close(old_pipe[1]); // 关闭旧管道的写端

    int first_num;
    // 从旧管道中读取第一个数，如果读取失败则退出
    if (!read(old_pipe[0], &first_num, sizeof(first_num)))
    {
        close(old_pipe[0]); // 关闭旧管道的读端
        exit(0);            // 退出进程
    }

    fprintf(1, "prime %d\n", first_num); // 打印第一个数，即当前的素数

    int new_pipe[2];
    pipe(new_pipe); // 创建新管道

    int p_id = fork(); // 创建子进程
    if (p_id == 0)
        new_prime_proc(new_pipe); // 子进程递归调用处理新管道
    else
    {
        int t;
        // 在父进程中，从旧管道中读取数，如果不是first_num的倍数则写入新管道
        while (read(old_pipe[0], &t, sizeof(t)))
        {
            if (t % first_num != 0)
            {
                write(new_pipe[1], &t, sizeof(t));
            }
        }

        close(old_pipe[0]); // 关闭旧管道的读端
        close(new_pipe[0]); // 关闭新管道的读端
        close(new_pipe[1]); // 关闭新管道的写端

        wait((int *)0); // 等待子进程结束
    }
}

int main()
{
    int p[2];
    pipe(p); // 创建管道

    int i;
    // 向管道中写入从2到35的所有数
    for (i = 2; i <= 35; i++)
        write(p[1], &i, sizeof(i));

    new_prime_proc(p); // 调用处理函数处理管道中的数

    exit(0); // 退出程序
}
