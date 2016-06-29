# equable_fs
Light-weight file system for Linux

For install this FS you need:
# make
# sudo insmod equable_fat_module.ko
# sudo mount -o loop -t eqbl_fs image dir
//=======================================
# sudo umount dir
# sudo rmmod equable_fat_module