//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "fs.h"
#include "file.h"
#include "fcntl.h"


int k_readlink(char* path, char* buf, uint bufsiz); /* A&T forward declaration */

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=proc->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;

  for(fd = 0; fd < NOFILE; fd++){
    if(proc->ofile[fd] == 0){
      proc->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

int
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

int
sys_read(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return fileread(f, p, n);
}

int
sys_write(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return filewrite(f, p, n);
}

int
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  proc->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

int
sys_fstat(void)
{
  struct file *f;
  struct stat *st;

  if(argfd(0, 0, &f) < 0 || argptr(1, (void*)&st, sizeof(*st)) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
int
sys_link(void)
{
  char name[DIRSIZ], *new, *old;
  struct inode *dp, *ip;

  if(argstr(0, &old) < 0 || argstr(1, &new) < 0)
    return -1;
  if((ip = namei(old)) == 0)
    return -1;

  begin_trans();

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    commit_trans();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  commit_trans();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  commit_trans();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

//PAGEBREAK!
int
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], *path;
  uint off;

  if(argstr(0, &path) < 0)
    return -1;
  if((dp = nameiparent(path, name)) == 0)
    return -1;

  begin_trans();

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  commit_trans();

  return 0;

bad:
  iunlockput(dp);
  commit_trans();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  uint off;
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;
  ilock(dp);

  if((ip = dirlookup(dp, name, &off)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && ip->type == T_FILE)
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

int
sys_open(void)
{
  char *path;
  int fd, omode;
  struct file *f;
  struct inode *ip;
  struct inode *sym_ip;
  int i;

  if(argstr(0, &path) < 0 || argint(1, &omode) < 0)
    return -1;
  if(omode & O_CREATE){
    begin_trans();
    ip = create(path, T_FILE, 0, 0);
    commit_trans();
    if(ip == 0)
      return -1;
  } else {
    if((ip = namei(path)) == 0)
      return -1;
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      return -1;
    }
  }

  //A&T checks if symlink
  for (i=0;i < 16 ; i++) { //prevents loops ,up to 16 chain links
      if (ip->flags & I_SYMLNK) {
          if((sym_ip = namei((char*)ip->addrs)) == 0) {
              iunlock(ip);
              return -1;
          }
          iunlock(ip);
          ip = sym_ip;
          ilock(ip);
      } else
          break;
  }
  if (i == 16) {
      panic("symbolic link exceeds 16 links ");
  }
  //A&T - end

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    return -1;
  }
  iunlock(ip);

  f->type = FD_INODE;
  f->ip = ip;
  f->off = 0;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  return fd;
}

int
sys_mkdir(void)
{
  char *path;
  struct inode *ip;

  begin_trans();
  if(argstr(0, &path) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    commit_trans();
    return -1;
  }
  iunlockput(ip);
  commit_trans();
  return 0;
}

int
sys_mknod(void)
{
  struct inode *ip;
  char *path;
  int len;
  int major, minor;

  begin_trans();
  if((len=argstr(0, &path)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEV, major, minor)) == 0){
    commit_trans();
    return -1;
  }
  iunlockput(ip);
  commit_trans();
  return 0;
}

int
sys_chdir(void)
{
  char *path;
  char sym_path[MAX_LNK_NAME];            /* A&T if it's a symlink */
  struct inode *ip;

  if(argstr(0, &path) < 0)
      return -1;

  /* A&T de-reference path if it's a symlink */
  if ((k_readlink(path, sym_path, MAX_LNK_NAME)) != -1) {
      if ((ip = namei(sym_path)) == 0)
          return -1;
  } else {			/* A&T no a symlink */
      if ((ip = namei(path)) == 0)
          return -1;
  }



  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    return -1;
  }
  iunlock(ip);
  iput(proc->cwd);
  proc->cwd = ip;
  return 0;
}

int
sys_exec(void)
{
  char *path, *argv[MAXARG];
  int i;
  uint uargv, uarg;

  if(argstr(0, &path) < 0 || argint(1, (int*)&uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv))
      return -1;
    if(fetchint(proc, uargv+4*i, (int*)&uarg) < 0)
      return -1;
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    if(fetchstr(proc, uarg, &argv[i]) < 0)
      return -1;
  }
  return exec(path, argv);
}

