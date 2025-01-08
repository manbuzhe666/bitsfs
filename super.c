#include <linux/module.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/parser.h>
#include <linux/random.h>
#include <linux/buffer_head.h>
#include <linux/exportfs.h>
#include <linux/vfs.h>
#include <linux/seq_file.h>
#include <linux/mount.h>
#include <linux/log2.h>
#include <linux/uaccess.h>
#include <linux/dax.h>
#include <linux/iversion.h>
#include "bitsfs.h"

/** inode cache */
static struct kmem_cache * bitsfs_inode_cachep;

static struct inode *bitsfs_alloc_inode(struct super_block *sb)
{
	struct bitsfs_inode_info *bi;
    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "Alloc inode start");
	bi = kmem_cache_alloc(bitsfs_inode_cachep, GFP_KERNEL);
	if (!bi)
		return NULL;
	inode_set_iversion(&bi->vfs_inode, 1);
    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "Alloc inode end, bi=%p", bi);
	return &bi->vfs_inode;
}

static void bitsfs_free_kcache(struct inode *inode)
{
	kmem_cache_free(bitsfs_inode_cachep, BITSFS_I2BI(inode));
}

static void bitsfs_put_super(struct super_block * sb)
{
	struct bitsfs_sb_info *sbi = BITFS_S2SI(sb);
	percpu_counter_destroy(&sbi->s_freeblocks_counter);
	percpu_counter_destroy(&sbi->s_freeinodes_counter);
	percpu_counter_destroy(&sbi->s_dirs_counter);
	brelse (sbi->s_sbh);
	sb->s_fs_info = NULL;
	fs_put_dax(sbi->s_daxdev);
	kfree(sbi);
}

static const struct super_operations bitsfs_sb_ops = {
    .alloc_inode    = bitsfs_alloc_inode,
    .write_inode    = bitsfs_write_inode,
    .destroy_inode	= bitsfs_free_kcache,
    .evict_inode    = bitsfs_evict_inode,
    .put_super      = bitsfs_put_super,
};

static int bitsfs_fill_super(struct super_block *sb, void *data, int silent)
{
    int ret = 0;
    int blocksize = BITSFS_BLOCK_SIZE;
    unsigned long sb_block = BITSFS_SUPER_BLOCK;
	unsigned long sb_offset = 0;
    struct buffer_head *bh;
    struct dax_device *dax_dev;
    struct bitsfs_super_block *bs;
    struct bitsfs_sb_info *sbi;
    struct inode *root;

    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "Fill super block");

    dax_dev = fs_dax_get_by_bdev(sb->s_bdev);

    sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
    if(!sbi) {
        goto failed;
    }
    sb->s_fs_info = sbi;

    blocksize = sb_min_blocksize(sb, BITSFS_BLOCK_SIZE);
    if (blocksize != BITSFS_BLOCK_SIZE) {
		sb_block = (sb_block * BITSFS_BLOCK_SIZE) / blocksize;
		sb_offset = (sb_block * BITSFS_BLOCK_SIZE) % blocksize;
	}
    
    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "Read block dev bd_dev=%lu blk_size1=%lu blk_size2=%d sb_block=%lu sb_offset=%lu", 
            sb->s_bdev->bd_dev, sb->s_bdev->bd_block_size, blocksize, sb_block, sb_offset);

    if (!(bh = sb_bread(sb, sb_block))) {
        bitsfs_msg(sb, KERN_ERR, __func__, __FILE__, __LINE__, "Unable to read super block");
        goto failed;
    }

    bs = (struct bitsfs_super_block*) ((char*)bh->b_data + sb_offset);
    sbi->s_sbh = bh;
    sbi->s_bs = bs;
    sbi->s_daxdev = dax_dev;
    sbi->s_mount_state = le16_to_cpu(bs->s_state);
    sbi->s_sb_block = BITSFS_SUPER_BLOCK;
    sbi->s_first_ino = le16_to_cpu(bs->s_first_ino);

    sb->s_magic = le16_to_cpu(bs->s_magic);
    sb->s_flags |= SB_POSIXACL;
    sb->s_blocksize = le32_to_cpu(bs->s_block_size);
    sb->s_time_min = S32_MIN;
    sb->s_time_max = S32_MAX;

    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "finish reading bitsfs super block, s_name=%s s_magic=%d s_magic=%lu", 
            bs->s_name, BITSFS_SUPER_MAGIC, sb->s_magic);
    
    if (sb->s_magic != BITSFS_SUPER_MAGIC)
        goto cantfind_bitsfs;
    
    sb->s_op = &bitsfs_sb_ops;

    root = bitsfs_iget(sb, BITSFS_ROOT_INO);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto failed;
	}

    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "Root inode info, i_mode=%d i_blocks=%d i_size=%lu i_state=%d", 
            root->i_mode, root->i_blocks, root->i_size, root->i_state);

	if (!S_ISDIR(root->i_mode) || !root->i_blocks || !root->i_size) {
		iput(root);
		bitsfs_msg(sb, KERN_ERR, __func__, __FILE__, __LINE__, 
                "error: corrupt root inode");
		goto failed;
	}

    set_root_block_bitmap(root, 0);
    set_root_inode_bitmap(root, BITSFS_ROOT_INO - 1);

	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
	    bitsfs_msg(sb, KERN_ERR, __func__, __FILE__, __LINE__, 
                 "error: get root inode failed");
	 	ret = -ENOMEM;
		goto failed;
	}

    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__,  "End fill super block");
    return 0;
