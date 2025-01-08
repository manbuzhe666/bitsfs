#include "bitsfs.h"
#include <linux/buffer_head.h>
#include <linux/pagemap.h>
#include <linux/mpage.h>
#include <linux/fiemap.h>
#include <linux/iomap.h>
#include <linux/namei.h>
#include <linux/uio.h>
#include <linux/dax.h>

/*
 * Read the block bitmap
 */
static struct buffer_head *read_block_bitmap(struct super_block *sb, int block_no)
{
    struct buffer_head *bh = NULL;
    bh = sb_getblk(sb, block_no);
    if (unlikely(!bh)) {
        bitsfs_msg(sb, KERN_ERR, __func__, __FILE__, __LINE__,
                "Cannot read block bitmap");
        return NULL;
    }
    if (likely(bh_uptodate_or_lock(bh)))
        return bh;

    if (bh_submit_read(bh) < 0) {
        brelse(bh);
        bitsfs_msg(sb, KERN_ERR, __func__, __FILE__, __LINE__,
                "Cannot read block bitmap");
        return NULL;
    }

    return bh;
}

void set_root_block_bitmap(struct inode *inode, int pos) 
{
    int ret;
    struct buffer_head *bh;
    bh = read_block_bitmap(inode->i_sb, BITSFS_BLKBMP_BLOCK);
    ret = bitsfs_set_bit(pos, bh->b_data);
    bitsfs_msg(inode->i_sb, KERN_INFO, __func__, __FILE__, __LINE__,
        "Set root block bitmap pos=%d, ret=%d", pos, ret);
    brelse(bh);
}

static int alloc_single_block(struct inode *inode, unsigned long *pos) 
{
    int err = 0;
    int block_length = BITSFS_BLOCK_SIZE << 3;
    struct buffer_head *bh;

    bitsfs_msg(inode->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "Alloc single block, ino=%lu", inode->i_ino);

    bh = read_block_bitmap(inode->i_sb, BITSFS_BLKBMP_BLOCK);
    *pos = bitsfs_find_next_zero_bit(bh->b_data, block_length, 0);
    if(*pos >= block_length) {
        bitsfs_msg(inode->i_sb, KERN_ERR, __func__, __FILE__, __LINE__, 
                "No enough blocks to alloc, pos=%lu", pos);
        err = -EIO;
        goto fail;
    }
    bitsfs_set_bit(*pos, bh->b_data);
fail:
    brelse(bh);
    return err;
}

static int find_avai_block_range(
        struct buffer_head *bh, 
        int range, 
        unsigned long offset, 
        unsigned long *start, unsigned long *end) 
{
    int err = 0;
    int block_length = BITSFS_BLOCK_SIZE << 3;

    *start = bitsfs_find_next_zero_bit(bh->b_data, block_length, offset);
    if (*start == block_length) {
        err = -EIO;
        goto fail;
    }
    *end = bitsfs_find_next_bit(bh->b_data, block_length, *start);
    while ((*end - *start) < range) {
        *start = bitsfs_find_next_zero_bit(bh->b_data, block_length, *end);
        if (*start == block_length) {
            err = -EIO;
            break;
        }
        *end = bitsfs_find_next_bit(bh->b_data, block_length, *start);
    }
fail:
    return err;
}

static int alloc_batch_blocks(struct inode *inode, int count, unsigned long *pos) 
{
    int err;
    unsigned long start, end;
    struct buffer_head *bh;

    bitsfs_msg(inode->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "Alloc batch blocks, ino=%lu, count=%d", inode->i_ino, count);

    bh = read_block_bitmap(inode->i_sb, BITSFS_BLKBMP_BLOCK);
    err = find_avai_block_range(bh, count, 0, &start, &end);
    if(err) {
        bitsfs_msg(inode->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
                "No enough blocks to alloc");
        goto fail;
    }
    *pos = start;
    for (;start < end; ++start) 
        bitsfs_set_bit(start, bh->b_data);
fail:
    brelse(bh);
    return err;
}