int
sys_pipe(void)
{
  int *fd;
  struct file *rf, *wf;
  int fd0, fd1;

  if(argptr(0, (void*)&fd, 2*sizeof(fd[0])) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      proc->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  fd[0] = fd0;
  fd[1] = fd1;
  return 0;
}


//A&T create a soft link
int
sys_symlink(void)
{
    char *target,*path;
    //    int fd;
    struct file *f;
    struct inode *ip;

    if(argstr(0, &target) < 0 || argstr(1, &path) < 0)
        return -1;
    begin_trans();
    ip = create(path, T_FILE, 0, 0);
    commit_trans();
    if(ip == 0)
        return -1;

    if((f = filealloc()) == 0){  //|| (fd = fdalloc(f)) < 0){
        if(f)
            fileclose(f);
        iunlockput(ip);
        return -1;
    }

    //change the inode
    if (strlen(target) > MAX_LNK_NAME)
        panic("target soft link path is too long ");
    safestrcpy((char*)ip->addrs,target,MAX_LNK_NAME);
    ip->flags |= I_SYMLNK;
    ip->size = 0;
    K_DEBUG_PRINT(9,"inode ip->addrs= %s",(char*)ip->addrs);
    iunlock(ip);
    //

    f->type = FD_SYMLNK;
    f->ip = ip;
    f->off = 0;
    f->readable = 1; //readable
    f->writable = 0; //not writable

    return 0;

}

//A&T stores the target name in buf
int
sys_readlink(void)
{
    char *path;
    char *buf;
    uint bufsiz;
    struct inode *ip, *sym_ip;
    int i;

    if(argstr(0, &path) < 0 || argstr(1, &buf) < 0  || argint(2, (int*)&bufsiz) < 0)
        return -1;

    if((ip = namei(path)) == 0)
        return -1;
    ilock(ip);

    if (!(ip->flags & I_SYMLNK)){
        iunlock(ip);
        return -1;
    }

    for (i=0;i < 16 ; i++) { //prevents loops ,up to 16 chain links
        /* if (ip->flags & I_SYMLNK) { */
        if((sym_ip = namei((char*)ip->addrs)) == 0) {
            iunlock(ip);
            return -2;		/* broken link */
        }
        if (sym_ip->flags & I_SYMLNK) {
            iunlock(ip);
            ip = sym_ip;
            ilock(ip);
        } else {
            break;		/* found the non-symlink file. last
                                   link in ip. */
        }
        /* } else { */
        /*     break; */
        /* } */
    }
    if (i == 16) {
        panic("symbolic link exceeds 16 links ");
    }


    if((ip->type == T_FILE) && (ip->flags & I_SYMLNK)){
        safestrcpy(buf,(char*)ip->addrs,bufsiz);
        iunlock(ip);
        return strlen(buf);
    }
    iunlock(ip);
    return -1;
}


//A&T stores the target name in buf - for use by kernel (not syscall)
int
k_readlink(char* path, char* buf, uint bufsiz)
{
    struct inode *ip, *sym_ip;
    int i;

    if((ip = namei(path)) == 0)
        return -1;
    ilock(ip);

    if (!(ip->flags & I_SYMLNK)){
        iunlock(ip);
        return -1;
    }

    for (i=0;i < 16 ; i++) { //prevents loops ,up to 16 chain links
        /* if (ip->flags & I_SYMLNK) { */
        if((sym_ip = namei((char*)ip->addrs)) == 0) {
            iunlock(ip);
            return -1;
        }
        if (sym_ip->flags & I_SYMLNK) {
            iunlock(ip);
            ip = sym_ip;
            ilock(ip);
        } else {
            break;		/* found the non-symlink file. last
                                   link in ip. */
        }
        /* } else { */
        /*     break; */
        /* } */
    }
    if (i == 16) {
        panic("symbolic link exceeds 16 links ");
    }


    if((ip->type == T_FILE) && (ip->flags & I_SYMLNK)){
        safestrcpy(buf,(char*)ip->addrs,bufsiz);
        iunlock(ip);
        return strlen(buf);
    }
    iunlock(ip);
    return -1;
}

/* A&T tags */
int
sys_ftag(void) {
    int fd, ret;
    char *key;
    char *val;
    struct file *file_ptr;

    if(argfd(0, &fd, &file_ptr) < 0 || argstr(1, &key) < 0  || argstr(2, &val) < 0)
        return -1;

    K_DEBUG_PRINT(7, "fd = %d, key = %s, val = %s", fd, key, val);

    begin_trans();
    K_DEBUG_PRINT(7, "began transaction", 999);

    ret = fs_ftag(file_ptr, key, val);

    K_DEBUG_PRINT(7, "fs_ftag done", 999);

    commit_trans();

    K_DEBUG_PRINT(7, "transaction commited.", 999);
    return ret;
}

int sys_funtag(void) {
    int fd, ret;
    char *key;
    struct file *file_ptr;

    if(argfd(0, &fd, &file_ptr) < 0 || argstr(1, &key) < 0)
        return -1;

    begin_trans();
    ret = fs_funtag(file_ptr, key);
    commit_trans();
    return ret;
}

int sys_gettag(void) {
    int fd,ret;
    char *key;
    char *buf;
    struct file *file_ptr;

    if(argfd(0, &fd, &file_ptr) < 0 || argstr(1, &key) < 0 || argstr(2, &buf) < 0)
        return -1;
    K_DEBUG_PRINT(6, "inside sys_gettag. key = %s, file_ptr = %x.",key,(int)file_ptr);
    begin_trans();

    ret =fs_gettag(file_ptr,key,buf);
    commit_trans();
    return ret;
}
