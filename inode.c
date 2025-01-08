#include "bitsfs.h"
#include <linux/time.h>
#include <linux/highuid.h>
#include <linux/pagemap.h>
#include <linux/dax.h>
#include <linux/blkdev.h>
#include <linux/quotaops.h>
#include <linux/writeback.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/fiemap.h>
#include <linux/iomap.h>
#include <linux/namei.h>
#include <linux/uio.h>

void bitsfs_set_file_ops(struct inode *inode);
void bitsfs_set_dir_ops(struct inode *inode);

static struct buffer_head* read_inode_bitmap(struct super_block *sb)
{
    struct buffer_head *bh = NULL;
    bh = sb_bread(sb, le32_to_cpu(BITSFS_INDBMP_BLOCK));
    if (!bh)
        bitsfs_msg(sb, KERN_ERR, __func__, __FILE__, __LINE__,
                "Cannot read inode bitmap");
    return bh;
}

void set_root_inode_bitmap(struct inode *inode, int pos) 
{
    int ret;
    struct buffer_head *bh;
    bh = read_inode_bitmap(inode->i_sb);
    ret = bitsfs_set_bit(pos, bh->b_data);
    bitsfs_msg(inode->i_sb, KERN_INFO, __func__, __FILE__, __LINE__,
            "Set root inode bitmap pos=%d, ret=%d", pos, ret);
    brelse(bh);
}

static struct bitsfs_inode *bitsfs_read_inode(struct super_block *sb, ino_t ino,
                    struct buffer_head **p)
{
    unsigned long block;
    unsigned long offset;
    struct buffer_head *bh;
    struct bitsfs_inode *raw_inode;
    struct bitsfs_super_block *bs = BITFS_S2SI(sb)->s_bs;

    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "Read inode from disk start, ino=%lu", ino);
            
    *p = NULL;
    if (ino != BITSFS_ROOT_INO && ino < BITSFS_ROOT_INO)
        goto Einval;

    block = bs->s_inode_size * (ino - 1) / BITSFS_BLOCK_SIZE;
    offset = bs->s_inode_size * (ino - 1) % BITSFS_BLOCK_SIZE;

    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "Read inode from disk, block=%lu offset=%lu", block, offset);

    /* Read block from buff */
    if (!(bh = sb_bread(sb, BITSFS_INDTBL_BLOCK + block)))
        goto Eio;

    *p = bh;
    raw_inode = (struct bitsfs_inode*)(bh->b_data + offset);

    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "Read inode from disk end, ino=%lu raw_inode=%p", ino, raw_inode);
    return raw_inode;
Einval:
    bitsfs_msg(sb, KERN_ERR, __func__, __FILE__, __LINE__, 
            "Bad inode number: %lu", (unsigned long) ino);
    return ERR_PTR(-EINVAL);
Eio:
    bitsfs_msg(sb, KERN_ERR, __func__, __FILE__, __LINE__, 
           "Unable to read inode block - inode=%lu, block=%lu",
           (unsigned long) ino, block);
    return ERR_PTR(-EIO);
}

int bitsfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
    int n, err = 0;
    ino_t ino = inode->i_ino;
    struct bitsfs_inode_info *bi = BITSFS_I2BI(inode);
    struct super_block *sb = inode->i_sb;
    struct buffer_head * bh;

    /* inode on disk */
    struct bitsfs_inode *raw_inode = bitsfs_read_inode(sb, ino, &bh);

    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__, 
           "Write inode start, ino=%lu, i_block=%p",
           (unsigned long)ino, bi->i_data);

    raw_inode->i_mode = cpu_to_le16(inode->i_mode);
    raw_inode->i_links_count = cpu_to_le16(inode->i_nlink);
    raw_inode->i_size = cpu_to_le32(inode->i_size);
    raw_inode->i_atime = cpu_to_le32(inode->i_atime.tv_sec);
    raw_inode->i_ctime = cpu_to_le32(inode->i_ctime.tv_sec);
    raw_inode->i_mtime = cpu_to_le32(inode->i_mtime.tv_sec);

    raw_inode->i_blocks = cpu_to_le32(inode->i_blocks);
    raw_inode->i_dtime = cpu_to_le32(bi->i_dtime);
    raw_inode->i_flags = cpu_to_le32(bi->i_flags);
    raw_inode->i_file_acl = cpu_to_le32(bi->i_file_acl);

    if (!S_ISREG(inode->i_mode))
        raw_inode->i_dir_acl = cpu_to_le32(bi->i_dir_acl);
    
    for (n = 0; n < BITSFS_TMAX_BLOCKS; n++)
        raw_inode->i_block[n] = bi->i_data[n];

    mark_buffer_dirty(bh);
    bi->i_state &= ~BITSFS_STATE_NEW;

    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__, 
           "Write inode end, ino=%lu i_state=%d", ino, bi->i_state);
    
    brelse (bh);
    return err;
}

