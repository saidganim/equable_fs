#include "eqbl_fat.h"
#include <linux/fs.h>
#include <linux/buffer_head.h>
/*
 * This file contains operations implementation
 */



// INODE_OPERATIONS


// TODO: While using sb_bread 2.4 kernel bdev mechanism. But in coming time need to change mechanism to bio struct
// Reading one cluster
inline int efat_read_cluster(struct super_block *sb, char* buffer, unsigned int number){
								unsigned int block_count, i;
								uint64_t offset;
								struct buffer_head *bh;
								block_count  = CLUSTER_SIZE / sb->s_blocksize;
								offset = block_count * number;
								for( i = 0; i < block_count; ++i) {
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
								for( i = 0; i < block_count; ++i) {
																bh = sb_bread(sb, offset + i);
																if(unlikely(!bh))
																								return -EBUSY;
																get_bh(bh);
																memcpy(bh->b_data, buffer + sb->s_blocksize * i, sb->s_blocksize);
																mark_buffer_dirty(bh);
																if(!sync_dirty_buffer(bh)) {
																								put_bh(bh);
																								brelse(bh);
																								return -EBUSY;
																}
																put_bh(bh);
																brelse(bh);
								};
								return 0;
};
