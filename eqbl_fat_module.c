/*
 * This module has been written by Saidgani Musaev in MSU( Moscow state univerisy) as a graduate work.
 * This filesystem is based on fat32 principles, but this is a lot simpler.
 * The main idea of this project is using block device evenly for making data storage more sustainable.
 */

#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/time.h>
#include <linux/blkdev.h>
#include <linux/stat.h>
#include <linux/dcache.h>
#include <linux/buffer_head.h>
#include <linux/parser.h>
#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include "eqbl_fat.h"

MODULE_AUTHOR("Saidgani Musaev <cpu808694@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Equable fs, based on fat32");

static struct kmem_cache *efat_inode_cachep; // TODO: the kmem_cache is unused. Need to Fix it (Kernel cache for inodes) 

short int RECORD_COUNT = 0;
unsigned int VALID_FAT[2];

DEFINE_MUTEX(efat_sb_mutex);
DEFINE_MUTEX(efat_directory_children_mutex);
DEFINE_MUTEX(efat_inode_mutex);

uint64_t INODE_NUMBER = ROOT_INODE_NUMBER + 1;

struct free_block* free_block_list_head;

static unsigned int efat_inode_count = 0;

//BDEV_OPERATIONS
extern unsigned int get_free_block( void );
extern int efat_read_cluster(struct super_block *sb, char* buffer, unsigned int number);
extern int efat_write_cluster(struct super_block *sb, char* buffer, unsigned int number);

// DIR_OPERATIONS
int efat_dir_open(struct inode *inode, struct file *file);
int efat_dir_close(struct inode *inode, struct file *file);
int efat_readdir(struct file *file, struct dir_context *ctx);

// INODE_OPERATIONS
int create_efat_inode( struct inode*, struct dentry*, umode_t mode, bool excl);
struct dentry* efat_lookup(struct inode*, struct dentry*, unsigned int flags);
struct inode* eqbl_fat_get_inode(struct super_block* sb, const struct  inode* dir, struct dentry* dentry, umode_t mode);

// SUPER_OPERATIONS
void eqbl_fat_put_super(struct super_block *sb);
void destroy_efat_inode(struct inode *inode);

// FILE OPERATIONS
ssize_t efat_read(struct file * filp, char __user * buf, size_t len,
              loff_t * ppos);
ssize_t efat_write(struct file * filp, const char __user * buf, size_t len,
               loff_t * ppos);

int efat_mknod( struct inode* dir, struct dentry* dentry, umode_t, dev_t dev);

int efat_create( struct inode *dir, struct dentry *dentry, umode_t mode, bool excl);
int efat_mkdir(struct inode * dir, struct dentry * dentry, umode_t mode);
struct efat_inode* efat_get_same_inode(struct super_block* , struct inode*);

const struct inode_operations eqbl_fat_inode_operations;
const struct file_operations eqbl_dir_operations;
const struct file_operations eqbl_file_operations;

int sync_efat_inode(struct super_block* sb, struct efat_inode* efat_sync){
    char* buff;
    unsigned int i, j;
    struct efat_inode* efat_i;
    buff = (char*) kmalloc(CLUSTER_SIZE, GFP_KERNEL);

    //reading_root:
    for(i = EFAT_INODESTORE_CLUSTER_NUMBER ; i < EFAT_INODESTORE_CLUSTER_NUMBER +  EFAT_INODESTORE_CLUSTER_COUNT; ++i){
        efat_read_cluster(sb, buff, i);
        efat_i = (struct efat_inode*) buff;
        for(j = 0; j < CLUSTER_SIZE / sizeof(struct efat_inode); ++j){
            if(efat_sync->first_cluster == efat_i->first_cluster) { // Found the inode record
                memcpy(efat_i, efat_sync, sizeof(struct efat_inode));
                efat_write_cluster(sb, buff, i);
                kfree( buff );
                return 0;
            }
            ++efat_i;
        }
    }
    kfree(buff);
    return -ENOENT;
}