int bitsfs_get_block(struct inode *inode, sector_t iblock,
        struct buffer_head *bh_result, int create)
{
    bool new = false;
    int n, err;
    u64 blk_no;
    unsigned long block_cnt, block_no;
    int pos = 0, offset = 0;
    struct super_block *sb = inode->i_sb;
    struct bitsfs_inode_info *bi = BITSFS_I2BI(inode); 

    if (iblock + 1 <= BITSFS_DDIR_BLOCKS) {
        pos = iblock;
        block_cnt = iblock + 1;
        for (n = 0;n <= pos; ++n) {
            if (!bi->i_data[n]) {
                err = alloc_single_block(inode, &block_no);
                if (err)
                    goto fail;
                bi->i_data[n] = block_no + BITSFS_DATA_BLOCK;
                new = true;
            }
        }
    } else {
        pos = iblock + 1 - BITSFS_DDIR_BLOCKS;
        pos = (pos % BITSFS_NDIR_BLOCK_COUNT == 0) ? (pos / BITSFS_NDIR_BLOCK_COUNT) : (pos / BITSFS_NDIR_BLOCK_COUNT + 1);
        pos += (BITSFS_DDIR_BLOCKS - 1);
        offset = pos % BITSFS_NDIR_BLOCK_COUNT;
        block_cnt = BITSFS_DDIR_BLOCKS;

        /* Exceed supported max block size */
        if(pos >= BITSFS_TMAX_BLOCKS) {
            bitsfs_msg(sb, KERN_ERR, __func__, __FILE__, __LINE__, 
                    "warning: iblock is too big, iblock=%lu", iblock);
            return -EIO;
        }

        /* Alloc 1st level blocks */
        for (n = 0;n < BITSFS_DDIR_BLOCKS; ++n) {
            if (!bi->i_data[n]) {
                err = alloc_single_block(inode, &block_no);
                if (err)
                    goto fail;
                bi->i_data[n] = block_no + BITSFS_DATA_BLOCK;
                new = true;
            }
        }

        /* Alloc 2nd level blocks, batch size: 1024 */
        for (n = BITSFS_DDIR_BLOCKS;n <= pos; ++n) {
            if (!bi->i_data[n]) {
                err = alloc_batch_blocks(inode, BITSFS_NDIR_BLOCK_COUNT, &block_no) ;
                if (err)
                    goto fail;
                bi->i_data[n] = block_no + BITSFS_DATA_BLOCK;
                new = true;
            }
            block_cnt += BITSFS_NDIR_BLOCK_COUNT;
        }
    }

    blk_no = bi->i_data[pos] + offset;
    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "bitsfs_get_block, iblock=%lu create=%d pos=%d offset=%d blk_no=%lu block_cnt=%lu block_bits=%d", 
            iblock, create, pos, offset, blk_no, block_cnt, inode->i_blkbits);

    map_bh(bh_result, sb, blk_no);
    bh_result->b_size = (block_cnt << inode->i_blkbits);
    if (new)
        set_buffer_new(bh_result);
    //set_buffer_boundary(bh_result);
    return 0;
fail:
    bitsfs_msg(sb, KERN_ERR, __func__, __FILE__, __LINE__, 
            "Failed to get block, err=%d", err);
    return err;
}

void __bitsfs_truncate_blocks(struct inode *inode)
{
    int n, k;
    struct super_block *sb = inode->i_sb;
    struct bitsfs_inode_info *bi = BITSFS_I2BI(inode);
    struct buffer_head *bh;

    bh = read_block_bitmap(sb, BITSFS_BLKBMP_BLOCK);
    for (n = 0;n < BITSFS_DDIR_BLOCKS; ++n) {
        if (!bi->i_data[n]) {
            bitsfs_clear_bit(bi->i_data[n], bh->b_data);
        }
    }

    for (;n < BITSFS_TMAX_BLOCKS; ++n) {
        if (!bi->i_data[n]) {
            for (k = 0; k < BITSFS_NDIR_BLOCK_COUNT; ++k)
                bitsfs_clear_bit(bi->i_data[n] + k, bh->b_data);
        }
    }
    brelse(bh);
}


