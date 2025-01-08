/**
 * @file mkfs_bitsfs.h
 * @author Aaron Lau (bitsobject.com)
 * @brief This header is for mkfs_bitsfs.c
 * @version 0.1
 * @date 2024-12-06
 * 
 * @copyright Copyright (c) 2024
 * 
 */
 
/*
 * Bitsfs Magic Number
 */
#define    BITSFS_SUPER_MAGIC      0xEF99

/*
 * File system states
 */
#define    BITSFS_VALID_FS         0x0001    /* Unmounted cleanly */
#define    BITSFS_ERROR_FS         0x0002    /* Errors detected */
#define    BITSFS_CORRUPTED        177       /* Filesystem corrupted */

/*
 * Codes for operating systems
 */
#define BITSFS_OS_LINUX        0
#define BITSFS_OS_HURD         1
#define BITSFS_OS_MASIX        2
#define EBITSFS_OS_FREEBSD     3
#define BITSFS_OS_LITES        4
#define BITSFS_OS_WINDOWS      5

/*
 * Single block size in bytes
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
#define    BITSFS_BLKBMP_BLOCK 2    /* Block bitmap block number of */
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

/*
 * Bitsfs super block on the disk
 */
struct bitsfs_super_block {
    uint32_t    s_inodes_count;        /* Inodes count */
    uint32_t    s_blocks_count;        /* Blocks count */
    uint32_t    s_free_inodes_count;   /* Free inodes count */
    uint32_t    s_free_blocks_count;   /* Free blocks count */
    uint32_t    s_block_bitmap_block;  /* Blocks bitmap block */
    uint32_t    s_inode_bitmap_block;  /* Inodes bitmap block */
    uint32_t    s_inode_table_block;   /* Inodes table block */
    uint32_t    s_data_block;          /* First Data Block */
    uint32_t    s_block_size;          /* Block size */
    uint32_t    s_first_ino;           /* First inode number (default 2) */
    uint32_t    s_inode_size;          /* size of inode structure */
    uint32_t    s_mtime;               /* Mount time */
    uint32_t    s_wtime;               /* Write time */
    uint16_t    s_magic;               /* Magic number */
    uint16_t    s_state;               /* File system state */
    uint32_t    s_creator_os;          /* OS */
    char        s_name[8];             /* Fs name */
    uint32_t    s_reserved[239];       /* Padding to the end of the block 1024 bytes */
};

/*
 * Bitsfs inode on the disk
 */
struct bitsfs_inode {
    uint16_t    i_mode;           /* File mode */
    uint16_t    i_uid;            /* Low 16 bits of Owner Uid */
    uint32_t    i_size;           /* Size in bytes */
    uint32_t    i_atime;          /* Access time */
    uint32_t    i_ctime;          /* Creation time */
    uint32_t    i_mtime;          /* Modification time */
    uint32_t    i_dtime;          /* Deletion Time */
    uint16_t    i_gid;            /* Low 16 bits of Group Id */
    uint16_t    i_links_count;    /* Links count */
    uint32_t    i_blocks;         /* Blocks count */
    uint32_t    i_flags;          /* File flags */
    uint32_t    i_block[BITSFS_TMAX_BLOCKS];  /* Pointers to blocks */
    uint32_t    i_file_acl;       /* File ACL */
    uint32_t    i_dir_acl;        /* Directory ACL */
    uint32_t    i_reserved[5];    /* Padding to 128 bytes */
};

#define DENT_NAME_LEN    56

/*
 * Directory entry on disk
 */
struct bitsfs_dir_entry {
    uint32_t    inode;          /* Inode number */
    uint16_t    rec_len;        /* Fixed value: DENT_LEN */
    uint8_t     name_len;       /* Real length of name */
    uint8_t     file_type;      /* File type */
    char        name[DENT_NAME_LEN];  /* File name */
};

#define DENT_LEN sizeof(struct bitsfs_dir_entry)  // 64 bytes

/*
 * Directory entry of "/.", "/..",
 */
struct bitsfs_dir_special {
    uint32_t    inode1;          /* Inode number */
    uint16_t    rec_len1;        /* Directory entry length */
    uint8_t     name_len1;       /* Name length */
    uint8_t     file_type1;      /* File type */
    char        name1[DENT_NAME_LEN];        /* File name, */
    uint32_t    inode2;          /* Inode number */
    uint16_t    rec_len2;        /* Directory entry length */
    uint8_t     name_len2;       /* Name length */
    uint8_t     file_type2;      /* File type */
    char        name2[DENT_NAME_LEN];        /* File name */
};