int sync_fat(struct super_block *sb){
    char* buff;
    int res = 0;
    unsigned int i;
    struct eqbl_fat_super_block* efat_sb;
    efat_sb = (struct eqbl_fat_super_block*)sb->s_fs_info;

    buff = (char*) kmalloc(CLUSTER_SIZE, GFP_KERNEL);
    for( i = EQBL_FAT_ARRAY_OFFSET; i <= EQBL_FAT_ARRAY_SIZE  * sizeof(struct eqbl_file_alloc_table) / CLUSTER_SIZE; i += 1 ){
     // Reading clusters step by step and putting data into array of fat structures  
        memcpy(buff, &efat_sb->fat[i - EQBL_FAT_ARRAY_OFFSET], CLUSTER_SIZE);
        if(unlikely(0 != efat_write_cluster(sb, buff , i))){
           res = -EBUSY;
	   goto sync_fat_release;
	}
    }
sync_fat_release:
    kfree(buff);
    return res;
}

int efat_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry)
{
	//TODO: For beginning steps my fs doesn't support the links
	struct inode *inode = d_inode(old_dentry);
    inode->i_ctime = dir->i_ctime = dir->i_mtime = CURRENT_TIME;
    inc_nlink(inode);
    ihold(inode);
    dget(dentry);
    d_instantiate(dentry, inode);
	return 0;
}

struct dentry* efat_lookup(struct inode* parent, struct dentry* child, unsigned int flags){
    struct inode *inode;
    char *buff;
    struct efat_inode *efat_i;
    int i, j;
    buff = (char*)kmalloc(CLUSTER_SIZE, GFP_KERNEL);
    for(i = EFAT_INODESTORE_CLUSTER_NUMBER ; i < EFAT_INODESTORE_CLUSTER_NUMBER +  EFAT_INODESTORE_CLUSTER_COUNT; ++i){
        efat_read_cluster(parent->i_sb, buff, i);
        efat_i = (struct efat_inode*) buff;
        for(j = 0; j < CLUSTER_SIZE / sizeof(struct efat_inode); ++j){
            if(!strcmp(efat_i->name, child->d_name.name) ){
                inode =  new_inode(parent->i_sb);
                inode->i_ino = get_next_ino();
                inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
                inode->i_op = &eqbl_fat_inode_operations;
                if(efat_i->file_flags && 0b01000000){
                    inode_init_owner(inode, parent, S_IFDIR);
                    inode->i_fop = &eqbl_dir_operations;
                }
                else{
                    inode_init_owner(inode, parent, S_IFREG);
                    inode->i_fop = &eqbl_file_operations;
                 }
                inode->i_private = (struct efat_inode*) kmem_cache_alloc(efat_inode_cachep, GFP_KERNEL);
                memcpy(inode->i_private, efat_i, sizeof(struct efat_inode));
                d_add(child, inode);
                goto release;
            }
            if(efat_i->first_cluster == 0)
                break;
            ++efat_i;
        }
    }
    d_add(child, NULL);
    kfree(buff);
    return NULL;
release:
    kfree(buff);
    return NULL;
};


int efat_unlink(struct inode *dir, struct dentry *dentry)
{

	//TODO: Realizations just in memory. Immediately fix it!
	struct inode *inode = d_inode(dentry);
	 
	inode->i_ctime = dir->i_ctime = dir->i_mtime = CURRENT_TIME; 
	drop_nlink(inode);
	dput(dentry);
	return 0;
}

int efat_rmdir(struct inode *dir, struct dentry *dentry){
	if (!simple_empty(dentry))
	   return -ENOTEMPTY;
	 
	drop_nlink(d_inode(dentry));
	efat_unlink(dir, dentry);
	drop_nlink(dir);
	return 0;
}


int efat_rename(struct inode *old_dir, struct dentry *old_dentry,
struct inode *new_dir, struct dentry *new_dentry){
	struct inode *inode = d_inode(old_dentry);
	int they_are_dirs = d_is_dir(old_dentry);
	if (!simple_empty(new_dentry))
	   return -ENOTEMPTY;

	if (d_really_is_positive(new_dentry)) {
    	efat_unlink(new_dir, new_dentry);
    	if (they_are_dirs) {
        	drop_nlink(d_inode(new_dentry));
        	drop_nlink(old_dir);
    	}
	} else if (they_are_dirs) {
	   drop_nlink(old_dir);
	   inc_nlink(new_dir);
	}
	 