struct inode *bitsfs_new_inode(struct inode *dir, umode_t mode,
                 const struct qstr *qstr)
{
    ino_t ino = 0;
    struct inode *inode;
    struct bitsfs_inode_info *ei;
    struct super_block *sb;
    struct bitsfs_sb_info *sbi;
    struct bitsfs_super_block *bs;
    struct buffer_head *bitmap_bh = NULL;
    int err;

    sb = dir->i_sb;
    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "New inode start, child_name=%s", qstr->name);

    inode = new_inode(sb);
    if (!inode)
        return ERR_PTR(-ENOMEM);

    ei = BITSFS_I2BI(inode);
    sbi = BITSFS_B2BI(sb);
    bs = sbi->s_bs;
    
    bitmap_bh = read_inode_bitmap(sb);
    ino = bitsfs_find_next_zero_bit((unsigned long *)bitmap_bh->b_data, bs->s_inodes_count, BITSFS_ROOT_INO - 1);
    bitsfs_set_bit(ino, bitmap_bh->b_data);

    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__, 
           "New inode start got next zero bit, pos=%lu inodes_count=%lu offset=%d", 
           ino, bs->s_inodes_count, (BITSFS_ROOT_INO - 1));
  
    ino += 1;
    mark_buffer_dirty(bitmap_bh);
    brelse(bitmap_bh);

    percpu_counter_dec(&sbi->s_freeinodes_counter);
    if (S_ISDIR(mode))
        percpu_counter_inc(&sbi->s_dirs_counter);

    inode->i_mode = mode;
	inode->i_uid = current_fsuid();
	inode->i_gid = dir->i_gid;
    inode->i_ino = ino;
    inode->i_blocks = 0;
    inode->i_mtime = inode->i_atime = inode->i_ctime = current_time(inode);
    memset(ei->i_data, 0, sizeof(ei->i_data));
    ei->i_file_acl = 0;
    ei->i_dir_acl = 0;
    ei->i_dtime = 0;
    ei->i_state = BITSFS_STATE_NEW;
    if (insert_inode_locked(inode) < 0) {
        bitsfs_msg(sb, KERN_ERR, __func__, __FILE__, __LINE__, 
                "inode number already in use - inode=%lu", (unsigned long)ino);
        err = -EIO;
        goto fail;
    }
    mark_inode_dirty(inode);
    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "New inode start end, ino=%lu, i_state=%d, i_mode=%d", 
            inode->i_ino, inode->i_state, inode->i_mode);
    return inode;
fail:
    make_bad_inode(inode);
    iput(inode);
    return ERR_PTR(err);
}

struct inode *bitsfs_iget (struct super_block *sb, unsigned long ino)
{
    int n;
    struct inode *inode;
    struct bitsfs_inode_info *bi;
    struct bitsfs_inode *raw_inode;
    struct buffer_head *bh = NULL;
    long ret = -EIO;

    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "Get inode start, ino=%lu", ino);

    inode = iget_locked(sb, ino);
    if (!inode)
        return ERR_PTR(-ENOMEM);
    if (!(inode->i_state & I_NEW))
		return inode;

    bi = BITSFS_I2BI(inode);

    raw_inode = bitsfs_read_inode(inode->i_sb, ino, &bh);
    if (IS_ERR(raw_inode)) {
        ret = PTR_ERR(raw_inode);
         goto bad_inode;
    }

    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "Get inode read inode, raw_inode=%p link_count=%d", raw_inode, raw_inode->i_links_count);

    inode->i_mode = le16_to_cpu(raw_inode->i_mode);
    set_nlink(inode, le16_to_cpu(raw_inode->i_links_count));
    inode->i_size = le32_to_cpu(raw_inode->i_size);
    inode->i_atime.tv_sec = (signed)le32_to_cpu(raw_inode->i_atime);
    inode->i_ctime.tv_sec = (signed)le32_to_cpu(raw_inode->i_ctime);
    inode->i_mtime.tv_sec = (signed)le32_to_cpu(raw_inode->i_mtime);
    inode->i_atime.tv_nsec = inode->i_mtime.tv_nsec = inode->i_ctime.tv_nsec = 0;
    inode->i_blocks = le32_to_cpu(raw_inode->i_blocks);

    if (inode->i_nlink == 0 && (inode->i_mode == 0 || bi->i_dtime)) {
        /* this inode is deleted */
        ret = -ESTALE;
        goto bad_inode;
    }

    bi->i_dtime = le32_to_cpu(raw_inode->i_dtime);
    bi->i_flags = le32_to_cpu(raw_inode->i_flags);
    bi->i_file_acl = le32_to_cpu(raw_inode->i_file_acl);
    bi->i_dir_acl = 0;

    if (S_ISDIR(inode->i_mode))
        bi->i_dir_acl = le32_to_cpu(raw_inode->i_dir_acl);

    bi->i_state = 0;

    for (n = 0; n < BITSFS_NDIR_BLOCKS; n++)
        bi->i_data[n] = raw_inode->i_block[n];

    if (S_ISREG(inode->i_mode)) {
        bitsfs_set_file_ops(inode);
    } else if (S_ISDIR(inode->i_mode)) {
        bitsfs_set_dir_ops(inode);
    }

    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__,
            "Get inode end, ino=%lu i_state=%lu, i_mode=%d",
            inode->i_ino, inode->i_state, S_ISDIR(inode->i_mode));
    
    brelse (bh);
    unlock_new_inode(inode);
    return inode;
