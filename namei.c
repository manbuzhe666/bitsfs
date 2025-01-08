#include "bitsfs.h"

static struct dentry *bitsfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
    struct inode *inode;
    ino_t ino;
    int res;
    
    bitsfs_msg(dir->i_sb, KERN_ERR, __func__, __FILE__, __LINE__, 
            "bitsfs_lookup start, d_name=%s", dentry->d_name.name);

    if (dentry->d_name.len > DENT_NAME_LEN)
        return ERR_PTR(-ENAMETOOLONG);

    res = bitsfs_get_ino_by_name(dir, &dentry->d_name, &ino);
    if (res) {
        if (res != -ENOENT)
            return ERR_PTR(res);
        inode = NULL;
    } else {
        inode = bitsfs_iget(dir->i_sb, ino);
        if (inode == ERR_PTR(-ESTALE)) {
            bitsfs_msg(dir->i_sb, KERN_ERR, __func__, __FILE__, __LINE__,
                "deleted inode referenced: %lu", (unsigned long) ino);
            return ERR_PTR(-EIO);
        }
    }
    return d_splice_alias(inode, dentry);
}

static inline int bitsfs_add_nondir(struct dentry *dentry, struct inode *inode)
{
    int err = bitsfs_add_link(dentry, inode);
    if (!err) {
        bitsfs_msg(inode->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "bitsfs_add_nondir, i_state=%d", (inode->i_state & I_NEW));
        d_instantiate_new(dentry, inode);
        return 0;
    }
    inode_dec_link_count(inode);
    discard_new_inode(inode);
    return err;
}

static int bitsfs_create(struct inode *dir, struct dentry *dentry,
            umode_t mode, bool excl)
{
    struct inode *inode;

    bitsfs_msg(dir->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "bitsfs_create start");

    inode = bitsfs_new_inode(dir, mode, &dentry->d_name);
    if (IS_ERR(inode))
        return PTR_ERR(inode);

    bitsfs_msg(dir->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "bitsfs_create end, dir_ino=%lu, mode=%d", dir->i_ino, (inode->i_mode));

    bitsfs_set_file_ops(inode);
    mark_inode_dirty(inode);
    return bitsfs_add_nondir(dentry, inode);
}

static int bitsfs_link(struct dentry * old_dentry, struct inode * dir,
    struct dentry *dentry)
{
    struct inode *inode = d_inode(old_dentry);
    int err;

    bitsfs_msg(dir->i_sb, KERN_ERR, __func__, __FILE__, __LINE__, 
            "bitsfs_link start");

    inode->i_ctime = current_time(inode);
    inode_inc_link_count(inode);
    ihold(inode);

    err = bitsfs_add_link(dentry, inode);
    if (!err) {
        d_instantiate(dentry, inode);
        return 0;
    }
    
    inode_dec_link_count(inode);
    iput(inode);

    bitsfs_msg(dir->i_sb, KERN_ERR, __func__, __FILE__, __LINE__, 
            "bitsfs_link end");
    return err;
}

static int bitsfs_unlink(struct inode *dir, struct dentry *dentry)
{
    int err;
    struct inode *inode = d_inode(dentry);
    struct bitsfs_dir_entry *de;
    struct page *page;
    void *page_addr;

    bitsfs_msg(dir->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "bitsfs_unlink start, ino=%lu", inode->i_ino);
    
    de = bitsfs_find_entry(dir, &dentry->d_name, &page, &page_addr);
    if (IS_ERR(de)) {
        err = PTR_ERR(de);
        goto out;
    }

    bitsfs_msg(dir->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "bitsfs_unlink delete entry, page=%p page_addr=%p", page, page_addr);
    err = bitsfs_delete_entry(dir, de, page, page_addr);
    bitsfs_put_page(page, page_addr);
    if (err)
        goto out;

    inode->i_ctime = dir->i_ctime;
    inode_dec_link_count(inode);
    err = 0;

    bitsfs_msg(dir->i_sb, KERN_ERR, __func__, __FILE__, __LINE__, 
            "bitsfs_unlink end, ino=%lu", inode->i_ino);
out:
    return err;
}

static int bitsfs_mkdir(struct inode *dir, struct dentry * dentry, umode_t mode)
{
    struct inode * inode;
    int err;

    bitsfs_msg(dir->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "bitsfs_mkdir start");

    inode_inc_link_count(dir);

    inode = bitsfs_new_inode(dir, S_IFDIR | mode, &dentry->d_name);
    err = PTR_ERR(inode);
    if (IS_ERR(inode))
        goto out_dir;

    bitsfs_msg(dir->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "bitsfs_mkdir new inode, ino=%lu", inode->i_ino);
    
    bitsfs_set_dir_ops(inode);
    inode_inc_link_count(inode);

    err = bitsfs_make_empty(inode, dir);
    if (err)
        goto out_fail;

    bitsfs_msg(dir->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "bitsfs_mkdir make empty");
    
    err = bitsfs_add_link(dentry, inode);
    if (err)
        goto out_fail;

    bitsfs_msg(dir->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "bitsfs_mkdir add link");
    
    d_instantiate_new(dentry, inode);

    bitsfs_msg(dir->i_sb, KERN_ERR, __func__, __FILE__, __LINE__, 
            "bitsfs_mkdir end");
out:
    return err;

out_fail:
    inode_dec_link_count(inode);
    inode_dec_link_count(inode);
    discard_new_inode(inode);
out_dir:
    inode_dec_link_count(dir);
    goto out;
}