	old_dir->i_ctime = old_dir->i_mtime = new_dir->i_ctime =
	new_dir->i_mtime = inode->i_ctime = CURRENT_TIME;
	return 0;
}

ssize_t efat_read(struct file * filp, char __user * buf, size_t len,
              loff_t * ppos){
    struct efat_inode *inode =(struct efat_inode*) filp->f_inode->i_private;
    char* buffer;
    size_t begin_len;
    char __user * ptr;
    unsigned int i, first_cluster, cluster_no, cluster_off;
    loff_t shift;
    struct eqbl_fat_super_block* efat_sb;
    efat_sb = (struct eqbl_fat_super_block*)filp->f_inode->i_sb->s_fs_info;
   
    if( (*ppos) > filp->f_inode->i_size)
        return 0;
    len = min( len , filp->f_inode->i_size - *ppos);
    begin_len = len;
    buffer = (char*) kmalloc(CLUSTER_SIZE, GFP_KERNEL);
    first_cluster = *ppos / CLUSTER_SIZE;
    cluster_off = *ppos - CLUSTER_SIZE * first_cluster;
    cluster_no = inode->first_cluster;
    for(i = 0 ; i < first_cluster; ++i){
        cluster_no = efat_sb->fat[0].data[cluster_no];
        if(efat_sb->fat[0].data[cluster_no] ==  -1 && i < first_cluster - 1){
            kfree(buffer);
            return 0;
        }
    }
    
    ptr = buf;
    while( len > 0 ){
        efat_read_cluster( filp->f_path.dentry->d_sb, buffer, cluster_no);
        shift = min(CLUSTER_SIZE - cluster_off, len);
        copy_to_user(ptr, buffer + cluster_off, shift);
        ptr += shift;
        len -= shift;
        cluster_no = efat_sb->fat[0].data[cluster_no];
        cluster_off = 0;
    }
    *ppos += begin_len - len;
    kfree(buffer);
    return begin_len - len;
};

ssize_t efat_write(struct file * filp, const char __user * buf, size_t len,
               loff_t * ppos){
    struct efat_inode *efat_i =(struct efat_inode*) filp->f_inode->i_private;
    char* buffer;
    char __user * ptr;
    unsigned int i;
    size_t begin_len;
    unsigned int first_cluster, cluster_no, cluster_off;
    loff_t shift;
    struct eqbl_fat_super_block* efat_sb;
  
    efat_sb = (struct eqbl_fat_super_block*)filp->f_inode->i_sb->s_fs_info;
    begin_len = len; 
    buffer = (char*) kmalloc(CLUSTER_SIZE, GFP_KERNEL);
    first_cluster = *ppos / CLUSTER_SIZE;
    cluster_off = *ppos - CLUSTER_SIZE * first_cluster;
    //read_time = len / CLUSTER_SIZE + 1;
    cluster_no = efat_i->first_cluster;
    for(i = 0 ; i < first_cluster; ++i){
        cluster_no = efat_sb->fat[0].data[cluster_no];
        if(efat_sb->fat[0].data[cluster_no] ==  -1){
            efat_sb->fat[0].data[cluster_no] =  get_free_block();
            efat_sb->fat[0].data[efat_sb->fat[0].data[cluster_no]] = -1;
            break;
        }
    }
    ptr = buf;
    while( len > 0 ){
        if(efat_sb->fat[0].data[cluster_no] ==  -1){
            efat_sb->fat[0].data[cluster_no] =  get_free_block();
            efat_sb->fat[0].data[efat_sb->fat[0].data[cluster_no]] = -1;
        }
        shift = min(CLUSTER_SIZE - cluster_off, len);
        efat_read_cluster( filp->f_path.dentry->d_sb, buffer, cluster_no);
        copy_from_user(buffer + cluster_off, ptr , shift);
        efat_write_cluster( filp->f_path.dentry->d_sb, buffer, cluster_no);
        ptr += shift;
        len -= shift;
        cluster_off = 0;
        cluster_no = efat_sb->fat[0].data[cluster_no];

    }
    efat_i->size += begin_len - len;
    sync_efat_inode(filp->f_inode->i_sb, efat_i);
    sync_fat(filp->f_path.dentry->d_sb);
    filp->f_inode->i_size += begin_len - len; 
    *ppos += begin_len - len;
    kfree(buffer);
    return begin_len - len;
};

