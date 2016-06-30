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


short int RECORD_COUNT = 0;
unsigned int VALID_FAT[2];

DEFINE_MUTEX(efat_sb_mutex);
DEFINE_MUTEX(efat_directory_children_mutex);
DEFINE_MUTEX(efat_inode_mutex);

uint64_t INODE_NUMBER = ROOT_INODE_NUMBER + 1;


struct eqbl_file_alloc_table{
    unsigned int data[FAT_SIZE - sizeof( char )];
    char flag;
};

struct free_block{
  struct list_head list;
  unsigned int number;
};





struct efat_inode {
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
    struct __eqbl_file_super_block* __efat_sb;
};



struct free_block* free_block_list_head;

static unsigned int efat_inode_count = 0;



//BDEV_OPERATIONS
inline unsigned int get_free_block( void );
inline int efat_read_cluster(struct super_block *sb, char* buffer, unsigned int number);
inline int efat_write_cluster(struct super_block *sb, char* buffer, unsigned int number);


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
            printk( KERN_ALERT " inode is located in root directory::: dentry name: <%s>  first_cluster = %d\n", efat_sync->name, efat_sync->first_cluster);
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

void sync_fat(struct super_block *sb){
    char* buff;
    unsigned int i, j;
    struct eqbl_fat_super_block* efat_sb;
    efat_sb = (struct eqbl_fat_super_block*)sb->s_fs_info;

    buff = (char*) kmalloc(CLUSTER_SIZE, GFP_KERNEL);
    for( i = EQBL_FAT_ARRAY_OFFSET; i <= EQBL_FAT_ARRAY_SIZE  * sizeof(struct eqbl_file_alloc_table) / CLUSTER_SIZE; i += 1 ){
     // Reading clusters step by step and putting data into array of fat structures  
        memcpy(buff, &efat_sb->fat[i - EQBL_FAT_ARRAY_OFFSET], CLUSTER_SIZE);
        if(unlikely(0 != efat_write_cluster(sb, buff , i)))
           return -EBUSY;
        printk( KERN_ALERT "EQBL_FAT_FS: WRITING FAT: %d cluster is being written...\n", i);
    }

    kfree(buff);
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
    struct inode *inode_root = NULL,  *inode;
    char *buff;
    struct efat_inode *efat_i;
    int i, j;
    printk(KERN_ALERT "LOOKUP::: Searching dentry %s\n", child->d_name.name);
    buff = (char*)kmalloc(CLUSTER_SIZE, GFP_KERNEL);
    for(i = EFAT_INODESTORE_CLUSTER_NUMBER ; i < EFAT_INODESTORE_CLUSTER_NUMBER +  EFAT_INODESTORE_CLUSTER_COUNT; ++i){
        efat_read_cluster(parent->i_sb, buff, i);
        efat_i = (struct efat_inode*) buff;
        for(j = 0; j < CLUSTER_SIZE / sizeof(struct efat_inode); ++j){
            printk(KERN_ALERT "Found dentry with name::: <%s> and first_cluster ::: <%d>\n", efat_i->name, efat_i->first_cluster);
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
                inode->i_private = kmalloc(sizeof(struct efat_inode), GFP_KERNEL);
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

	//TODO: The same situation as a efat_link function
	struct inode *inode = d_inode(dentry);
	 
	inode->i_ctime = dir->i_ctime = dir->i_mtime = CURRENT_TIME; 
	drop_nlink(inode);
	dput(dentry);
	return 0;
}

int efat_rmdir(struct inode *dir, struct dentry *dentry)
{
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
    struct eqbl_fat_super_block* efat_sb;
    efat_sb = (struct eqbl_fat_super_block*)filp->f_inode->i_sb->s_fs_info;
    loff_t shift;
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
    struct eqbl_fat_super_block* efat_sb;
    efat_sb = (struct eqbl_fat_super_block*)filp->f_inode->i_sb->s_fs_info;
    loff_t shift;
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
        printk( KERN_ALERT "WRITING INTO %d cluster \n", cluster_no);
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
    printk( KERN_ALERT " Trying to save efat_inode record name::: %s", efat_i->name);
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
    
    //spin_lock(&dentry->d_lock);
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
    printk( KERN_ALERT "READDIR is emitted on %s with cluster %d\n", filp->f_path.dentry->d_name.name, efat_i->first_cluster);
    if(unlikely(! efat_i->file_flags && 0b01000000))
        return -ENOTDIR;
    record_no = efat_i->first_cluster; //fat[0].data[efat_i->first_cluster];
    do {
        if(record_no == -1) break;
        efat_read_cluster(sb, buff, EFAT_INODESTORE_CLUSTER_NUMBER + record_no * sizeof(struct efat_inode) / CLUSTER_SIZE);
        printk( KERN_ALERT  "EFAT::: READING EFAT_INODE record on position number # <%d>", record_no);
        tmp_i = (struct efat_inode*) (buff + record_no * sizeof(struct efat_inode) % CLUSTER_SIZE);
        printk(KERN_ALERT "READDIR %s with first cluster = %d\n", tmp_i->name, tmp_i->first_cluster);
        dir_emit(ctx, tmp_i->name, strlen(tmp_i->name) , inode->i_ino, DT_UNKNOWN);
        ctx->pos += sizeof(struct efat_inode);
        pos += sizeof(struct efat_inode);
        record_no = efat_sb->fat[0].data[record_no];
    } while(efat_sb->fat[0].data[record_no] != 0 && efat_sb->fat[0].data[record_no] != -1);

    kfree(buff);
    //spin_unlock(&dentry->d_lock);
    return 0;
}


const struct file_operations eqbl_dir_operations ={
    // .open           = efat_dir_open,
    // .release        = efat_dir_close,
    // .llseek         = dcache_dir_lseek,
    // .read           = generic_read_dir,
    .iterate        = efat_readdir,
    // .fsync          = noop_fsync,
    

};


const struct file_operations eqbl_file_operations = {
     .read        = efat_read,
    // .aio_read   = generic_file_aio_read,
     .write      = efat_write,
     .open       = efat_dir_open,
    // .aio_write  = generic_file_aio_write,
    // .mmap       = generic_file_mmap,
    // .fsync      = noop_fsync,
    // .splice_read    = generic_file_splice_read,
    // .splice_write   = generic_file_splice_write,
    // .llseek     = generic_file_llseek,
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






static struct kmem_cache *efat_inode_cachep; // Kernel cache for inodes




// Reading one cluster
inline int efat_read_cluster(struct super_block *sb, char* buffer, unsigned int number){
    unsigned int block_count, i;
    uint64_t offset;
    struct buffer_head *bh;
    block_count  = CLUSTER_SIZE / sb->s_blocksize;
    offset = block_count * number;
    for( i = 0; i < block_count ; ++i){
        bh = sb_bread(sb, offset + i);
        if(unlikely(!bh))
            return -EBUSY;
        get_bh(bh);
        memcpy(buffer + sb->s_blocksize * i, bh->b_data, sb->s_blocksize);
        put_bh(bh);
        brelse(bh);
    };
    return 0;
};


// Writing one cluster
inline int efat_write_cluster(struct super_block *sb, char* buffer, unsigned int number){
    unsigned int block_count, i;
    uint64_t offset;
    struct buffer_head *bh;
    block_count  = CLUSTER_SIZE / sb->s_blocksize;
    offset = block_count * number;
    for( i = 0; i < block_count ; ++i){
        bh = sb_bread(sb, offset + i);
        if(unlikely(!bh))
            return -EBUSY;
        get_bh(bh);
        memcpy(bh->b_data, buffer + sb->s_blocksize * i, sb->s_blocksize);
        mark_buffer_dirty(bh);
        if(!sync_dirty_buffer(bh)){    
            put_bh(bh);
            brelse(bh);    
    	    return -EBUSY;
	    }
        put_bh(bh);
        brelse(bh);
    };
    return 0;
};


// Getting free block in FAT
inline unsigned int get_free_block( void ){
	struct free_block* free_b;
	unsigned int res;
    if(list_empty(&free_block_list_head->list))
        return -1; // there is no any free block
    printk(KERN_ALERT "GET_FREE_BLOCK_DEBUG : 1\n");
	free_b = list_entry(free_block_list_head->list.next, struct free_block, list );
	printk(KERN_ALERT "GET_FREE_BLOCK_DEBUG : 2\n");
    list_del_init(free_block_list_head->list.next);
	//free_block_list_head = free_b->list.next;
    res = free_b->number;
	kfree(free_b);
    printk(KERN_ALERT "GET_FREE_BLOCK_DEBUG : 3\n");
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
        printk(KERN_ALERT "Created regular file %d\n", efat_i->first_cluster);
    } else
    {
        efat_i->first_cluster = efat_inode_count + 2;
        printk(KERN_ALERT "Created directory %d\n", efat_i->first_cluster);
    }
        

    printk( KERN_ALERT " EFAT:: Created inode with first_cluster = %d on cluster in INODESTORE :: %d with offset :: %d\n", efat_i->first_cluster, cluster_no, cluster_off);
    efat_i->size = 0;
    efat_sb->fat[0].data[efat_i->first_cluster] = -1;
    memcpy(efat_i->name + 0, dentry->d_name.name, min(strlen(dentry->d_name.name), 64 - sizeof(char) - sizeof(uint64_t) - sizeof(uint64_t) - sizeof(loff_t)));
    memcpy(stored_efat_i, efat_i, sizeof(struct efat_inode));
    efat_write_cluster(inode->i_sb, buffer, EFAT_INODESTORE_CLUSTER_NUMBER + cluster_no);
    i = ((struct efat_inode*)dir->i_private)->first_cluster;
    last_cluster = i;
    while(true){
        printk("EFAT_DEBUG#1 : %d\n", i);
        i = efat_sb->fat[0].data[i];
        if( i != -1 && i != 0) 
            last_cluster = i;
        if(i == -1 || i == 0)
            break;
    }
    
    printk(KERN_ALERT "LAST_CLUSTER = %d\n", last_cluster);
    // last_cluster = fat[0].data[i]; 
    // fat[0].data[i] = efat_inode_count + 1; 
    // fat[0].data[fat[0].data[i]] = last_cluster;
    efat_sb->fat[0].data[last_cluster] = efat_inode_count; 
    efat_sb->fat[0].data[efat_sb->fat[0].data[last_cluster]] = -1;
    efat_sb->fat[0].data[efat_inode_count + 1] = -1;
    printk(KERN_ALERT "STRANGE MARK\n");
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

int efat_mkdir(struct inode * dir, struct dentry * dentry, umode_t mode)
{
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
    printk( KERN_ALERT "EQBL_FAT_FS:  equable filesystem is umounted\n");
};

void destroy_efat_inode(struct inode *inode){
    struct efat_inode* efat_inode;
    efat_inode = EFAT_INODE(inode);
    kmem_cache_free(efat_inode_cachep, efat_inode);
};








//DEPRECATED
// static struct efat_inode* efat_get_root_inode(struct super_block *sb){
//     struct eqbl_fat_super_block* efat_sb;
//     uint64_t offset;
//     struct buffer_head* bh;
//     struct efat_inode* res;
//     efat_sb = (struct eqbl_fat_super_block*)sb->s_fs_info;
//     offset = EQBL_FAT_ARRAY_SIZE * FAT_SIZE + 2; // Location of sector, containing root inode on disk
//     bh = sb_bread(sb, offset);
//     BUG_ON(!bh);
//     res = (struct efat_inode*) bh->b_data;
//     brelse(bh);
//     return res;
// }

enum {
    Opt_mode,
    Opt_err
};

static const match_table_t tokens = {
    {Opt_mode, "mode=%o"},
    {Opt_err, NULL}
};

static umode_t efat_parse_options(char *data)
{
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
    char buffer[EFAT_INODESTORE_CLUSTER_COUNT * CLUSTER_SIZE / sizeof(struct efat_inode)];
    int i, j;
    struct efat_inode* res = (struct efat_inode* ) kmalloc(sizeof(struct efat_inode), GFP_KERNEL);
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
    char buffer[EFAT_INODESTORE_CLUSTER_COUNT * CLUSTER_SIZE / sizeof(struct efat_inode)];
    int i, j;
    struct efat_inode* res = (struct efat_inode* ) kmalloc(sizeof(struct efat_inode), GFP_KERNEL);
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
    unsigned int cluster_no;
    struct efat_inode *efat_i;
    inode = new_inode(sb);
    if(unlikely(!inode))
        return inode;
    inode_init_owner(inode, dir, mode);
    inode->i_atime =  inode->i_mtime = inode->i_ctime = CURRENT_TIME;
    efat_i = (struct efat_inode*)kmalloc(sizeof(struct efat_inode),GFP_KERNEL); // kmem_cache_alloc(efat_inode_cachep, GFP_KERNEL);
    memset(efat_i, 0 , sizeof(struct efat_inode));
    inode->i_ino = get_next_ino();
    efat_i->i_ino = inode->i_ino;
    inode->i_sb = sb;
    printk( KERN_ALERT "metka #1\n");
    if(dentry)
        memcpy(efat_i->name, dentry->d_name.name, strlen(dentry->d_name.name));
    printk( KERN_ALERT "metka #2\n");
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
            printk( KERN_ALERT "metka #3\n");
            efat_i->file_flags = 0b01000000;
            /* directory inodes start off with i_nlink == 2 (for "." entry) */
            inc_nlink(inode);
            break;
        case S_IFLNK:
            inode->i_op = &page_symlink_inode_operations;
            break;
        }
        // cluster_no = 0;
        // if(dir){
        //     cluster_no = ((struct efat_inode*)dir->i_private)->first_cluster;
        //     while(fat[0].data[cluster_no] != 0  && fat[0].data[cluster_no] != -1){
        //         cluster_no = fat[0].data[cluster_no];
        //     }
        // }
        // fat[0].data[cluster_no] = efat_inode_count + 1;
        // fat[0].data[fat[0].data[cluster_no]] = -1;
        printk( KERN_ALERT "metka #4\n");
        inode->i_private = efat_i;
        return inode;
}


static int eqbl_fat_fill_sb( struct super_block* sb, void* data, int silent){
    struct inode *inode_root = NULL, *tmp_inode, **marked_inode;
    struct eqbl_file_alloc_table *buffer_fat;
    char* linked_inodes;
  //  struct request_queue *blk_queue = NULL;
    struct eqbl_fat_super_block *efat_sb;
    struct __eqbl_fat_super_block *__efat_sb;
    struct free_block* fr_block;
    char *buff, *store;
    char *cluster_bufer;
    umode_t umode;
    struct efat_inode *efat_i, *tmp_efat_i;
    int i, j, fat_ptr, ret = 0;
    buff = (char*)kmalloc(CLUSTER_SIZE, GFP_KERNEL);
    efat_read_cluster(sb, buff, EQBL_FAT_SUPER_BLOCK_OFFSET);
    printk( KERN_ALERT "SUPER_BLOCK ADDRES ::: %d\n", sb);
    efat_inode_count = 0;
    __efat_sb = ( struct eqbl_fat_super_block* )buff;
    efat_sb = (struct eqbl_fat_super_block*)kmalloc(sizeof(struct eqbl_fat_super_block), GFP_KERNEL);
    if(unlikely(__efat_sb->magic != EQBL_FAT_MAGIC_NUMBER)){
        printk( KERN_ALERT "EQBL_FAT_FS_ERR: The magic number on device doesn't match magic number of FS...%d\n", __efat_sb->magic);
        __efat_sb->magic = EQBL_FAT_MAGIC_NUMBER;
        cluster_bufer = (char*)kmalloc(CLUSTER_SIZE, GFP_KERNEL);
        memcpy(cluster_bufer, __efat_sb, sizeof(struct efat_inode));
        efat_write_cluster(sb, cluster_bufer, EQBL_FAT_SUPER_BLOCK_OFFSET);

       // ret = -EACCES;
        //goto release;
    }
    efat_sb->__efat_sb = __efat_sb;
    printk(KERN_ALERT "EFAT_INFO: superblock_blksize:%d\n",sb->s_blocksize);
    sb->s_magic = EQBL_FAT_MAGIC_NUMBER;

    sb->s_fs_info = efat_sb;
    //sb->s_maxbytes = MAX_FILE_SIZE;

    sb->s_maxbytes      = MAX_LFS_FILESIZE;
    //sb->s_blocksize     = 512;
    //sb->s_blocksize_bits    = PAGE_CACHE_SHIFT;

    sb->s_op = &eqbl_fat_super_operations; 		  // This field should be set in the future
    inode_root = eqbl_fat_get_inode(sb, NULL, NULL, S_IFDIR);
    umode = efat_parse_options(data);
    if(unlikely(!umode))
        return -EINVAL;
    inode_init_owner( inode_root, NULL, S_IFDIR | umode );
    sb->s_root = d_make_root( inode_root );
    memcpy(((struct efat_inode*)inode_root->i_private)->name, sb->s_root->d_name.name, strlen(sb->s_root->d_name.name));
    ((struct efat_inode*)(inode_root->i_private))->first_cluster = 0; // Because this is a root directory

    if( !sb->s_root ){
        printk( KERN_ALERT "EQBL_FAT_FS_ERR: Cannot allocate root dentry \n");
	    return -ENOMEM;
    }

    // reading fat table
    printk( KERN_ALERT "EQBL_FAT_FS_ERR: READING FAT...\n");
    for( i = EQBL_FAT_ARRAY_OFFSET; i <= EQBL_FAT_ARRAY_SIZE  * sizeof(struct eqbl_file_alloc_table) / CLUSTER_SIZE; i += 1 ){
      // Reading clusters step by step and putting data into array of fat structures  
	  // printk( KERN_ALERT "EQBL_FAT_FS_ERR: READING FAT: %d cluster is being read...\n", i);
       if(unlikely(0 != efat_read_cluster(sb, buff , i)))
            return -EBUSY;
	   buffer_fat =  (struct eqbl_file_alloc_table*) buff;
	   for( j = 0 ; j < CLUSTER_SIZE / sizeof(struct eqbl_file_alloc_table); j += 1)
	       efat_sb->fat[CLUSTER_SIZE / sizeof(struct eqbl_file_alloc_table) * (i - EQBL_FAT_ARRAY_OFFSET) + j] = buffer_fat[j];
    }
    
    VALID_FAT[0] = 0;
    VALID_FAT[1] = 1;
    VALID_FAT[2] = 2;
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
    	printk( KERN_ALERT " Cannot allocate memory for free_block_list_head\n");
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
    printk(KERN_ALERT "FAT[%d] = %d   ", i, efat_sb->fat[0].data[i]);
    if(efat_sb->fat[0].data[i] == 0 && i > 256){ // Filling double linked list of free blocks
    	fr_block = (struct free_block*)kmalloc( sizeof(struct free_block), GFP_KERNEL);
        if(!fr_block){
        	ret = -ENOMEM;
        	printk( KERN_ALERT " Cannot allocate memory for free_block #%d\n", i);
        	goto release;
        }
    	fr_block->number = i;
    	list_add(&fr_block->list, &free_block_list_head->list);
          }
     
    }

    //READING INODE_STORE

     for(i = EFAT_INODESTORE_CLUSTER_NUMBER ; i < EFAT_INODESTORE_CLUSTER_NUMBER +  EFAT_INODESTORE_CLUSTER_COUNT; ++i){
        efat_read_cluster(sb, buff, i);
        efat_i = (struct efat_inode*) buff;
        for(j = 0; j < CLUSTER_SIZE / sizeof(struct efat_inode); ++j){
            if(efat_i->first_cluster == 0 )
                goto end_of_inodestore;
            ++efat_inode_count;
            ++efat_i;
        }
    }

    end_of_inodestore:
    printk( KERN_ALERT "EFAT:: %d are found on disk", efat_inode_count);
//     marked_inode = (struct efat_inode**) kmalloc(EFAT_INODESTORE_CLUSTER_COUNT * CLUSTER_SIZE / sizeof(struct efat_inode), GFP_KERNEL);
//     memset(marked_inode, NULL , EFAT_INODESTORE_CLUSTER_COUNT * CLUSTER_SIZE / sizeof(struct efat_inode));
//     linked_inodes = (char*) kmalloc(EFAT_INODESTORE_CLUSTER_COUNT * CLUSTER_SIZE / sizeof(struct efat_inode), GFP_KERNEL);
//     memset(linked_inodes,0, EFAT_INODESTORE_CLUSTER_COUNT * CLUSTER_SIZE / sizeof(struct efat_inode));
//     store = (char*)kmalloc(CLUSTER_SIZE, GFP_KERNEL);
//     for(i = EFAT_INODESTORE_CLUSTER_NUMBER ; i < EFAT_INODESTORE_CLUSTER_NUMBER +  EFAT_INODESTORE_CLUSTER_COUNT; ++i){
//         efat_read_cluster(sb, buff, i);
//         efat_i = (struct efat_inode*) buff;
//         for(j = 0; j < CLUSTER_SIZE / sizeof(struct efat_inode); ++j){
//             printk( KERN_ALERT " inode is located in some directory::: first_cluster = %d\n", efat_i->first_cluster);
//             if(efat_i->first_cluster == 0 || efat_i->first_cluster == -1)
//             goto reading_root;
//             if(efat_i->file_flags && 0b01000000){ // Directory is found
//                 if(!marked_inode[efat_i->first_cluster]) {// Inode is not allocated in memory
//                     marked_inode[efat_i->first_cluster] = new_inode(sb);
//                     marked_inode[efat_i->first_cluster]->i_private = kmalloc(sizeof(struct efat_inode), GFP_KERNEL);
//                     memcpy(marked_inode[efat_i->first_cluster]->i_private, efat_i, sizeof(struct efat_inode));
//                 }
//                 fat_ptr = efat_i->first_cluster;
//                 while(fat_ptr > 0){
//                     efat_read_cluster(sb, store, EFAT_INODESTORE_CLUSTER_NUMBER + efat_i->first_cluster  / (CLUSTER_SIZE / sizeof(struct efat_inode)) );
//                     tmp_efat_i = store[efat_i->first_cluster % (CLUSTER_SIZE / sizeof(struct efat_inode))];
//                     //if(tmp_efat_i && 0b01000000){
//                         //marked_inode[tmp_efat_i->first_cluster] = kmalloc(sizeof(struct inode), GFP_KERNEL);
//                         //memcpy(marked_inode[tmp_efat_i->first_cluster],tmp_efat_i, sizeof(struct efat_inode));
//                     //}
//                     if(!marked_inode[tmp_efat_i->first_cluster]) {// Child inode is located in root directory
//                         marked_inode[tmp_efat_i->first_cluster] = new_inode(sb);
//                         marked_inode[tmp_efat_i->first_cluster]->i_private = kmalloc(sizeof(struct efat_inode), GFP_KERNEL);
//                         memcpy(marked_inode[tmp_efat_i->first_cluster]->i_private, tmp_efat_i, sizeof(struct efat_inode));
//                     }
//                     inode_init_owner(marked_inode[efat_i->first_cluster], efat_i, (tmp_efat_i->file_flags && 0b01000000? S_IFDIR : S_IFREG) | umode);
//                     linked_inodes[tmp_efat_i->first_cluster] = 1;
//                     fat_ptr = fat[0].data[fat_ptr];
//                 }
//             } else {
//                 if(!marked_inode[efat_i->first_cluster]) // Inode is not allocated in memory
//                     marked_inode[efat_i->first_cluster] = new_inode(sb);
//                     marked_inode[efat_i->first_cluster]->i_private = kmalloc(sizeof(struct efat_inode), GFP_KERNEL);
//                     memcpy(marked_inode[efat_i->first_cluster]->i_private, efat_i, sizeof(struct efat_inode));
//             }
//             ++efat_i;
//         }
//     }
// reading_root:
//     for(i = EFAT_INODESTORE_CLUSTER_NUMBER ; i < EFAT_INODESTORE_CLUSTER_NUMBER +  EFAT_INODESTORE_CLUSTER_COUNT; ++i){
//         efat_read_cluster(sb, buff, i);
//         efat_i = (struct efat_inode*) buff;
//         for(j = 0; j < CLUSTER_SIZE / sizeof(struct efat_inode); ++j){
//             printk( KERN_ALERT " inode is located in root directory::: first_cluster = %d\n", efat_i->first_cluster);
//             if(efat_i->first_cluster == 0 || efat_i->first_cluster == -1)
//             goto release;
//             if(!linked_inodes[efat_i->first_cluster]) {// Inode is not linked with parent
//                 dentry =
//                 inode_init_owner(marked_inode[efat_i->first_cluster], inode_root, (efat_i->file_flags && 0b01000000? S_IFDIR : S_IFREG) | umode);
//                 ++efat_inode_count;
//             }
//             ++efat_i;
//         }
//     }

release:
    kfree(buff);
    kfree(store);
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
        printk( KERN_ALERT "EQBL_FAT_FS_ERR: Cannot allocate memory for efat_inode_cache\n");
        return -ENOMEM;
    }
    
    printk( KERN_ALERT "EFS WAS INITIALIZATED\n");
	return register_filesystem( &eqbl_fat_struct ); // registrating the filesystem in the kernel
}


static void __exit eqbl_fat_releasing ( void ){
    kmem_cache_destroy(efat_inode_cachep);
    unregister_filesystem( &eqbl_fat_struct );
    printk( KERN_ALERT "EFS WAS RELEASED\n");
    return;
}

module_init( eqbl_fat_registrating )
module_exit( eqbl_fat_releasing )
