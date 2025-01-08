/**
 * @file mkfs_bitsfs.c
 * @author Aaron Lau (bitsobject.com)
 * @brief To make bitsfs
 * @version 0.1
 * @date 2024-12-06
 * 
 * @copyright Copyright (c) 2024
 * 
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include "mkfs_bitsfs.h"

#define    DFD    3

#define    EXIT_SUCCESS    0
#define    EXIT_FAILURE    1

#define    BDEV_MIN_SIZE   256

int open_dev(char *dev_path)
{
    int fd;
    fd = open(dev_path, O_WRONLY, 0666);
    if(fd < 0) {
        printf("Can't open dev [ %s ].\n", dev_path);
    }
    printf("open_dev fd=%d\n", fd);
    return fd;
}

int move_fd(int from, int to)
{
    int ret;
    if (from == to)
        return to;
    ret = dup2(from, to);
    if (ret != to) {
        printf("Failed to dup fd, ret=%d\n", ret);
    }
    close(from);
    printf("move ret=%d\n", ret);
    return ret;
}

int do_stat(int fd, struct stat *buff)
{
    int ret;
    ret = fstat(fd, buff);
    if(ret < 0) {
        printf("Failed to stat dev\n");
    }
    printf("do_stat ret=%d\n", ret);
    return ret;
}

off_t get_vol_size(int fd) 
{
    off_t dsize = 0;
    dsize = lseek(fd, 0, SEEK_END);

    /* reset pos pointer */ 
    lseek(fd, 0, SEEK_SET);
    printf("get_vol_size dsize=%d\n", dsize);

    return dsize;
}

/**
 * Put buff to bdev
 */
ssize_t PUT(int fd, uint64_t off, void *buff, size_t len)
{
    ssize_t ret;
    lseek(fd, off, SEEK_SET);
    ret = write(fd, buff, len);
    return ret;
}

/**
 * Fill super block object
 */
static void fill_sb(struct bitsfs_super_block *sb)
{
    sb->s_block_bitmap_block = BITSFS_BLKBMP_BLOCK;
    sb->s_inode_bitmap_block = BITSFS_INDBMP_BLOCK;
    sb->s_inode_table_block  = BITSFS_INDTBL_BLOCK;
    sb->s_data_block         = BITSFS_DATA_BLOCK;
    sb->s_block_size         = BITSFS_BLOCK_SIZE;
    sb->s_first_ino          = BITSFS_ROOT_INO;
    sb->s_inode_size         = sizeof(struct bitsfs_inode);
    sb->s_magic              = BITSFS_SUPER_MAGIC;
    sb->s_state              = BITSFS_VALID_FS;
    sb->s_creator_os         = BITSFS_OS_LINUX;
    strncpy(sb->s_name, "bitsfs", sizeof(sb->s_name));
}

static void fill_inode(struct bitsfs_inode *inode)
{
    time_t tsp = time(NULL);
    inode->i_mode  = S_IFDIR | S_IRWXU | S_IRGRP | S_IROTH | S_IXGRP | S_IXOTH;
    inode->i_mtime = tsp;
    inode->i_atime = tsp;
    inode->i_ctime = tsp;
    inode->i_size  = BITSFS_BLOCK_SIZE;
    inode->i_links_count = 2; /* "/.", "/.." */
    inode->i_block[0] = BITSFS_DATA_BLOCK;
    inode->i_blocks = 1;
}

static void fill_root_dir(struct bitsfs_dir_special *root_dir)
{
    root_dir->inode1 = BITSFS_ROOT_INO;
    root_dir->rec_len1 = DENT_LEN;
    root_dir->name_len1 = 1;
    root_dir->file_type1 = BITSFS_FT_DIR;
    root_dir->name1[0] = '.';
    
    root_dir->inode2 = BITSFS_ROOT_INO;
    root_dir->rec_len2 = DENT_LEN;
    root_dir->name_len2 = 2;
    root_dir->file_type2 = BITSFS_FT_DIR;
    root_dir->name2[0] = '.';
    root_dir->name2[1] = '.';
}