int efat_dir_open(struct inode *inode, struct file *file){
    static struct qstr cursor_name = QSTR_INIT(".", 1);
    struct efat_inode* efat_i;
    file->private_data = d_alloc(file->f_path.dentry, &cursor_name);
    efat_i = (struct efat_inode*) inode->i_private;
    file->f_inode->i_size = efat_i->size;
    return file->private_data ? 0 : -ENOMEM;
}

int efat_dir_close(struct inode *inode, struct file *file){
    dput(file->private_data);
    return 0;
}

int efat_readdir(struct file *filp, struct dir_context *ctx){
    loff_t pos;
    struct inode *inode;
    struct super_block *sb;
    char* buff;
    struct efat_inode *efat_i, *tmp_i;
    unsigned int i, record_no;
    struct eqbl_fat_super_block* efat_sb;
    efat_sb = (struct eqbl_fat_super_block*)filp->f_inode->i_sb->s_fs_info;    
    pos = ctx->pos;
    if (pos) {
        /* FIXME: We use a hack of reading pos to figure if we have filled in all data.
         * We should probably fix this to work in a cursor based model and
         * use the tokens correctly to not fill too many data in each cursor based call */
        return 0;
    }
    buff = (char*) kmalloc(CLUSTER_SIZE, GFP_KERNEL);
    inode = filp->f_inode;
    sb = inode->i_sb;
    efat_i = (struct efat_inode*)inode->i_private;
    if(unlikely(! efat_i->file_flags && 0b01000000))
        return -ENOTDIR;
    record_no = efat_i->first_cluster;
    do {
        if(record_no == -1) break;
        efat_read_cluster(sb, buff, EFAT_INODESTORE_CLUSTER_NUMBER + record_no * sizeof(struct efat_inode) / CLUSTER_SIZE);
        tmp_i = (struct efat_inode*) (buff + record_no * sizeof(struct efat_inode) % CLUSTER_SIZE);
        dir_emit(ctx, tmp_i->name, strlen(tmp_i->name) , inode->i_ino, DT_UNKNOWN);
        ctx->pos += sizeof(struct efat_inode);
        pos += sizeof(struct efat_inode);
        record_no = efat_sb->fat[0].data[record_no];
    } while(efat_sb->fat[0].data[record_no] != 0 && efat_sb->fat[0].data[record_no] != -1);

    kfree(buff);
    return 0;
}

const struct file_operations eqbl_dir_operations ={
    .iterate        = efat_readdir,
};

const struct file_operations eqbl_file_operations = {
     .read        = efat_read,
     .write      = efat_write,
     .open       = efat_dir_open,
};

const struct inode_operations eqbl_fat_inode_operations = {
    .create      = efat_create,
    .lookup     = efat_lookup,
    .link       = efat_link,
    .unlink     = efat_unlink,
    .symlink    = 0,
    .mkdir      = efat_mkdir,
    .rmdir      = efat_rmdir,
    .mknod      = efat_mknod,
    .rename     = efat_rename,
};

const struct super_operations eqbl_fat_super_operations = {
    .statfs      = simple_statfs,
    .drop_inode = generic_delete_inode,
    .show_options   = generic_show_options,
    .destroy_inode = destroy_efat_inode,
    .put_super = eqbl_fat_put_super
};


// Getting free block in FAT
inline unsigned int get_free_block( void ){
	struct free_block* free_b;
	unsigned int res;
    if(list_empty(&free_block_list_head->list))
        return -1; // there is no any free block
	free_b = list_entry(free_block_list_head->list.next, struct free_block, list );
    list_del_init(free_block_list_head->list.next);
	//free_block_list_head = free_b->list.next;
    res = free_b->number;
	kfree(free_b);
	return res;
};

// INODE_OPERATIONS
/**
 * This function creates inode , sets pointer to inode_operations struct,
 * Monitors is inode creates for directory or for regular file.
 *  
 */
