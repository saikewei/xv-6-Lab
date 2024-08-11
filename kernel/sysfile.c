//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

#define THRESHOLD 10

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if (argint(n, &fd) < 0)
    return -1;
  if (fd < 0 || fd >= NOFILE || (f = myproc()->ofile[fd]) == 0)
    return -1;
  if (pfd)
    *pfd = fd;
  if (pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for (fd = 0; fd < NOFILE; fd++)
  {
    if (p->ofile[fd] == 0)
    {
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if (argfd(0, 0, &f) < 0)
    return -1;
  if ((fd = fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  if (argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if (argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if (argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  if (argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if (argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if ((ip = namei(old)) == 0)
  {
    end_op();
    return -1;
  }

  ilock(ip);
  if (ip->type == T_DIR)
  {
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if ((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if (dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0)
  {
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for (off = 2 * sizeof(de); off < dp->size; off += sizeof(de))
  {
    if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if (de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if (argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if ((dp = nameiparent(path, name)) == 0)
  {
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if (namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if ((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if (ip->nlink < 1)
    panic("unlink: nlink < 1");
  if (ip->type == T_DIR && !isdirempty(ip))
  {
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if (ip->type == T_DIR)
  {
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode *
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if ((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if ((ip = dirlookup(dp, name, 0)) != 0)
  {
    iunlockput(dp);
    ilock(ip);
    if (type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if ((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if (type == T_DIR)
  {              // Create . and .. entries.
    dp->nlink++; // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if (dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if (dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

// 打开文件系统调用，返回文件描述符。
// 如果操作失败，返回-1。
uint64 sys_open(void)
{
  char path[MAXPATH]; // 用于存储文件路径的字符数组
  int fd, omode;      // fd表示文件描述符，omode表示文件打开模式
  struct file *f;     // 文件结构指针
  struct inode *ip;   // inode结构指针
  int n;              // 临时变量，用于存储函数返回值

  // 获取系统调用的第一个参数（文件路径）和第二个参数（打开模式）
  // argstr获取字符串类型参数，argint获取整数类型参数
  if ((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
    return -1; // 如果获取参数失败，返回-1

  begin_op(); // 开始文件系统操作

  // 如果打开模式包含O_CREATE，表示需要创建文件
  if (omode & O_CREATE)
  {
    // 创建一个新文件，返回对应的inode指针
    ip = create(path, T_FILE, 0, 0);
    if (ip == 0)
    {
      end_op(); // 如果创建失败，结束操作并返回-1
      return -1;
    }
  }
  else // 如果不是创建文件的操作
  {
    // 根据路径查找文件对应的inode
    if ((ip = namei(path)) == 0)
    {
      end_op(); // 如果查找失败，结束操作并返回-1
      return -1;
    }
    ilock(ip); // 锁定inode以进行后续操作

    // 如果inode表示目录，并且打开模式不是只读，返回错误
    if (ip->type == T_DIR && omode != O_RDONLY)
    {
      iunlockput(ip); // 解锁并释放inode
      end_op();       // 结束操作
      return -1;
    }
  }

  // 检查是否为设备文件，并验证设备号是否合法
  if (ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV))
  {
    iunlockput(ip); // 如果设备号非法，解锁并释放inode
    end_op();       // 结束操作
    return -1;
  }

  // 处理符号链接，逐层解析直到达到实际文件或达到限制
  int layer = 0; // 跟踪符号链接的层数
  while (ip->type == T_SYMLINK && !(omode & O_NOFOLLOW))
  {
    layer++;                // 增加层数
    if (layer == THRESHOLD) // 如果层数达到阈值，返回错误
    {
      iunlockput(ip); // 解锁并释放inode
      end_op();       // 结束操作
      return -1;
    }
    else // 继续解析符号链接
    {
      // 读取符号链接指向的路径
      if (readi(ip, 0, (uint64)path, 0, MAXPATH) < MAXPATH)
      {
        iunlockput(ip); // 如果读取失败，解锁并释放inode
        end_op();       // 结束操作
        return -1;
      }
      iunlockput(ip);   // 解锁并释放当前符号链接的inode
      ip = namei(path); // 解析符号链接指向的路径
      if (ip == 0)
      {
        end_op(); // 如果解析失败，结束操作并返回-1
        return -1;
      }
      ilock(ip); // 锁定解析后的inode
    }
  }

  // 分配一个文件结构并获取文件描述符
  if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0)
  {
    if (f) // 如果文件结构分配失败，释放文件结构
      fileclose(f);
    iunlockput(ip); // 解锁并释放inode
    end_op();       // 结束操作
    return -1;
  }

  // 如果是设备文件，设置文件类型为设备文件，并记录设备号
  if (ip->type == T_DEVICE)
  {
    f->type = FD_DEVICE;
    f->major = ip->major;
  }
  else // 否则设置文件类型为普通文件，文件偏移量为0
  {
    f->type = FD_INODE;
    f->off = 0;
  }

  f->ip = ip;                                           // 将inode指针赋给文件结构
  f->readable = !(omode & O_WRONLY);                    // 设置文件的可读性
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR); // 设置文件的可写性

  // 如果打开模式包含O_TRUNC，并且文件类型为普通文件，截断文件内容
  if ((omode & O_TRUNC) && ip->type == T_FILE)
  {
    itrunc(ip);
  }

  iunlock(ip); // 解锁inode
  end_op();    // 结束文件系统操作

  return fd; // 返回文件描述符
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if (argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0)
  {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  if ((argstr(0, path, MAXPATH)) < 0 ||
      argint(1, &major) < 0 ||
      argint(2, &minor) < 0 ||
      (ip = create(path, T_DEVICE, major, minor)) == 0)
  {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();

  begin_op();
  if (argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0)
  {
    end_op();
    return -1;
  }
  ilock(ip);
  if (ip->type != T_DIR)
  {
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  if (argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0)
  {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for (i = 0;; i++)
  {
    if (i >= NELEM(argv))
    {
      goto bad;
    }
    if (fetchaddr(uargv + sizeof(uint64) * i, (uint64 *)&uarg) < 0)
    {
      goto bad;
    }
    if (uarg == 0)
    {
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if (argv[i] == 0)
      goto bad;
    if (fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

bad:
  for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  if (argaddr(0, &fdarray) < 0)
    return -1;
  if (pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if ((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0)
  {
    if (fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if (copyout(p->pagetable, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
      copyout(p->pagetable, fdarray + sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0)
  {
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

// 创建符号链接系统调用，返回0表示成功，返回-1表示失败。
uint64 sys_symlink(void)
{
  char target[MAXPATH], path[MAXPATH]; // 定义用于存储目标路径和符号链接路径的字符数组

  // 获取系统调用的第一个参数（符号链接指向的目标路径）和第二个参数（符号链接路径）
  // 如果获取参数失败，则返回-1
  if (argstr(0, target, MAXPATH) < 0 || argstr(1, path, MAXPATH) < 0)
    return -1;

  begin_op();       // 开始文件系统操作
  struct inode *ip; // 定义inode结构指针

  // 创建一个类型为符号链接的inode，路径为`path`
  // 如果创建失败，结束操作并返回-1
  if ((ip = create(path, T_SYMLINK, 0, 0)) == 0)
  {
    end_op();
    return -1;
  }

  // 将目标路径写入到符号链接的inode中
  // 如果写入的字节数少于MAXPATH，则表示写入失败，解锁并释放inode，然后结束操作返回-1
  if (writei(ip, 0, (uint64)target, 0, MAXPATH) < MAXPATH)
  {
    iunlockput(ip);
    end_op();
    return -1;
  }

  iunlockput(ip); // 解锁并释放inode
  end_op();       // 结束文件系统操作
  return 0;       // 返回0表示成功
}
