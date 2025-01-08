#include "bitsfs.h"
#include <linux/buffer_head.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/iversion.h>

typedef struct bitsfs_dir_entry bitsfs_dirent;

static inline int bitsfs_name_match (int len, const char * const name, bitsfs_dirent *de)
{
    if (!de->inode)
        return 0;
    return !memcmp(name, de->name, len);
}

/*
 * block size
 */
static inline unsigned bitsfs_chunk_size(struct inode *inode)
{
    return inode->i_sb->s_blocksize;
}

/*
 * Return the offset into page `page_nr' of the last valid
 * byte in that page, plus one.
 */
static unsigned bitsfs_last_byte(struct inode *inode, unsigned long page_nr)
{
    unsigned last_byte = inode->i_size;

    last_byte -= page_nr << PAGE_SHIFT;
    if (last_byte > PAGE_SIZE)
        last_byte = PAGE_SIZE;
    return last_byte;
}

static int bitsfs_prepare_chunk(struct page *page, loff_t pos, unsigned len)
{
    return __block_write_begin(page, pos, len, bitsfs_get_block);
}

static int bitsfs_commit_chunk(struct page *page, loff_t pos, unsigned len)
{
    struct address_space *mapping = page->mapping;
    struct inode *dir = mapping->host;
    int err = 0;

    inode_inc_iversion(dir);
    block_write_end(NULL, mapping, pos, len, len, page, NULL);

    if (pos+len > dir->i_size) {
        i_size_write(dir, pos+len);
        mark_inode_dirty(dir);
    }

    if (IS_DIRSYNC(dir)) {
        err = write_one_page(page);
        if (!err)
            err = sync_inode_metadata(dir, 1);
    } else {
        unlock_page(page);
    }

    return err;
}

/*
 * Calls to bitsfs_get_page()/bitsfs_put_page() must be nested according to the
 * rules documented in kmap_local_page()/kunmap_local().
 *
 * NOTE: bitsfs_find_entry() and bitsfs_dotdot() act as a call to bitsfs_get_page()
 * and should be treated as a call to bitsfs_get_page() for nesting purposes.
 */
static struct page * bitsfs_get_page(struct inode *dir, unsigned long n,
                   int quiet, void **page_addr)
{
    struct address_space *mapping;
    struct page *page;
    mapping = dir->i_mapping;

    bitsfs_msg(dir->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "bitsfs_get_page start, ino=%lu, page_no=%lu, mapping=%p,  nrpages=%lu", 
            dir->i_ino, n, mapping, mapping->nrpages);

    page = read_mapping_page(mapping, n, NULL);

    bitsfs_msg(dir->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "bitsfs_get_page end, page=%d", page);
    
    if (!IS_ERR(page)) {
        *page_addr = kmap_local_page(page);
        /*if (unlikely(!PageChecked(page))) {
            if (PageError(page) || !bitsfs_check_page(page, quiet,
                                *page_addr))
                goto fail;
        }*/
    }
    return page;
fail:
    bitsfs_put_page(page, *page_addr);
    return ERR_PTR(-EIO);
}

static inline unsigned bitsfs_validate_entry(char *base, unsigned offset, unsigned mask)
{
    bitsfs_dirent *de = (bitsfs_dirent*)(base + offset);
    bitsfs_dirent *p = (bitsfs_dirent*)(base + (offset & mask));
    while ((char*)p < (char*)de) {
        if (p->rec_len == 0)
            break;
        p = (bitsfs_dirent*)((char*)p + DENT_LEN);
    }
    return (char *)p - base;
}

static inline void bitsfs_set_de_type(bitsfs_dirent *de, struct inode *inode)
{
    /* TODO */
    // de->file_type = fs_umode_to_ftype(inode->i_mode);
}

/*
 *    Find dentry by the specific name
 */