int efat_mknod( struct inode* dir, struct dentry *dentry, umode_t umode, dev_t dev){
    char *buffer;
    unsigned int cluster_no, cluster_off, i, last_cluster;
    int err = -ENOSPC;
    struct efat_inode *efat_i, *stored_efat_i;
    struct inode *inode = eqbl_fat_get_inode(dir->i_sb, dir, dentry, umode);
    struct eqbl_fat_super_block* efat_sb;
    efat_sb = (struct eqbl_fat_super_block*)dir->i_sb->s_fs_info;
    buffer = (char*) kmalloc(CLUSTER_SIZE, GFP_KERNEL);
    efat_i = inode->i_private;
    cluster_no = efat_inode_count *  sizeof(struct efat_inode) / CLUSTER_SIZE;
    cluster_off = efat_inode_count *  sizeof(struct efat_inode)  % CLUSTER_SIZE;
    mutex_lock(&efat_inode_mutex);
    efat_read_cluster(inode->i_sb, buffer, EFAT_INODESTORE_CLUSTER_NUMBER + cluster_no);
    stored_efat_i = (struct efat_inode*) buffer;
    stored_efat_i += cluster_off / sizeof(struct efat_inode);

    if(((umode) & S_IFMT) == S_IFREG){
        efat_i->first_cluster = get_free_block();
    } else
    {
        efat_i->first_cluster = efat_inode_count + 2;
    }

    efat_i->size = 0;
    efat_sb->fat[0].data[efat_i->first_cluster] = -1;
    memcpy(efat_i->name + 0, dentry->d_name.name, min(strlen(dentry->d_name.name), 64 - sizeof(char) - sizeof(uint64_t) - sizeof(uint64_t) - sizeof(loff_t)));
    memcpy(stored_efat_i, efat_i, sizeof(struct efat_inode));
    efat_write_cluster(inode->i_sb, buffer, EFAT_INODESTORE_CLUSTER_NUMBER + cluster_no);
    i = ((struct efat_inode*)dir->i_private)->first_cluster;
    last_cluster = i;
    while(true){
        i = efat_sb->fat[0].data[i];
        if( i != -1 && i != 0) 
            last_cluster = i;
        if(i == -1 || i == 0)
            break;
    }    
    efat_sb->fat[0].data[last_cluster] = efat_inode_count; 
    efat_sb->fat[0].data[efat_sb->fat[0].data[last_cluster]] = -1;
    efat_sb->fat[0].data[efat_inode_count + 1] = -1;
    sync_fat(inode->i_sb);
    mutex_unlock(&efat_inode_mutex);
    if( inode ){
        d_instantiate(dentry, inode);
        dget(dentry);
        err = 0;
        ++efat_inode_count;
        dir->i_mtime = dir->i_ctime = CURRENT_TIME;
    }
    kfree(buffer);
    return err;
} 

int efat_create( struct inode *dir, struct dentry *dentry, umode_t mode, bool excl){
    return efat_mknod(dir, dentry, mode | S_IFREG, 0);    
};

int efat_mkdir(struct inode * dir, struct dentry * dentry, umode_t mode){
    int retval = efat_mknod(dir, dentry, mode | S_IFDIR, 0);
    if (!retval)
        inc_nlink(dir);
    return retval;
}

// SUPER_BLOCK
void eqbl_fat_put_super(struct super_block *sb){ 		// Function which is called during umounting fs
    struct free_block* tmp;
    struct list_head *q, *pos;
    //Removing list of free blocks from memory
    list_for_each_safe(pos, q, &free_block_list_head->list){
         tmp= list_entry(pos, struct free_block, list);
         list_del(pos);
         kfree(tmp);
    }
};

void destroy_efat_inode(struct inode *inode){
    struct efat_inode* efat_inode;
    efat_inode = EFAT_INODE(inode);
    kmem_cache_free(efat_inode_cachep, efat_inode);
};

enum {
    Opt_mode,
    Opt_err
};

static const match_table_t tokens = {
    {Opt_mode, "mode=%o"},
    {Opt_err, NULL}
};