static int bitsfs_rmdir (struct inode *dir, struct dentry *dentry)
{
    struct inode * inode = d_inode(dentry);
    int err = -ENOTEMPTY;

    bitsfs_msg(dir->i_sb, KERN_ERR, __func__, __FILE__, __LINE__, 
            "bitsfs_rmdir start");

    if (bitsfs_empty_dir(inode)) {
        err = bitsfs_unlink(dir, dentry);
        if (!err) {
            inode->i_size = 0;
            inode_dec_link_count(inode);
            inode_dec_link_count(dir);
        }
    }
    bitsfs_msg(dir->i_sb, KERN_ERR, __func__, __FILE__, __LINE__, 
            "bitsfs_rmdir end");
    return err;
}

static int bitsfs_tmpfile(struct inode *dir, struct dentry *dentry, umode_t mode)
{
    struct inode *inode;

    bitsfs_msg(dir->i_sb, KERN_ERR, __func__, __FILE__, __LINE__, 
            "bitsfs_tmpfile start");

    inode = bitsfs_new_inode(dir, mode, NULL);
    if (IS_ERR(inode))
        return PTR_ERR(inode);

    bitsfs_set_file_ops(inode);
    mark_inode_dirty(inode);
    d_tmpfile(dentry, inode);
    unlock_new_inode(inode);

    bitsfs_msg(dir->i_sb, KERN_ERR, __func__, __FILE__, __LINE__, 
            "bitsfs_tmpfile end");
    return 0;
}

static int bitsfs_rename (
            struct inode *old_dir, struct dentry *old_dentry,
            struct inode *new_dir, struct dentry *new_dentry,
            unsigned int flags)
{
    int err;
    struct inode *old_inode = d_inode(old_dentry);
    struct inode *new_inode = d_inode(new_dentry);
    struct page *dir_page = NULL;
    struct page *old_page = NULL;
    void *dir_page_addr;
    void *old_page_addr;
    struct bitsfs_dir_entry *dir_de = NULL;
    struct bitsfs_dir_entry *old_de = NULL;

    bitsfs_msg(old_dir->i_sb, KERN_ERR, __func__, __FILE__, __LINE__, 
            "bitsfs_rename start");

    if (flags & ~RENAME_NOREPLACE)
        return -EINVAL;

    old_de = bitsfs_find_entry(old_dir, &old_dentry->d_name, &old_page,
                 &old_page_addr);
    if (IS_ERR(old_de)) {
        err = PTR_ERR(old_de);
        goto out;
    }

    if (S_ISDIR(old_inode->i_mode)) {
        err = -EIO;
        dir_de = bitsfs_dotdot(old_inode, &dir_page, &dir_page_addr);
        if (!dir_de)
            goto out_old;
    }

    if (new_inode) {
        void *page_addr;
        struct page *new_page;
        struct bitsfs_dir_entry *new_de;

        err = -ENOTEMPTY;
        if (dir_de && !bitsfs_empty_dir(new_inode))
            goto out_dir;

        new_de = bitsfs_find_entry(new_dir, &new_dentry->d_name,
                     &new_page, &page_addr);
        if (IS_ERR(new_de)) {
            err = PTR_ERR(new_de);
            goto out_dir;
        }
        bitsfs_set_link(new_dir, new_de, new_page, page_addr, old_inode, 1);
        bitsfs_put_page(new_page, page_addr);
        new_inode->i_ctime = current_time(new_inode);
        if (dir_de)
            drop_nlink(new_inode);
        inode_dec_link_count(new_inode);
    } else {
        err = bitsfs_add_link(new_dentry, old_inode);
        if (err)
            goto out_dir;
        if (dir_de)
            inode_inc_link_count(new_dir);
    }

    /*
     * Like most other Unix systems, set the ctime for inodes on a
      * rename.
     */
    old_inode->i_ctime = current_time(old_inode);
    mark_inode_dirty(old_inode);

    bitsfs_delete_entry(old_dir, old_de, old_page, old_page_addr);

    if (dir_de) {
        if (old_dir != new_dir)
            bitsfs_set_link(old_inode, dir_de, dir_page,
                      dir_page_addr, new_dir, 0);

        bitsfs_put_page(dir_page, dir_page_addr);
        inode_dec_link_count(old_dir);
    }

    bitsfs_msg(old_dir->i_sb, KERN_ERR, __func__, __FILE__, __LINE__, 
            "bitsfs_tmpfile end");
    bitsfs_put_page(old_page, old_page_addr);
    return 0;

out_dir:
    if (dir_de)
        bitsfs_put_page(dir_page, dir_page_addr);
out_old:
    bitsfs_put_page(old_page, old_page_addr);
out:
    return err;
}

const struct inode_operations bitsfs_dir_inode_operations = {
    .create      = bitsfs_create,
    .lookup      = bitsfs_lookup,
    .link        = bitsfs_link,
    .unlink      = bitsfs_unlink,
    .mkdir       = bitsfs_mkdir,
    .rmdir       = bitsfs_rmdir,
    .rename      = bitsfs_rename,
    .tmpfile     = bitsfs_tmpfile,
};