bitsfs_dirent *bitsfs_find_entry(struct inode *dir,
            const struct qstr *child, struct page **res_page,
            void **res_page_addr)
{
    unsigned long start, n;
    unsigned long npages = dir_pages(dir); /* get all pages of dir */
    struct page *page = NULL;
    void *page_addr = NULL;
    bitsfs_dirent *de;

    bitsfs_msg(dir->i_sb, KERN_ERR, __func__, __FILE__, __LINE__, 
            "bitsfs_find_entry start, d_name=%s, npages=%lu", child->name, npages);

    if (npages == 0)
        goto out;

    /* ouput params init */
    *res_page = NULL;
    *res_page_addr = NULL;

    /* get the start lookup page */
    start = BITSFS_I2BI(dir)->i_dir_start_lookup;
    if (start >= npages)
        start = 0;

    /* loop all pages from  */
    n = start;
    do {
        char *kaddr;

        /* get the n-th page*/
        page = bitsfs_get_page(dir, n, 0, &page_addr);
        if (IS_ERR(page))
            return ERR_CAST(page);

        /* get the first dentry */
        de = (bitsfs_dirent *)page_addr;

        /* point the end of page */
        kaddr = (char*)page_addr + bitsfs_last_byte(dir, n) - DENT_LEN;

        bitsfs_msg(dir->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "bitsfs_find_entry outer loop, page_addr=%p kaddr=%p", page_addr, kaddr);

        while ((char*)de <= kaddr) {
            bitsfs_msg(dir->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
                    "bitsfs_find_entry inner loop, kaddr=%p de=%p name=%s name_len=%d rec_len=%d", 
                    kaddr, de, de->name, de->name_len, de->rec_len);

            /* goto out when dentry invalid */
            if (de->rec_len == 0) {
                bitsfs_msg(dir->i_sb, KERN_ERR, __func__, __FILE__, __LINE__,
                        "bitsfs_find_entry reach empty entry");
                bitsfs_put_page(page, page_addr);
                goto out;
            }

            /* check name match */
            if (bitsfs_name_match(child->len, child->name, de))
                goto found;
            
            /* step to next dentry */
            de = (bitsfs_dirent *)((char *)de + DENT_LEN);
        }

        /* put old page */
        bitsfs_put_page(page, page_addr);

        /* reset n if reach npages */
        if (++n >= npages)
            n = 0;
    } while (n != start);
out:
    bitsfs_msg(dir->i_sb, KERN_ERR, __func__, __FILE__, __LINE__, 
            "bitsfs_find_entry not found");
    return ERR_PTR(-ENOENT);

found:
    bitsfs_msg(dir->i_sb, KERN_ERR, __func__, __FILE__, __LINE__, 
            "bitsfs_find_entry found, res_page=%p res_page_addr=%lu", page, page_addr);
    *res_page = page;
    *res_page_addr = page_addr;

    /* set dir_start-lookup*/
    BITSFS_I2BI(dir)->i_dir_start_lookup = n;
    return de;
}

/**
 * Return the '..' directory entry
 */
bitsfs_dirent *bitsfs_dotdot(struct inode *dir, struct page **p, void **pa)
{
    void *page_addr;
    struct page *page = bitsfs_get_page(dir, 0, 0, &page_addr);
    bitsfs_dirent *de = NULL;

    if (!IS_ERR(page)) {
        de = (bitsfs_dirent *)((char *)page_addr + DENT_LEN);
        *p = page;
        *pa = page_addr;
    }
    return de;
}

int bitsfs_get_ino_by_name(struct inode *dir, const struct qstr *child, ino_t *ino)
{
    bitsfs_dirent *de;
    struct page *page;
    void *page_addr;
    
    bitsfs_msg(dir->i_sb, KERN_ERR, __func__, __FILE__, __LINE__, 
            "bitsfs_get_ino_by_name start, d_name=%s", child->name);

    de = bitsfs_find_entry(dir, child, &page, &page_addr);
    if (IS_ERR(de))
        return PTR_ERR(de);

    *ino = le32_to_cpu(de->inode);
    bitsfs_put_page(page, page_addr);
    return 0;
}

void bitsfs_set_link(struct inode *dir, bitsfs_dirent *de,
           struct page *page, void *page_addr, struct inode *inode,
           int update_times)
{
    int err;

    loff_t pos = page_offset(page) + (char*)de - (char*)page_addr;
    
    lock_page(page);
    err = bitsfs_prepare_chunk(page, pos, DENT_LEN);
    BUG_ON(err);
    
    de->inode = cpu_to_le32(inode->i_ino);
    bitsfs_set_de_type(de, inode);

    err = bitsfs_commit_chunk(page, pos, DENT_LEN);

    if (update_times)
        dir->i_mtime = dir->i_ctime = current_time(dir);
    mark_inode_dirty(dir);
}

/*
 *    Add a dentry link to inode
 */
