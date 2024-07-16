#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

char *fmtname(char *path)
{ // 获取当前文件名
    char *p;
    for (p = path + strlen(path); p >= path && *p != '/'; p--)
        ;
    p++;
    return p;
}

// 递归查找指定路径下的文件或目录
void find(char *path, char *target)
{
    char buf[512], *p;
    int fd;
    struct dirent de;
    struct stat st;

    // 打开指定路径的文件或目录
    if ((fd = open(path, 0)) < 0)
    {
        fprintf(2, "find: cannot open %s\n", path); // 输出错误信息，无法打开路径
        return;
    }

    // 获取文件或目录的状态信息
    if (fstat(fd, &st) < 0)
    {
        fprintf(2, "find1: cannot stat %s\n", path); // 输出错误信息，无法获取状态信息
        close(fd);
        return;
    }

    // 根据文件或目录的类型进行处理
    switch (st.type)
    {
    case T_FILE:
        // 如果是文件，比较文件名是否与目标名称相同
        if (strcmp(target, fmtname(path)) == 0)
        {
            printf("%s\n", path); // 输出文件路径
        }

        break;
    case T_DIR:
        // 如果是目录，检查路径长度是否超出缓冲区范围
        if (strlen(path) + 1 + DIRSIZ + 1 > sizeof buf)
        {
            printf("find: path too long\n"); // 输出错误信息，路径过长
            break;
        }
        strcpy(buf, path); // 将路径复制到缓冲区
        p = buf + strlen(buf);
        *p++ = '/';

        // 遍历目录中的每个文件或子目录
        while (read(fd, &de, sizeof(de)) == sizeof(de))
        {
            if (de.inum == 0)
                continue; // 跳过空目录项

            memmove(p, de.name, DIRSIZ); // 将文件或目录名复制到缓冲区末尾
            p[DIRSIZ] = 0;               // 添加字符串结束符

            // 获取文件或目录的状态信息
            if (stat(buf, &st) < 0)
            {
                printf("find2: cannot stat %s\n", buf); // 输出错误信息，无法获取文件或目录的状态信息
                continue;
            }

            // 排除当前目录和上级目录的特殊情况
            if (strlen(de.name) == 1 && de.name[0] == '.')
                continue;
            if (strlen(de.name) == 2 && de.name[0] == '.' && de.name[1] == '.')
                continue;

            find(buf, target); // 递归调用查找函数，继续查找子目录
        }
        break;
    }

    close(fd); // 关闭文件或目录
}

int main(int argc, char *argv[])
{
    if (argc == 3)
        find(argv[1], argv[2]);
    else
        printf("Wrong argument\n");

    exit(0);
}