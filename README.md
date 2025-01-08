BitsFS is a simple file system based on Linux kernel. You can use it after make fs on a block device.

## 1. File list
--File System Source
bitsfs.h  
block.c  
dentry.c  
namei.c  
inode.c  
super.c
Makefile  

--Make FS Tool
mkfs_bitsfs.c  
mkfs_bitsfs.h  

## 2. Compile & Install module
make
insmod bitsfs.ko

## 3. Make FS
gcc -o mkfs_bitsfs mkfs_bitsfs.c
./mkfs_bitsfs /dev/sdb

## 4. Mount FS
mount /dev/sdb /mnt/bitsfs
