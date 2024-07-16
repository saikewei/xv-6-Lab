#include "kernel/types.h" // 包含内核类型定义
#include "user/user.h"    // 包含用户模式下的库函数
#include "kernel/param.h" // 包含内核参数定义

int main(int argc, char *argv[])
{
    // 检查命令行参数是否少于2个
    if (argc < 2)
    {
        fprintf(2, "Error: too few parameters for xargs\n"); // 输出错误信息
        exit(1);                                             // 退出程序，返回状态1
    }

    int i;
    int arg_count = 0;  // 参数数量计数器
    char *args[MAXARG]; // 参数数组，最大长度为MAXARG

    // 将命令行参数复制到参数数组args中
    for (i = 1; i < argc; ++i)
    {
        args[arg_count++] = argv[i];
    }

    int initial_arg_count = arg_count; // 存储初始参数数量的位置
    char input_char;                   // 用于读取字符
    char *current_line;                // 指向当前处理的行
    char line_buffer[512];             // 临时存储字符串的缓冲区
    current_line = line_buffer;
    int line_index = 0; // 当前行的索引

    // 从标准输入读取字符
    while (read(0, &input_char, 1) > 0)
    {
        if (input_char == '\n')
        {
            // 处理换行符，将当前行作为参数
            current_line[line_index] = '\0'; // 添加字符串结束符
            line_index = 0;                  // 重置索引

            args[arg_count++] = current_line; // 将当前行添加到参数数组
            args[arg_count] = 0;              // 设置参数数组的结束标志

            if (fork()) // 创建子进程
            {
                wait(0);                       // 父进程等待子进程结束
                arg_count = initial_arg_count; // 重置参数数量
            }
            else
            {
                exec(argv[1], args); // 子进程执行命令
            }
        }
        else if (input_char == ' ')
        {
            // 处理空格，将当前单词作为参数
            current_line[line_index] = '\0';  // 添加字符串结束符
            line_index = 0;                   // 重置索引
            args[arg_count++] = current_line; // 将当前单词添加到参数数组
            char line_buffer[512];            // 重新分配缓冲区
            current_line = line_buffer;
        }
        else
        {
            // 处理普通字符，将其添加到当前行
            current_line[line_index++] = input_char;
        }
    }
    exit(0); // 程序结束
}
