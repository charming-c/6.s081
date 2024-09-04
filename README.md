# fs lab 实验手册
实现两个功能：
- 为 inode 实现两级索引
- 实现软链接

## 1. 两级索引
这里的实现和多级页表很相像，实现起来更简单
代码如下：
```c
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0)
      ip->addrs[bn] = addr = balloc(ip->dev);
    return addr;
  }

  // bn > NDIRECT, two cases:
  // 1. bn - NDIRECT < INDRECT
  // 2. bn - NDIRECT - INDRECT < NDOUBLEINDRECT
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      a[bn] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }

  bn -= NINDIRECT;
  if(bn < NDOUBLEINDITRCE) {
    /**
     * 1. 先判断一级是否为空
     * 2. 再判断二级是否为空
     */

    if((addr = ip->addrs[NDIRECT + 1]) == 0)
      ip->addrs[NDIRECT + 1] = addr = balloc(ip->dev);

    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;

    if((addr = a[bn / NINDIRECT]) == 0) {
      a[bn / NINDIRECT] = addr = balloc(ip->dev);
      log_write(bp);
    }

    struct buf *bp2 = bread(ip->dev, addr);
    a = (uint *)bp2->data;
    
    if((addr = a[bn % NINDIRECT]) == 0) {
      a[bn % NINDIRECT] = addr = balloc(ip->dev);
      log_write(bp2);
    }
    brelse(bp2);
    brelse(bp);
    return addr;
  }

  panic("bmap: out of range");
}

// Truncate inode (discard contents).
// Caller must hold ip->lock.
void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;

  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j])
        bfree(ip->dev, a[j]);
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  if(ip->addrs[NDIRECT + 1]) {
    bp = bread(ip->dev, ip->addrs[NDIRECT + 1]);
    a = (uint *)bp->data;
    for(j = 0; j < NINDIRECT; j++) {
      if(a[j]) {
        struct buf *bp2 = bread(ip->dev, a[j]);
        uint *b = (uint *)bp2->data;
        for(int k = 0; k < NINDIRECT; k++) {
          if(b[k])
            bfree(ip->dev, b[k]);
        }
        brelse(bp2);
        bfree(ip->dev, a[j]);
      }
      
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT + 1]);
    ip->addrs[NDIRECT + 1] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}
```
## 2. 软链接
主要是创建一个 inode，并且将 target 保存到 inode 的第一个 block 块中，这里有一个坑点就是递归查找 inode 的代码位置，这里必须在查找到 ip 之后里面进行软链接查询，我放到判断 device 和 dir 之后，虽然通过测试，但是内核 panic。
代码如下：
```c
uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;
  int depth = 0;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  
  // /**
  //  * 是 symlink，并且 mode 不是 O_NOFOLLOW，则继续查找 ip
  //  */
  if(ip->type == T_SYMLINK && (omode & O_NOFOLLOW) == 0) {
    while(depth < 10) {
      if(readi(ip, 0, (uint64)path, 0, MAXPATH) < 0) {
        iunlockput(ip);
        end_op();
        return -1;
      }
      iunlockput(ip);
      
      if((ip = namei(path)) == 0) {
        end_op();
        return -1;
      }
      ilock(ip);
      if(ip->type != T_SYMLINK) break;
      depth++;
    }

    if(depth >= 10) {
      iunlockput(ip);
      end_op();
      return -1;
    }
  }


  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }

  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_symlink(void)
{
  struct inode *ip;
  char target[MAXPATH], path[MAXPATH];
  if(argstr(0, target, MAXPATH) < 0 || argstr(1, path, MAXPATH) < 0)
    return -1;

  begin_op();
  ip = create(path, T_SYMLINK, 0, 0);
  if(ip == 0) goto fail;

  if(writei(ip, 0, (uint64)target, 0, MAXPATH) < 0)
    goto fail;

  iunlockput(ip);

  end_op();
  return 0;
  fail:
    end_op();
    return -1;
}
```