int bitsfs_add_link(struct dentry *dentry, struct inode *inode)
{
    struct inode *dir = d_inode(dentry->d_parent);
    const char *child_name = dentry->d_name.name;
    int child_len = dentry->d_name.len;
    
    unsigned long n, npages = dir_pages(dir);
    struct page *page = NULL;
    void *page_addr = NULL;
    
    int err;
    loff_t pos;
    bitsfs_dirent * de;

    bitsfs_msg(dir->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
                    "bitsfs_add_link start, ino=%d child_name=%s", 
                    inode->i_ino, child_name);

    for (n = 0; n <= npages; n++) {
        char *kaddr;
        char *dir_end;

        page = bitsfs_get_page(dir, n, 0, &page_addr);
        err = PTR_ERR(page);
        if (IS_ERR(page))
            goto out;
        
        lock_page(page);
        de = (bitsfs_dirent*)page_addr;
        dir_end = (char*)page_addr + bitsfs_last_byte(dir, n);

        kaddr = (char*)page_addr + PAGE_SIZE - DENT_LEN;
        while ((char *)de <= kaddr) {

            bitsfs_msg(dir->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
                    "bitsfs_add_link loop, kaddr=%p dir_end=%p de=%p name=%s name_len=%d rec_len=%d", 
                    kaddr, dir_end, de, de->name, de->name_len, de->rec_len);

            if ((char *)de == dir_end || de->rec_len == 0) {
                goto got_it;
            }
            
            err = -EEXIST;
            if (bitsfs_name_match(child_len, child_name, de))
                goto out_unlock; /* found same name dentry */
            
            /* step to next dentry */
            de = (bitsfs_dirent *)((char*)de + DENT_LEN);
        }
        unlock_page(page);
        bitsfs_put_page(page, page_addr);
    }
    return -EEXIST;
got_it:
    pos = page_offset(page) + (char*)de - (char*)page_addr;
    err = bitsfs_prepare_chunk(page, pos, DENT_LEN);
    if (err)    
        goto out_unlock;
    
    /* construct dentry */
    de->inode    = cpu_to_le32(inode->i_ino);
    de->rec_len  = DENT_LEN;
    de->name_len = dentry->d_name.len;
    memcpy(de->name, child_name, child_len);
    bitsfs_set_de_type(de, inode);

    /* commit chunk */
    err = bitsfs_commit_chunk(page, pos, DENT_LEN);

    /* change inode mtime & ctime */
    dir->i_mtime = dir->i_ctime = current_time(dir);
    mark_inode_dirty(dir);

    bitsfs_msg(dir->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "bitsfs_add_link commit chunk, de=%p, name=%s, rec_len=%d", 
            de, de->name, de->rec_len);
out_put:
    bitsfs_put_page(page, page_addr);
out:
    bitsfs_msg(dir->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "bitsfs_add_link end, ino=%d child_name=%s err=%d", 
            inode->i_ino, child_name, err);
    return err;
out_unlock:
    unlock_page(page);
    goto out_put;
}

/*
 * Delete a entry by set ino = 0
 */
int bitsfs_delete_entry(struct inode *dir, bitsfs_dirent *den, struct page *page,
            char *kaddr)
{
    int err;
    loff_t pos;
    struct inode *inode = page->mapping->host;
    unsigned from = ((char*)den - kaddr) & ~(bitsfs_chunk_size(inode)-1);
    unsigned to = ((char *)den - kaddr) + DENT_LEN;

    bitsfs_msg(dir->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "bitsfs_delete_entry start, ino=%lu from=%p to=%p", inode->i_ino, from, to);

    pos = page_offset(page) + from;
    lock_page(page);
    err = bitsfs_prepare_chunk(page, pos, to - from);
    BUG_ON(err);
    bitsfs_msg(dir->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "bitsfs_delete_entry prepare, ino=%lu err=%d", inode->i_ino, err);
    den->inode = 0;
    err = bitsfs_commit_chunk(page, pos, to - from);
    inode->i_ctime = inode->i_mtime = current_time(inode);
    mark_inode_dirty(inode);

    bitsfs_msg(dir->i_sb, KERN_INFO, __func__, __FILE__, __LINE__, 
            "bitsfs_delete_entry end, ino=%lu err=%d", inode->i_ino, err);
    
    return err;
}

/*
 * Set the first fragment of directory: . and ..
 */
