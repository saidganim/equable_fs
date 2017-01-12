#ifndef EQBL_FAT_FS
#define EQBL_FAT_FS


#include <linux/list.h>
#include <linux/fs.h>



#define EFAT_SUPER_BLOCK(sb) sb->s_fs_info

#define EFAT_INODE(inode) (struct efat_inode*)inode->i_private

// I'm not still sure , that this number is not taken...
#define EQBL_FAT_MAGIC_NUMBER 0xAAFF8021

// Name of filesystem in mount table
#define EQBL_FAT_NAME "eqbl_fat"

// Number of block where super_block is located
#define EQBL_FAT_SUPER_BLOCK_OFFSET 0

// Number of cluster where file allocation tables are located
#define EQBL_FAT_ARRAY_OFFSET 1

// Count of file allocation tables on disk
#define EQBL_FAT_ARRAY_SIZE 64

// Max size of file ( for beginning 4 GB )
#define MAX_FILE_SIZE 4294967296

// Root inode number
#define ROOT_INODE_NUMBER 1

// Size of cluster
#define CLUSTER_SIZE (4*1024)

// Size of one file allocation table
#define FAT_SIZE 1024

#define EFS_LOG(msg) printk( KERN_ALERT msg)

#define EFAT_DEFAULT_MODE 0755


// For getting the number of cluster and number of offset in the cluster
#define GET_CLUSTER_NO(NO) NO / CLUSTER_SIZE
#define GET_CLUSTER_OFF(NO) NO - GET_CLUSTER_NO(NO) * CLUSTER_SIZE  

#define EFAT_INODESTORE_CLUSTER_NUMBER 65
#define EFAT_INODESTORE_CLUSTER_COUNT 4

#define KERN_LOG(str) printk( KERN_ALERT str)

struct eqbl_file_alloc_table{
    unsigned int data[FAT_SIZE - sizeof( char )];
    char flag;
};

struct free_block{
  struct list_head list;
  unsigned int number;
};

struct efat_inode { // exact size of strut is 64 bytes;
    uint64_t first_cluster; // position in fat
    uint64_t i_ino;
    loff_t size;
    char file_flags; // flags for file [<deleted>,<directory>,<>,<>,<>,<>,<>,<>]
    char name[64 - sizeof(char) - sizeof(uint64_t) - sizeof(uint64_t) - sizeof(loff_t)];
};

struct efat_file_record{
    char data[64];
};

struct __eqbl_fat_super_block{
    uint64_t magic;
};

struct eqbl_fat_super_block{
    struct eqbl_file_alloc_table fat[EQBL_FAT_ARRAY_SIZE];
    struct __eqbl_fat_super_block* __efat_sb;
};


unsigned int get_free_block( void );

// Low-latency bdev read/write functions  
inline int efat_read_cluster(struct super_block *sb, char* buffer, unsigned int number);
inline int efat_write_cluster(struct super_block *sb, char* buffer, unsigned int number);












#endif