void bitsfs_truncate_blocks(struct inode *inode, loff_t offset)
{
    struct super_block *sb = inode->i_sb;
    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "Truncate block start, ino=%lu i_mode=%d offset=%lu", 
            inode->i_ino, inode->i_mode, offset);
	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
	    S_ISLNK(inode->i_mode)))
		return;
	__bitsfs_truncate_blocks(inode);
}

static void bitsfs_write_failed(struct address_space *mapping, loff_t to)
{
    struct inode *inode = mapping->host;
    if (to > inode->i_size) {
        truncate_pagecache(inode, inode->i_size);
        bitsfs_truncate_blocks(inode, inode->i_size);
    }
}

static int bitsfs_readpage(struct file *file, struct page *page)
{
    return mpage_readpage(page, bitsfs_get_block);
}

static int bitsfs_writepage(struct page *page, struct writeback_control *wbc)
{
    return block_write_full_page(page, bitsfs_get_block, wbc);
}

static void bitsfs_readahead(struct readahead_control *rac)
{
    mpage_readahead(rac, bitsfs_get_block);
}

static int bitsfs_write_begin(struct file *file, struct address_space *mapping,
        loff_t pos, unsigned len, unsigned flags,
        struct page **pagep, void **fsdata)
{
    int ret;
    ret = block_write_begin(mapping, pos, len, flags, pagep,
                bitsfs_get_block);
    if (ret < 0)
        bitsfs_write_failed(mapping, pos + len);
    return ret;
}

static int bitsfs_write_end(struct file *file, struct address_space *mapping,
            loff_t pos, unsigned len, unsigned copied,
            struct page *page, void *fsdata)
{
    int ret;
    ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
    if (ret < len)
        bitsfs_write_failed(mapping, pos + len);
    return ret;
}

static sector_t bitsfs_bmap(struct address_space *mapping, sector_t block)
{
    return generic_block_bmap(mapping,block,bitsfs_get_block);
}

static ssize_t bitsfs_direct_IO(struct kiocb *iocb, struct iov_iter *iter)
{
    ssize_t ret;
    struct file *file = iocb->ki_filp;
    struct address_space *mapping = file->f_mapping;
    struct inode *inode = mapping->host;
    size_t count = iov_iter_count(iter);
    loff_t offset = iocb->ki_pos;

    ret = blockdev_direct_IO(iocb, inode, iter, bitsfs_get_block);
    if (ret < 0 && iov_iter_rw(iter) == WRITE)
        bitsfs_write_failed(mapping, offset + count);
    return ret;
}

static int bitsfs_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
    return mpage_writepages(mapping, wbc, bitsfs_get_block);
}

static int bitsfs_dax_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
    struct bitsfs_sb_info *sbi = BITFS_S2SI(mapping->host->i_sb);
    return dax_writeback_mapping_range(mapping, sbi->s_daxdev, wbc);
}

const struct address_space_operations bitsfs_aops = {
    .set_page_dirty   = __set_page_dirty_buffers,
    .readpage         = bitsfs_readpage,
    .readahead        = bitsfs_readahead,
    .writepage        = bitsfs_writepage,
    .write_begin      = bitsfs_write_begin,
    .write_end        = bitsfs_write_end,
    .bmap             = bitsfs_bmap,
    .direct_IO        = bitsfs_direct_IO,
    .writepages       = bitsfs_writepages,
    .migratepage      = buffer_migrate_page,
    .is_partially_uptodate = block_is_partially_uptodate,
    .error_remove_page = generic_error_remove_page,
};

const struct address_space_operations bitsfs_dax_aops = {
    .writepages      = bitsfs_dax_writepages,
    .direct_IO       = noop_direct_IO,
    .set_page_dirty  = __set_page_dirty_buffers,
    .invalidatepage  = noop_invalidatepage,
};