cantfind_bitsfs:
    bitsfs_msg(sb, KERN_ERR, __func__, __FILE__, __LINE__, 
            "Cannot find valid bitsfs on disk");
failed:
    brelse(bh);
    kfree(sbi);
    return ret;
}

static void kill_super_block(struct super_block *sb)
{
    struct dentry *root;
    struct inode *rinode;

    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__,  
            "kill_super_block start"); 

    root = sb->s_root;
    rinode = root->d_inode;

    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__,  
            "kill_super_block, dentry=%p rinode=%p i_state=%lu", root, 
            rinode, rinode->i_state);
    WARN_ON((rinode->i_state & I_NEW));
	kill_block_super(sb);
    bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__,  "kill_super_block end");
}

static struct dentry *bitsfs_mount(struct file_system_type *fs_type,
    int flags, const char *dev_name, void *data) 
{
    printk("Bitsfs bitsfs_mount name=%s, dev=%s\n", fs_type->name, dev_name);
    return mount_bdev(fs_type, flags, dev_name, data, bitsfs_fill_super);
}

static void init_once(void *foo)
{
    struct bitsfs_inode_info *bi = (struct bitsfs_inode_info*) foo;
    inode_init_once(&bi->vfs_inode);
}

static int __init init_inodecache(void)
{
    bitsfs_inode_cachep = kmem_cache_create_usercopy("bitsfs_inode_cache",
                sizeof(struct bitsfs_inode_info), 0,
                (SLAB_RECLAIM_ACCOUNT|SLAB_MEM_SPREAD|
                    SLAB_ACCOUNT),
                offsetof(struct bitsfs_inode_info, i_data),
                sizeof_field(struct bitsfs_inode_info, i_data),
                init_once);
    if (bitsfs_inode_cachep == NULL)
        return -ENOMEM;
    return 0;
}

static void destroy_inodecache(void)
{
    kmem_cache_destroy(bitsfs_inode_cachep);
}

static struct file_system_type bitsfs_type = {
    .owner       = THIS_MODULE,
    .name        = "bitsfs",
    .mount       = bitsfs_mount,
    .kill_sb     = kill_super_block,
    .fs_flags    = FS_REQUIRES_DEV,
};
MODULE_ALIAS_FS("bitsfs");

static int __init init_bitsfs(void)
{
    int err;

    err = init_inodecache();
    if (err)
        return err;
    printk("Bitsfs init_bitsfs init inode cache\n");
    err = register_filesystem(&bitsfs_type);
    if (err)
        goto out;
    
    printk("Bitsfs init_bitsfs end\n");
    return 0;
out:
    printk("Bitsfs init_bitsfs err=%d\n", err);
    destroy_inodecache();
    return err;
}

static void __exit exit_bitsfs(void)
{
    printk("Bitsfs exit_bitsfs start \n");
    unregister_filesystem(&bitsfs_type);
    destroy_inodecache();
    printk("Bitsfs exit_bitsfs end \n");
}

MODULE_AUTHOR("Aaron of BitsObject.com");
MODULE_DESCRIPTION("Bits File System");
MODULE_LICENSE("GPL");
module_init(init_bitsfs)
module_exit(exit_bitsfs)