bad_inode:
    brelse(bh);
    iget_failed(inode);
    return ERR_PTR(ret);
}

void bitsfs_free_inode (struct inode * inode)
{
    unsigned long ino;
    struct super_block *sb = inode->i_sb;
    struct buffer_head *bitmap_bh;

    ino = inode->i_ino;

    bitmap_bh = read_inode_bitmap(sb);
    if (!bitmap_bh)
        return;

    /* update inode bitmaps */
    if (!test_and_clear_bit_le(ino-1, (void*)bitmap_bh->b_data))
        bitsfs_msg(sb, KERN_ERR, __func__, __FILE__, __LINE__,
            "Free inode, bit already cleared for inode %lu", ino);
    mark_buffer_dirty(bitmap_bh);
    brelse(bitmap_bh);
}

void bitsfs_evict_inode(struct inode *inode)
{
    int do_delete = 0;
    struct bitsfs_inode_info *bi;
    struct address_space* mapping = &inode->i_data;

    bi = BITSFS_I2BI(inode);

    bitsfs_msg(inode->i_sb, KERN_INFO, __func__, __FILE__, __LINE__,
            "Evict inode start, ino=%lu", inode->i_ino);
    
    bitsfs_msg(inode->i_sb, KERN_INFO, __func__, __FILE__, __LINE__,
            "Evict inode show, ino=%lu i_data=%p private_data=%p nrpages=%d i_state=%lu i_freeing=%d i_clear=%d", 
            inode->i_ino, inode->i_data, mapping->private_data, mapping->nrpages, 
            inode->i_state, (inode->i_state & I_FREEING), (inode->i_state & I_CLEAR));

    if (!inode->i_nlink && !is_bad_inode(inode)) {
        do_delete = 1;
    }

    truncate_inode_pages_final(&inode->i_data);
    if (do_delete) {
         sb_start_intwrite(inode->i_sb);
        /* set dtime */
        bi->i_dtime = ktime_get_real_seconds();
        mark_inode_dirty(inode);
        bitsfs_write_inode(inode, NULL);
        /* truncate to 0 */
         inode->i_size = 0;
        if (inode->i_blocks)
            bitsfs_truncate_blocks(inode, 0);
    }

    invalidate_inode_buffers(inode);
    clear_inode(inode);

    bitsfs_msg(inode->i_sb, KERN_INFO, __func__, __FILE__, __LINE__,
            "Evict inode end, ino=%lu", inode->i_ino);

    if (do_delete) {
        bitsfs_free_inode(inode);
        sb_end_intwrite(inode->i_sb);
    }
}

void bitsfs_set_file_ops(struct inode *inode)
{
    inode->i_op = &bitsfs_file_inode_operations;
    inode->i_fop = &bitsfs_file_operations;
    if (IS_DAX(inode))
        inode->i_mapping->a_ops = &bitsfs_dax_aops;
    else
        inode->i_mapping->a_ops = &bitsfs_aops;
}

void bitsfs_set_dir_ops(struct inode *inode)
{
    inode->i_op = &bitsfs_dir_inode_operations;
    inode->i_fop = &bitsfs_dir_operations;
    inode->i_mapping->a_ops = &bitsfs_aops;
}

const struct file_operations bitsfs_file_operations = {
    .llseek        = generic_file_llseek,
    .read_iter     = generic_file_read_iter,
    .write_iter    = generic_file_write_iter,
    .mmap          = generic_file_mmap,
    .open          = generic_file_open,
    .fsync         = generic_file_fsync,
    .get_unmapped_area = thp_get_unmapped_area,
    .splice_read   = generic_file_splice_read,
    .splice_write  = iter_file_splice_write,
};

const struct inode_operations bitsfs_file_inode_operations = {
};