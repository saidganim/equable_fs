#ifndef EQBL_FAT_FS
#define EQBL_FAT_FS






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

#endif
