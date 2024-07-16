#include "kernel/types.h"
#include "user/user.h"

void new_prime_proc(int *old_pipe)
{
    close(old_pipe[1]);

    int first_num;
    if (!read(old_pipe[0], &first_num, sizeof(first_num)))
    {
        close(old_pipe[0]);
        exit(0);
    }

    fprintf(1, "prime %d\n", first_num);

    int new_pipe[2];
    pipe(new_pipe);

    int p_id = fork();
    if (p_id == 0)
        new_prime_proc(new_pipe);
    else
    {
        int t;
        while (read(old_pipe[0], &t, sizeof(t)))
        {
            if (t % first_num != 0)
            {
                write(new_pipe[1], &t, sizeof(t));
            }
        }

        close(old_pipe[0]);
        close(new_pipe[0]);
        close(new_pipe[1]);

        wait((int *)0);
    }
}

int main()
{
    int p[2];
    pipe(p);

    int i;
    for (i = 2; i <= 35; i++)
        write(p[1], &i, sizeof(i));

    new_prime_proc(p);

    exit(0);
}