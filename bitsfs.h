#include <linux/fs.h>
#include <linux/percpu_counter.h>
#include <linux/rbtree.h>
#include <linux/mm.h>
#include <linux/highmem.h>

/*
 * Bitsfs Magic Number
 */
#define    BITSFS_SUPER_MAGIC      0xEF99

/*
 * File system states
 */
#define    BITSFS_VALID_FS         0x0001    /* Unmounted cleanly */
#define    BITSFS_ERROR_FS         0x0002    /* Errors detected */
#define    BITSFS_CORRUPTED        EUCLEAN   /* Filesystem corrupted */

/*
 * Inode dynamic state flags
 */
#define    BITSFS_STATE_NEW        0x00000001 /* inode is newly created */

/*
 * Codes for operating systems
 */
#define    BITSFS_OS_LINUX         0
#define    BITSFS_OS_HURD          1
#define    BITSFS_OS_MASIX         2
#define    EBITSFS_OS_FREEBSD      3
#define    BITSFS_OS_LITES         4
#define    BITSFS_OS_WINDOWS       5

/*
 * Single block size
 */
#define    BITSFS_BLOCK_SIZE       4096

/*
 * Indirect block array length
 */
#define    BITSFS_DDIR_BLOCKS      12
#define    BITSFS_NDIR_BLOCKS      4
#define    BITSFS_TMAX_BLOCKS      (BITSFS_DDIR_BLOCKS + BITSFS_NDIR_BLOCKS)
#define    BITSFS_NDIR_BLOCK_COUNT 1024

/*
 * Block layout
 */
#define    BITSFS_DBOOT_BLOCK      0    /* Dev boot block number */
#define    BITSFS_SUPER_BLOCK      1    /* Super block number */
#define    BITSFS_BLKBMP_BLOCK     2    /* Block bitmap block number */
#define    BITSFS_BLKBMP_BLOCKS    4    /* Block bitmap block count */
#define    BITSFS_INDBMP_BLOCK     6    /* Inode bitmap block number */
#define    BITSFS_INDTBL_BLOCK     7    /* Inode table block start number */
#define    BITSFS_INDTBL_BLOCKS    128  /* Inode table blocks count */
#define    BITSFS_DATA_BLOCK       135  /* Data block start number */

/*
 * Special inode numbers
 */
#define    EBITSFS_BAD_INO         1    /* Bad blocks inode */
#define    BITSFS_ROOT_INO         2    /* Root inode */

/*
 * Dir file types
 */
#define    BITSFS_FT_UNKNOWN       0
#define    BITSFS_FT_REG_FILE      1
#define    BITSFS_FT_DIR           2

#define    BITSFS_B2BI(sb)         (sb->s_fs_info)

/*
 * Dir entry limits
 *
 * NOTE: It must be a multiple of 4
 */
#define BITSFS_DIR_PAD              4
#define BITSFS_DIR_ROUND            (BITSFS_DIR_PAD - 1)
#define BITSFS_DIR_REC_LEN(nlen)    (((nlen) + 8 + BITSFS_DIR_ROUND) & ~BITSFS_DIR_ROUND)
#define BITSFS_MAX_REC_LEN         ((1<<16)-1)  /* max 255 char */

/*
 * Bitsfs super block in memory
 */
struct bitsfs_sb_info {
    unsigned long s_inodes_count;               /* Inodes count */
    unsigned long s_blocks_count;               /* Blocks count */
    unsigned long s_overhead_last;              /* Last calculated overhead */
    unsigned long s_blocks_last;                /* Last seen block count */
    struct buffer_head *s_sbh;                  /* Buffer containing the super block */
    struct bitsfs_super_block *s_bs;            /* Pointer to the super block in the buffer */
    unsigned long s_mount_opt;                  /* Mount options */
    unsigned long s_sb_block;                   /* Super block position from mount option sb=xx default 1*/
    unsigned short s_mount_state;               /* File system state. Ref to i_state */
    unsigned short s_pad;
    int s_inode_size;                           /* Inode size */
    int s_first_ino;                            /* The first inode number */
    struct percpu_counter s_freeblocks_counter;
    struct percpu_counter s_freeinodes_counter;
    struct percpu_counter s_dirs_counter;
    /*
     * s_lock protects against concurrent modifications of s_mount_state,
     * s_blocks_last, s_overhead_last and the content of superblock's
     * buffer pointed to by sbi->s_es.
     */
    spinlock_t s_lock;
    struct dax_device *s_daxdev;                 /* Direct Access device */
};

/*
 * Bitsfs inode in memory
 */
struct bitsfs_inode_info {
    __le32   i_data[BITSFS_TMAX_BLOCKS]; /* Refer to the i_block of the disk inode */
    __u32    i_flags;
    __u16    i_state;
    __u32    i_file_acl;
    __u32    i_dir_acl;
    __u32    i_dtime;
    __u32    i_dir_start_lookup;
    struct inode    vfs_inode;
};

/*
 * Bitsfs super block on the disk
 */