static umode_t efat_parse_options(char *data){
    substring_t args[MAX_OPT_ARGS];
    int option;
    int token;
    char *p;
    umode_t umode;
    umode = EFAT_DEFAULT_MODE;
    while ((p = strsep(&data, ",")) != NULL) {
        if (!*p)
            continue;
        token = match_token(p, tokens, args);
        switch (token) {
        case Opt_mode:
            if (match_octal(&args[0], &option))
                return NULL;
            umode = option & S_IALLUGO;
            break;
        }
    }
    return umode;
}

struct efat_inode* eqb_fat_read_inode(struct super_block* sb, unsigned int i_ino){
    struct efat_inode* e_inode;
    char buffer*;
    int i, j;
    struct efat_inode* res = (struct efat_inode*) kmem_cache_alloc(efat_inode_cachep, GFP_KERNEL);
    buffer = (char*) kmalloc(EFAT_INODESTORE_CLUSTER_COUNT * CLUSTER_SIZE / sizeof(struct efat_inode), GFP_KERNEL);
    mutex_lock_interruptible(&efat_inode_mutex);
    for( i = 0 ; i < EFAT_INODESTORE_CLUSTER_COUNT; ++i){
        efat_read_cluster(sb, buffer, EFAT_INODESTORE_CLUSTER_NUMBER);
        e_inode = (struct efat_inode*)buffer;
        for( j = 0 ; j < CLUSTER_SIZE / sizeof(struct efat_inode); ++j){
            if(e_inode->i_ino == i_ino){
                memcpy(res, e_inode, sizeof(struct efat_inode));
                kfree(buffer);
                mutex_unlock(&efat_inode_mutex);
                return res;
            }
            e_inode++;
        }
        kfree(buffer);
    }
    mutex_unlock(&efat_inode_mutex);
    return NULL;
}

struct efat_inode* efat_get_same_inode(struct super_block* sb, struct inode* inode){
    struct efat_inode *original, *e_inode;
    char buffer*;
    int i, j;
    struct efat_inode* res = (struct efat_inode*) kmem_cache_alloc(efat_inode_cachep, GFP_KERNEL);
    buffer = kmalloc(EFAT_INODESTORE_CLUSTER_COUNT * CLUSTER_SIZE / sizeof(struct efat_inode), GFP_KERNEL);
    original = (struct efat_inode*) inode->i_private;
    mutex_lock_interruptible(&efat_inode_mutex);
    for( i = 0 ; i < EFAT_INODESTORE_CLUSTER_COUNT; ++i){
        efat_read_cluster(sb, buffer, EFAT_INODESTORE_CLUSTER_NUMBER);
        e_inode = (struct efat_inode*)buffer;
        for( j = 0 ; j < CLUSTER_SIZE / sizeof(struct efat_inode); ++j){
            if(e_inode->first_cluster == original->first_cluster && e_inode->i_ino != original->i_ino){ // different inodes , but have pointers to the same clusters
                memcpy(res, e_inode, sizeof(struct efat_inode));
                kfree(buffer);
                mutex_unlock(&efat_inode_mutex);
                return res;
            }
            e_inode++;
        }
        kfree(buffer);
    }
    mutex_unlock(&efat_inode_mutex);
    return NULL;
}




struct inode* eqbl_fat_get_inode(struct super_block* sb, 
    const struct  inode* dir, struct dentry* dentry, umode_t mode){
    struct inode* inode;
    struct efat_inode *efat_i;
    inode = new_inode(sb);
    if(unlikely(!inode))
        return inode;
    inode_init_owner(inode, dir, mode);
    inode->i_atime =  inode->i_mtime = inode->i_ctime = CURRENT_TIME;
    efat_i = (struct efat_inode*) kmem_cache_alloc(efat_inode_cachep, GFP_KERNEL);
    memset(efat_i, 0 , sizeof(struct efat_inode));
    inode->i_ino = get_next_ino();
    efat_i->i_ino = inode->i_ino;
    inode->i_sb = sb;
    if(dentry)
        memcpy(efat_i->name, dentry->d_name.name, strlen(dentry->d_name.name));
    switch (mode & S_IFMT) {
        default:
            init_special_inode(inode, mode, 0);
            break;
        case S_IFREG:
            inode->i_op = &eqbl_fat_inode_operations;
            inode->i_fop = &eqbl_file_operations;
            efat_i->file_flags = 0b00000000;
            break;
        case S_IFDIR:
            inode->i_op = &eqbl_fat_inode_operations;
            inode->i_fop = &eqbl_dir_operations;
            efat_i->file_flags = 0b01000000;
            /* directory inodes start off with i_nlink == 2 (for "." entry) */
            inc_nlink(inode);
            break;
        case S_IFLNK:
            inode->i_op = &page_symlink_inode_operations;
            break;
        }
        inode->i_private = efat_i;
        return inode;
}

