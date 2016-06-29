#include "eqbl_fat.h"
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/stat.h>
#include <linux/random.h>
/*
 * This file contains operations implementation
 */



// INODE_OPERATIONS


/**
 * This function creates inode , sets pointer to inode_operations struct,
 * Monitors is inode creates for directory or for regular file.
 *  
 */
int create_efat_inode( struct inode *dir, struct dentry *dentry, umode_t mode, bool excl){
    char flag;
    struct inode *inode;
    struct efat_inode *efat_inode;
    struct eqbl_fat_super_block *efat_sb;
    struct super_block *sb;
     
    sb = dir->i_sb;
   
    efat_sb = (struct eqbl_fat_super_block*)sb->s_fs_info;

    if(!mutex_lock_interruptible(&efat_inode_mutex))
        return -EINTR;

    if(!S_ISDIR(mode) && !S_ISREG(mode)){
        printk(" Trying to create unsupported type of file(inode) \n");
	mutex_unlock(&efat_inode_mutex);
	return -EIO;
    };
    if(S_ISDIR(mode))
        flag = 0b01000000;
    else
        flag = 0b00000000;

    inode = new_inode(sb);

    if(!inode){
      mutex_unlock(&efat_inode_mutex);
      printk( KERN_ALERT "Cannot allocate new inode\n");
      return -ENOMEM;
    };
    
    inode->i_sb =  sb;
    inode->i_op = dir->i_op;
    inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
    inode->i_ino = INODE_NUMBER;
    inode->i_mode = 0766;
    //inode->i_uid = inode->i_gid = 0;
    //inode->i_blksize = CLUSTER_SIZE;
    inode->i_blocks = 0;
    INODE_NUMBER += 1;
    efat_inode = kmem_cache_alloc(efat_inode_cachep, GFP_KERNEL);
    efat_inode->file_flags = flag;
    inode->i_private = efat_inode;
    if(S_ISDIR(mode)){
        inode->i_fop = dir->i_fop;
    }

    else
        inode->i_fop = &eqbl_file_operations;



    
    
    
    mutex_unlock(&efat_inode_mutex);
    return 0;



};

/*
unsigned int get_free_block(){
  // Returning first free block
  struct free_block* fr_block, q, tmp;
  unsigned int res = -1;
  list_for_each_safe(fr_block, q, &free_block_list_head->list)
    if(fr_block->number != -1){
      tmp = &list_entry(fr_block, struct free_block, list );
      list_del(fr_block);
      res = fr_block->number;
      kfree((void*)tmp);
      break;
    }
    return res;
      
};
*/




// SUPER_BLOCK

void eqbl_fat_put_super(struct super_block *sb){ 		// Function which is called during umounting fs
    printk( KERN_ALERT "EQBL_FAT_FS_ERR:  equable filesystem is umounted\n");
};

void destroy_efat_inode(struct inode *inode){
    struct efat_inode* efat_inode;
    efat_inode = EFAT_INODE(inode);
    kmem_cache_free(efat_inode_cachep, efat_inode);
};