struct bitsfs_super_block {
    __le32    s_inodes_count;        /* Inodes count */
    __le32    s_blocks_count;        /* Blocks count */
    __le32    s_free_inodes_count;   /* Free inodes count */
    __le32    s_free_blocks_count;   /* Free blocks count */
    __le32    s_block_bitmap_block;  /* Blocks bitmap block */
    __le32    s_inode_bitmap_block;  /* Inodes bitmap block */
    __le32    s_inode_table_block;   /* Inodes table block */
    __le32    s_data_block;          /* First Data Block */
    __le32    s_block_size;          /* Block size */
    __le32    s_first_ino;           /* First inode number (default 2) */
    __le32    s_inode_size;          /* Size of inode structure */
    __le32    s_mtime;               /* Mount time */
    __le32    s_wtime;               /* Write time */
    __le16    s_magic;               /* Magic number */
    __le16    s_state;               /* File system state */
    __le32    s_creator_os;          /* OS */
    char      s_name[8];             /* FS name */
    __u32     s_reserved[239];       /* Padding to the end of the block */
};

/*
 * Bitsfs inode on the disk
 */
struct bitsfs_inode {
    __le16    i_mode;           /* File mode */
    __le16    i_uid;            /* Low 16 bits of Owner Uid */
    __le32    i_size;           /* Size in bytes */
    __le32    i_atime;          /* Access time */
    __le32    i_ctime;          /* Creation time */
    __le32    i_mtime;          /* Modification time */
    __le32    i_dtime;          /* Deletion Time */
    __le16    i_gid;            /* Low 16 bits of Group Id */
    __le16    i_links_count;    /* Links count */
    __le32    i_blocks;         /* Blocks count */
    __le32    i_flags;          /* File flags */
    __le32    i_block[BITSFS_TMAX_BLOCKS];  /* Pointers to blocks */
    __le32    i_file_acl;       /* File ACL */
    __le32    i_dir_acl;        /* Directory ACL */
    __u32     i_reserved[5];    /* Padding to 128 bytes */
};

#define DENT_NAME_LEN    56

/*
 * Directory entry on disk
 */
struct bitsfs_dir_entry {
    __le32    inode;          /* Inode number */
    __le16    rec_len;        /* Fixed value: DENT_LEN */
    __u8      name_len;       /* Real length of name */
    __u8      file_type;      /* File type */
    char      name[DENT_NAME_LEN];  /* File name */
};

#define DENT_LEN sizeof(struct bitsfs_dir_entry)  // 64 bytes

static void bitsfs_msg(struct super_block *sb, const char *prefix, const char *func, 
        const char *file, int line, const char *fmt, ...)
{
    struct va_format vaf;
    va_list args;
    va_start(args, fmt);
    vaf.fmt = fmt;
    vaf.va = &args;
    printk("%sBitsFS-%s: %pV -at %s() of %s(%d)\n", prefix, sb->s_id, &vaf, func, file, line);
    va_end(args);
}

static inline struct bitsfs_sb_info *BITFS_S2SI(struct super_block *sb)
{
    return sb->s_fs_info;
}

static inline struct bitsfs_inode_info *BITSFS_I2BI(struct inode *inode)
{
    return container_of(inode, struct bitsfs_inode_info, vfs_inode);
}

/*
 * Atomic bitops
 */
#define bitsfs_set_bit    test_and_set_bit_le
#define bitsfs_clear_bit  test_and_clear_bit_le
#define bitsfs_find_first_zero_bit    find_first_zero_bit_le
#define bitsfs_find_next_zero_bit     find_next_zero_bit_le
#define bitsfs_find_next_bit    find_next_bit_le

static inline void bitsfs_put_page(struct page *page, void *page_addr)
{
    kunmap_local(page_addr);
    put_page(page);
}

/* dentry.c */
extern int bitsfs_add_link(struct dentry *, struct inode *);
extern int bitsfs_get_ino_by_name(struct inode *dir,
                  const struct qstr *child, ino_t *ino);
extern int bitsfs_make_empty(struct inode *, struct inode *);
extern struct bitsfs_dir_entry *bitsfs_find_entry(struct inode *, const struct qstr *,
                        struct page **, void **res_page_addr);
extern int bitsfs_delete_entry(struct inode *dir, struct bitsfs_dir_entry *den, struct page *page,
                 char *kaddr);
extern int bitsfs_empty_dir(struct inode *);
extern struct bitsfs_dir_entry *bitsfs_dotdot(struct inode *dir, struct page **p, void **pa);
extern void bitsfs_set_link(struct inode *, struct bitsfs_dir_entry *, struct page *, void *,
              struct inode *, int);
static inline void bitsfs_put_page(struct page *page, void *page_addr);
extern const struct file_operations bitsfs_dir_operations;

/* inode.c */
extern void set_root_inode_bitmap(struct inode *, int) ;
extern struct inode *bitsfs_iget(struct super_block *, unsigned long);
extern struct inode *bitsfs_new_inode (struct inode *, umode_t, const struct qstr *);
extern int bitsfs_write_inode (struct inode *, struct writeback_control *);
extern void bitsfs_evict_inode(struct inode *);
extern const struct inode_operations bitsfs_file_inode_operations;
extern const struct file_operations bitsfs_file_operations;

/* block.c */
extern void set_root_block_bitmap(struct inode *, int) ;
extern int bitsfs_get_block(struct inode *, sector_t, struct buffer_head *, int);
extern void bitsfs_truncate_blocks(struct inode *, loff_t);
extern void bitsfs_set_file_ops(struct inode *inode);
extern void bitsfs_set_dir_ops(struct inode *inode);
extern const struct address_space_operations bitsfs_aops;
extern const struct address_space_operations bitsfs_dax_aops;

/* namei.c */
extern const struct inode_operations bitsfs_dir_inode_operations;