static int eqbl_fat_fill_sb( struct super_block* sb, void* data, int silent){
    struct inode *inode_root = NULL;
    struct eqbl_file_alloc_table *buffer_fat;
  //  struct request_queue *blk_queue = NULL;
    struct eqbl_fat_super_block *efat_sb;
    struct __eqbl_fat_super_block* __efat_sb;
    struct free_block* fr_block;
    char *buff;
    char *cluster_bufer;
    umode_t umode;
    struct efat_inode *efat_i;
    int i, j, ret = 0;
    buff = (char*)kmalloc(CLUSTER_SIZE, GFP_KERNEL);
    efat_read_cluster(sb, buff, EQBL_FAT_SUPER_BLOCK_OFFSET);
    efat_inode_count = 0;
    __efat_sb = ( struct __eqbl_fat_super_block* )buff;
    efat_sb = (struct eqbl_fat_super_block*)kmalloc(sizeof(struct eqbl_fat_super_block), GFP_KERNEL);
    if(unlikely(__efat_sb->magic != EQBL_FAT_MAGIC_NUMBER)){
        KERN_LOG("EQBL_FAT_FS_ERR: The magic number on device doesn't match magic number of FS...\n");
        __efat_sb->magic = EQBL_FAT_MAGIC_NUMBER;
        cluster_bufer = (char*)kmalloc(CLUSTER_SIZE, GFP_KERNEL);
        memcpy(cluster_bufer, __efat_sb, sizeof(struct efat_inode));
        efat_write_cluster(sb, cluster_bufer, EQBL_FAT_SUPER_BLOCK_OFFSET);
       // ret = -EACCES;
        //goto release;
    }
    efat_sb->__efat_sb = __efat_sb;
    sb->s_magic = EQBL_FAT_MAGIC_NUMBER;

    sb->s_fs_info = efat_sb;
    sb->s_maxbytes      = MAX_LFS_FILESIZE;
    sb->s_op = &eqbl_fat_super_operations;
    inode_root = eqbl_fat_get_inode(sb, NULL, NULL, S_IFDIR);
    umode = efat_parse_options(data);
    if(unlikely(!umode))
        return -EINVAL;
    inode_init_owner( inode_root, NULL, S_IFDIR | umode );
    sb->s_root = d_make_root( inode_root );
    memcpy(((struct efat_inode*)inode_root->i_private)->name, sb->s_root->d_name.name, strlen(sb->s_root->d_name.name));
    ((struct efat_inode*)(inode_root->i_private))->first_cluster = 0; // Because this is a root directory

    if( !sb->s_root ){
	    return -ENOMEM;
    }
    // reading fat table
    for( i = EQBL_FAT_ARRAY_OFFSET; i <= EQBL_FAT_ARRAY_SIZE  * sizeof(struct eqbl_file_alloc_table) / CLUSTER_SIZE; i += 1 ){
       if(unlikely(0 != efat_read_cluster(sb, buff , i)))
            return -EBUSY;
	   buffer_fat =  (struct eqbl_file_alloc_table*) buff;
	   for( j = 0 ; j < CLUSTER_SIZE / sizeof(struct eqbl_file_alloc_table); j += 1)
	       efat_sb->fat[CLUSTER_SIZE / sizeof(struct eqbl_file_alloc_table) * (i - EQBL_FAT_ARRAY_OFFSET) + j] = buffer_fat[j];
    }
    
    VALID_FAT[0] = 0;
    VALID_FAT[1] = 1;
    j = 0;
    for( i = 0; i < EQBL_FAT_ARRAY_SIZE; i += 1){
      if(efat_sb->fat[i].flag || 0b10000000 ){
	VALID_FAT[j] = i;
	j += 1;
	if(j == 2) // We found 2 fat
	  break;
      }
    }
    
    free_block_list_head = (struct free_block*)kmalloc( sizeof(struct free_block), GFP_KERNEL);
    if(!free_block_list_head){
    	ret = -ENOMEM;
    	goto release;
    }
    INIT_LIST_HEAD(&free_block_list_head->list);
    free_block_list_head->number = -1;
    for( i = 0 ; i < FAT_SIZE - sizeof( char ); i += 1){
      
      // Block of code, which checks fat tables to be equal with each other
      // If they don't, it will do something like voting function
 //      if(unlikely( fat[VALID_FAT[0]].data[i] != fat[VALID_FAT[1]].data[i] ||
	// 	   fat[VALID_FAT[0]].data[i] != fat[VALID_FAT[2]].data[i] ||
	// 	   fat[VALID_FAT[1]].data[i] != fat[VALID_FAT[2]].data[i]
	// 	 )
	// ) 
	// fat[VALID_FAT[0]].data[i] = fat[VALID_FAT[1]].data[i] = fat[VALID_FAT[2]].data[i] =
	// fat[VALID_FAT[0]].data[i] && fat[VALID_FAT[1]].data[i] || 
	// fat[VALID_FAT[0]].data[i] && fat[VALID_FAT[2]].data[i] || 
	// fat[VALID_FAT[1]].data[i] && fat[VALID_FAT[2]].data[i];
    if(efat_sb->fat[0].data[i] == 0 && i > 256){ // Filling double linked list of free blocks
    	fr_block = (struct free_block*)kmalloc( sizeof(struct free_block), GFP_KERNEL);
        if(!fr_block){
        	ret = -ENOMEM;
        	goto release;
        }
    	fr_block->number = i;
    	list_add(&fr_block->list, &free_block_list_head->list);
          }
    }

     for(i = EFAT_INODESTORE_CLUSTER_NUMBER ; i < EFAT_INODESTORE_CLUSTER_NUMBER +  EFAT_INODESTORE_CLUSTER_COUNT; ++i){
        efat_read_cluster(sb, buff, i);
        efat_i = (struct efat_inode*) buff;
        for(j = 0; j < CLUSTER_SIZE / sizeof(struct efat_inode); ++j){
            if(efat_i->first_cluster == 0 )
                goto release;
            ++efat_inode_count;
            ++efat_i;
        }
    }

release:
    kfree(buff);
    return ret;
}