int bitsfs_make_empty(struct inode *inode, struct inode *parent)
{
    int err;
    void *kaddr;
    struct page *page = grab_cache_page(inode->i_mapping, 0);
    unsigned chunk_size = bitsfs_chunk_size(inode);
    bitsfs_dirent * de;

    if (!page)
        return -ENOMEM;

    err = bitsfs_prepare_chunk(page, 0, chunk_size);
    if (err) {
        unlock_page(page);
        goto fail;
    }

    kaddr = kmap_atomic(page);
    memset(kaddr, 0, chunk_size);
    de = (bitsfs_dirent *)kaddr;
    de->inode = cpu_to_le32(inode->i_ino);
    de->name_len = 1;
    de->rec_len = DENT_LEN;
    memcpy (de->name, ".\0\0", 4);
    bitsfs_set_de_type (de, inode);

    de = (bitsfs_dirent *)(kaddr + DENT_LEN);
    de->inode = cpu_to_le32(parent->i_ino);
    de->name_len = 2;
    de->rec_len = DENT_LEN;
    memcpy (de->name, "..\0", 4);
    bitsfs_set_de_type (de, inode);
    kunmap_atomic(kaddr);
    err = bitsfs_commit_chunk(page, 0, chunk_size);
fail:
    put_page(page);
    return err;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
int bitsfs_empty_dir(struct inode * inode)
{
    int dir_has_error = 0;
    unsigned long i, npages = dir_pages(inode);
    void *page_addr = NULL;
    struct page *page = NULL;
    bitsfs_dirent * de;

    for (i = 0; i < npages; i++) {
        char *kaddr;
        page = bitsfs_get_page(inode, i, dir_has_error, &page_addr);
        if (IS_ERR(page)) {
            dir_has_error = 1;
            continue;
        }

        de = (bitsfs_dirent *)page_addr;
        kaddr = (char*)page_addr + bitsfs_last_byte(inode, i) - DENT_LEN;

        while ((char *)de <= kaddr) {
            if (de->rec_len == 0) {
                bitsfs_msg(inode->i_sb, KERN_ERR, __func__, __FILE__, __LINE__,
                        "Empty directory entry");
                goto out;
            }

            if (de->inode != 0) {
                /* check for . and .. */
                if (de->name[0] != '.')
                    goto not_empty;
                if (de->name_len > 2)
                    goto not_empty;
                if (de->name_len < 2) {
                    if (de->inode != cpu_to_le32(inode->i_ino))
                        goto not_empty;
                } else if (de->name[1] != '.')
                    goto not_empty;
            }
            de = (bitsfs_dirent *)((char*)de + DENT_LEN);
        }
        bitsfs_put_page(page, page_addr);
    }
out:
    return 1;
not_empty:
    bitsfs_put_page(page, page_addr);
    return 0;
}

static int bitsfs_readdir(struct file *file, struct dir_context *ctx)
{
    loff_t pos = ctx->pos;
    struct inode *inode = file_inode(file);
    struct super_block *sb = inode->i_sb;
    unsigned int offset = pos & ~PAGE_MASK;
    unsigned long n = pos >> PAGE_SHIFT;
    unsigned long npages = dir_pages(inode);

    bitsfs_msg(sb, KERN_ERR, __func__, __FILE__, __LINE__,
                   "bitsfs_readdir start ino=%lu", inode->i_ino);

    for ( ; n < npages; n++, offset = 0) {
        char *kaddr, *limit;
        bitsfs_dirent *de;
        struct page *page = bitsfs_get_page(inode, n, 0, (void **)&kaddr);

        if (IS_ERR(page)) {
            bitsfs_msg(sb, KERN_ERR, __func__, __FILE__, __LINE__,
                   "bitsfs_readdir bad page in #%lu", inode->i_ino);
            ctx->pos += PAGE_SIZE - offset;
            return PTR_ERR(page);
        }

        de = (bitsfs_dirent *)(kaddr+offset);
        limit = kaddr + bitsfs_last_byte(inode, n) - DENT_LEN;
        for ( ;(char*)de <= limit;) {
            if (de->rec_len == 0) {
                bitsfs_msg(sb, KERN_ERR, __func__, __FILE__, __LINE__,
                        "bitsfs_readdir reach empty dentry");
                bitsfs_put_page(page, kaddr);
                goto out;
            }
            if (de->inode) {
                bitsfs_msg(sb, KERN_INFO, __func__, __FILE__, __LINE__,
                        "bitsfs_readdir loop, inode=%lu name=%s rec_len=%d", 
                        de->inode, de->name, de->rec_len);
                
                if (!dir_emit(ctx, de->name, de->name_len, le32_to_cpu(de->inode), de->file_type)) {
                    bitsfs_put_page(page, kaddr);
                    goto out;
                }
            }
            ctx->pos += le16_to_cpu(de->rec_len);
            de = (bitsfs_dirent *)((char*)de + DENT_LEN);
        }
        bitsfs_put_page(page, kaddr);
    }
out:
    return 0;
}

const struct file_operations bitsfs_dir_operations = {
    .llseek      = generic_file_llseek,
    .read        = generic_read_dir,
    .fsync       = generic_file_fsync,
    .iterate_shared = bitsfs_readdir,
};