int main(int argc, char **argv)
{
    int fd;
    int nblocks = 0;
    int inode_count = 0;
    int block_count = 0;
    unsigned int inode_size;
    unsigned int rdir_size;
    off_t kbytes;
    ssize_t wlen;
    struct stat dstat;
    struct bitsfs_super_block *sb;
    struct bitsfs_inode *inode;
    struct bitsfs_dir_special *rdir;
    char *cbuff;
    void *buff;

    if (argc <= 1) {
        printf("Bad param");
        exit(EXIT_FAILURE);
    }

    fd = open_dev(argv[1]);
    if (fd < 0) {
        exit(EXIT_FAILURE);
    }

    fd = move_fd(fd, DFD);
    if (fd != DFD) {
       exit(EXIT_FAILURE);
    }

    if (do_stat(fd, &dstat) < 0) {
        exit(EXIT_FAILURE);
    }

    if(!S_ISBLK(dstat.st_mode)) {
        printf("Bad dev: not bdev\n");
        exit(EXIT_FAILURE);
    }

    kbytes = get_vol_size(fd) / 1024;
    printf("kbytes=%d\n", kbytes);
    if(kbytes <  BDEV_MIN_SIZE) {
        printf("Bad dev: vol size too small [%dKB]\n", kbytes);
        exit(EXIT_FAILURE);
    }

    printf("sbsize=%d, idsize=%d\n", sizeof(struct bitsfs_super_block), sizeof(struct bitsfs_inode));

    /* Calc block count */
    nblocks = (kbytes * 1024 / BITSFS_BLOCK_SIZE);
    inode_count = BITSFS_INDTBL_BLOCKS * BITSFS_BLOCK_SIZE / sizeof(struct bitsfs_inode);
    inode_size = sizeof(struct bitsfs_inode);
    rdir_size = sizeof(struct bitsfs_dir_special);
    printf("nblocks=%d, inodes=%d, isize=%d, rdrsize=%d\n", nblocks, inode_count, inode_size, rdir_size);
    
    /* Allocate buff */
    buff = malloc(BITSFS_BLOCK_SIZE);

    /* Fill super block */
    memset(buff, 0, BITSFS_BLOCK_SIZE);
    sb = (struct bitsfs_super_block*)buff;
    fill_sb(sb);
    sb->s_inodes_count = inode_count;
    sb->s_blocks_count = nblocks;
    sb->s_free_inodes_count = sb->s_inodes_count - 1;
    sb->s_free_blocks_count = sb->s_blocks_count - BITSFS_DATA_BLOCK - 1;

    /* Put super block */
    wlen = PUT(fd, BITSFS_SUPER_BLOCK * BITSFS_BLOCK_SIZE, sb, BITSFS_BLOCK_SIZE);
    printf("wlen1=%d\n", wlen);
    if(wlen != BITSFS_BLOCK_SIZE) {
        printf("Put super block failed, wlen=%d\n", wlen);
        goto mend;
    }

    /* Put inode bitmap */
    memset(buff, 0, BITSFS_BLOCK_SIZE);
    wlen = PUT(fd, BITSFS_BLKBMP_BLOCK * BITSFS_BLOCK_SIZE, buff, BITSFS_BLOCK_SIZE);
    wlen += PUT(fd, (BITSFS_BLKBMP_BLOCK + 1) * BITSFS_BLOCK_SIZE, buff, BITSFS_BLOCK_SIZE);
    wlen += PUT(fd, (BITSFS_BLKBMP_BLOCK + 2) * BITSFS_BLOCK_SIZE, buff, BITSFS_BLOCK_SIZE);
    wlen += PUT(fd, (BITSFS_BLKBMP_BLOCK + 3) * BITSFS_BLOCK_SIZE, buff, BITSFS_BLOCK_SIZE);
    printf("wlen2=%d\n", wlen);
    if(wlen != BITSFS_BLOCK_SIZE  * BITSFS_BLKBMP_BLOCKS) {
        printf("Put block bitmap failed, wlen=%d\n", wlen);
        goto mend;
    }

    /* Put inode bitmap */
    memset(buff, 0, BITSFS_BLOCK_SIZE);
    wlen = PUT(fd, BITSFS_INDBMP_BLOCK * BITSFS_BLOCK_SIZE, buff, BITSFS_BLOCK_SIZE);
    printf("wlen3=%d\n", wlen);
    if(wlen != BITSFS_BLOCK_SIZE) {
        printf("Put inode bitmap failed, wlen=%d\n", wlen);
        goto mend;
    }

    /* Put inode table */
    memset(buff, 0, BITSFS_BLOCK_SIZE);
    for (int i = 0;i < BITSFS_INDTBL_BLOCKS; ++i) {
        wlen = PUT(fd, (BITSFS_INDTBL_BLOCK + i) * BITSFS_BLOCK_SIZE, buff, BITSFS_BLOCK_SIZE);
        if(wlen != BITSFS_BLOCK_SIZE) {
            printf("Put inode table failed, wlen=%d\n", wlen);
            goto mend;
        }
    }

    /* Fill root inode */
    memset(buff, 0, BITSFS_BLOCK_SIZE);
    inode = (struct bitsfs_inode*)buff;
    fill_inode(inode);

    /* Put root inode */
    wlen = PUT(fd, 
            BITSFS_INDTBL_BLOCK * BITSFS_BLOCK_SIZE + (BITSFS_ROOT_INO - 1) * inode_size, inode, inode_size);
    printf("wlen4=%d\n", wlen);
    if(wlen != inode_size) {
        printf("Put root inode failed, wlen=%d\n", wlen);
        goto mend;
    }

    /* Fill root dir entry */
    memset(buff, 0, BITSFS_BLOCK_SIZE);
    rdir = (struct bitsfs_dir_special*)buff;
    fill_root_dir(rdir);

    wlen = PUT(fd, BITSFS_DATA_BLOCK * BITSFS_BLOCK_SIZE, rdir, BITSFS_BLOCK_SIZE);
    printf("wlen5=%d\n", wlen);
    if(wlen != BITSFS_BLOCK_SIZE) {
        printf("Put root dir entry failed, wlen=%d\n", wlen);
        goto mend;
    }
    
mend:
    close(fd);
    free(buff);
    return 0;
}