static struct dentry *eqbl_fat_mount( struct file_system_type *fs_type_struct, int flags, const char* dev_name,  void *data){
	return mount_bdev(fs_type_struct, flags, dev_name, data, eqbl_fat_fill_sb);
}

static struct file_system_type eqbl_fat_struct = {
	.owner = THIS_MODULE,	     // for getting module and prevent release module before all mount points are umounted
	.name = EQBL_FAT_NAME,	     // name of filesystem in kernel table
	.mount = eqbl_fat_mount,     // the main function in the structure: it is executed during filesystem mounting 
	.kill_sb = kill_block_super, // kill_block_super is provided by the kernel
 	.fs_flags = FS_REQUIRES_DEV, // eqbl_fat needs disk for work
};

static int __init eqbl_fat_registrating( void ){

    // This cache is allocated for efat_inodes
    efat_inode_cachep = kmem_cache_create("efat_inode_cache",
                                         sizeof(struct efat_inode),
                                         0,
                                         (SLAB_RECLAIM_ACCOUNT| SLAB_MEM_SPREAD),
                                         NULL);
    if(unlikely(!efat_inode_cachep)){
	KERN_LOG("EFAT: Couldn't Allocate memory for slab cache\n");
        return -ENOMEM;
    }
    
	return register_filesystem( &eqbl_fat_struct ); // registrating the filesystem in the kernel
}

static void __exit eqbl_fat_releasing ( void ){
    kmem_cache_destroy(efat_inode_cachep);
    unregister_filesystem( &eqbl_fat_struct );
    return;
}

module_init( eqbl_fat_registrating )
module_exit( eqbl_fat_releasing )
