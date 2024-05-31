/*
 * The Very Very Simple File System (vvsfs)
 * Eric McCreath 2006, 2008, 2010, 2020, 2023 - GPL
 * (based on the simplistic RAM filesystem McCreath 2001)
 *
 * Alwen Tiu, 2023. Added various improvements to allow multiple data blocks,
 * bitmaps, and using address space operations to simplify read/write.
 */

#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mpage.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/time.h>
// WTF this inlcude must be here or else it will break
#include <asm/uaccess.h>
#include "vvsfs.h"

// dependecy for debug print
#include <stdarg.h>

#define DEBUG 1

// inode cache -- this is used to attach vvsfs specific inode
// data to the vfs inode
static struct kmem_cache* vvsfs_inode_cache;

static struct address_space_operations vvsfs_as_operations;
static struct inode_operations         vvsfs_file_inode_operations;
static struct file_operations          vvsfs_file_operations;
static struct inode_operations         vvsfs_dir_inode_operations;
static struct file_operations          vvsfs_dir_operations;
static struct super_operations         vvsfs_ops;

/**
 * @brief Print fucntion for debugging
 * @param func_name name of the function you are calling this fucn
 * @param content content to be printed
 * @return void
 * @author Dai
 */
void _vvsfs_debug_print(const char* func_name, const char* fmt, ...) {
    va_list args;
    char*   new_fmt;
    if(!DEBUG)
        return;
    // prepent '[vvsfs]-func_name' to fmt
    new_fmt = kmalloc(strlen(fmt) + strlen(func_name) + strlen("[vvsfs]-") + 1, GFP_KERNEL);
    sprintf(new_fmt, "[vvsfs]-%s-%s", func_name, fmt);
    va_start(args, fmt);
    vprintk(new_fmt, args);
    va_end(args);
    kfree(new_fmt);
    printk("\n"); // To end the line
}

struct inode* vvsfs_iget(struct super_block* sb, unsigned long ino);

/**
 * @brief Translates a file's logical block number to a physical block number on disk and maps it to a buffer head.
 *
 * This function is used to translate a read or write operation for the "iblock"-th block
 * in a file to the actual disk block. It is used by the readpage/writepage operations for pagecache,
 * allowing for a more modular implementation of file read/write operations.
 *
 * @param[in]   inode  Pointer to the inode of the file.
 * @param[in]   iblock The logical block number within the file to be read or written.
 * @param[out]  bh     Pointer to the buffer head where the disk block should be loaded to.
 * @param[in]   create Flag to indicate whether to allocate a block on disk if it's not already allocated.
 *
 * @return Returns 0 on success, or a negative error code on failure.
 *         - -EFBIG if the block number is out of range.
 *         - -ENOSPC if there is no space left to allocate a new block.
 *
 */
static int vvsfs_file_get_block(struct inode* inode, sector_t iblock, struct buffer_head* bh, int create) {
    struct super_block*      sb  = inode->i_sb;
    struct vvsfs_sb_info*    sbi = sb->s_fs_info;
    struct vvsfs_inode_info* vi  = VVSFS_I(inode);
    uint32_t                 dno, bno;
    struct buffer_head*      indir_pointer_block; // should rename into indir_pointer_block
    uint32_t*                indir_addr;          // the memory address of the indirect pointer

    if(S_ISDIR(inode->i_mode)) {}

    if(DEBUG)
        printk("vvsfs - file_get_block");

    // exceed maximum file blocks
    if(iblock >= VVSFS_MAX_FILE_BLOCKS)
        return -EFBIG;

    // exceed maximum folder blocks
    if(S_ISDIR(inode->i_mode) && iblock >= VVSFS_DIRECT_BLOCKS)
        return -EFBIG;

    if(iblock > vi->i_db_count)
        return 0;

    // Case 1: The data block requests is pointed by a direct pointer (iblock < 14)
    if(iblock < VVSFS_DIRECT_BLOCKS) {
        // Case 1.1: The data block has not been created
        if(iblock == vi->i_db_count) {
            if(!create)
                return 0;
            dno = vvsfs_reserve_data_block(sbi->dmap);
            if(dno == 0)
                return -ENOSPC;
            vi->i_data[iblock] = dno;
            vi->i_db_count++;
            inode->i_blocks = vi->i_db_count * VVSFS_BLOCKSIZE / VVSFS_SECTORSIZE;
            bno             = vvsfs_get_data_block(dno);
            // Case 1.2: The data block has been created
        } else
            bno = vvsfs_get_data_block(vi->i_data[iblock]);
        // Case 2: The data block requests is pointed by a indirect pointer (iblock >= 14)
    } else {
        // 2#: If the block used to store indirect pointers has not been allocate (i_db_count = 14 && iblock >= 14),
        // allocate one
        if(vi->i_db_count == VVSFS_DIRECT_BLOCKS) {
            printk("vvsfs - file_get_block - creating indirect block");
            if(!create)
                return 0;
            // Reserve a block storing all indirect pointers
            dno = vvsfs_reserve_data_block(sbi->dmap);
            printk("Line 158 Reserve a block storing all indirect pointers"); // 111111
            if(dno == 0)
                return -ENOSPC;
            // Make the last pointer in i_data (storing 15 pointers) point to the block storing all indirect pointers
            vi->i_data[VVSFS_DIRECT_BLOCKS] = dno;
            printk("Line 163 Make the last pointer in i_data"); // 111111
        }

        // get the block containing indirect pointers
        indir_pointer_block = sb_bread(sb, vvsfs_get_data_block(vi->i_data[VVSFS_DIRECT_BLOCKS]));
        if(!indir_pointer_block)
            return -EIO;
        // the address containing the indirect pointer
        indir_addr = (uint32_t*)indir_pointer_block->b_data + (iblock - VVSFS_DIRECT_BLOCKS);

        printk("174!!!!!!!!");

        // Case 2.1: The data block has not been created
        if(iblock == vi->i_db_count) {
            if(!create)
                return 0;
            dno = vvsfs_reserve_data_block(sbi->dmap);
            if(dno == 0)
                return -ENOSPC;
            printk("183!!!!!!!!");

            memcpy(indir_addr, &dno, sizeof(uint32_t));
            vi->i_db_count++;
            inode->i_blocks = vi->i_db_count * VVSFS_BLOCKSIZE / VVSFS_SECTORSIZE;
            bno             = vvsfs_get_data_block(dno);

            printk("190!!!!!!!!");

            // Page has been modified, marked as dirty
            mark_buffer_dirty(indir_pointer_block);
            sync_dirty_buffer(indir_pointer_block);

            printk("196!!!!!!!!");

            // Case 2.2: The data block has been created
        } else {
            bno = vvsfs_get_data_block(*indir_addr);
        }
        brelse(indir_pointer_block);
    }
    map_bh(bh, sb, bno);
    return 0;
}

// Address pace operation readpage/readfolio.
// You do not need to modify this.
static int
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 19, 0)
vvsfs_readpage(struct file* file, struct page* page) {
    if(DEBUG)
        printk("vvsfs - readpage");
    return mpage_readpage(page, vvsfs_file_get_block);
}
#else
vvsfs_read_folio(struct file* file, struct folio* folio) {
    if(DEBUG)
        printk("vvsfs - read folio");
    return mpage_read_folio(folio, vvsfs_file_get_block);
}
#endif

// Address pace operation readpage.
// You do not need to modify this.
static int vvsfs_writepage(struct page* page, struct writeback_control* wbc) {
    if(DEBUG)
        printk("vvsfs - writepage");

    return block_write_full_page(page, vvsfs_file_get_block, wbc);
}

// Address pace operation readpage.
// You do not need to modify this.
static int vvsfs_write_begin(struct file* file, struct address_space* mapping, loff_t pos, unsigned int len,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 19, 0)
                             unsigned int flags,
#endif
                             struct page** pagep, void** fsdata) {
    printk("vvsfs - write_begin");

    if(pos + len > VVSFS_MAXFILESIZE)
        return -ENOSPC;


#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 19, 0)
    return block_write_begin(mapping, pos, len, flags, pagep, vvsfs_file_get_block);
#else
    return block_write_begin(mapping, pos, len, pagep, vvsfs_file_get_block);
#endif
}

// Address pace operation readpage.
// May require some modification to include additonal inode data.
static int vvsfs_write_end(struct file* file, struct address_space* mapping, loff_t pos, unsigned int len,
                           unsigned int copied, struct page* page, void* fsdata) {
    // struct inode *inode = file->f_inode;
    struct inode*            inode = mapping->host;
    struct vvsfs_inode_info* vi    = VVSFS_I(inode);
    int                      ret;

    printk("vvsfs - write_end");

    ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
    if(ret < len) {
        printk("wrote less than requested.");
        return ret;
    }

    /* Update inode metadata */
    inode->i_blocks = vi->i_db_count * VVSFS_BLOCKSIZE / VVSFS_SECTORSIZE;
    inode->i_mtime = inode->i_ctime = current_time(inode);
    mark_inode_dirty(inode);

    return ret;
}

static struct address_space_operations vvsfs_as_operations = {

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 19, 0)
    readpage : vvsfs_readpage,
#else
    read_folio : vvsfs_read_folio,
#endif
    writepage : vvsfs_writepage,
    write_begin : vvsfs_write_begin,
    write_end : vvsfs_write_end,
};


// vvsfs_readdir - reads a directory and places the result using filldir
static int vvsfs_readdir(struct file* filp, struct dir_context* ctx) {
    struct inode*            dir;
    struct vvsfs_inode_info* vi;
    struct super_block*      sb;
    int                      num_dirs;
    struct vvsfs_dir_entry*  dent;
    int                      i;
    struct buffer_head*      bh;
    char*                    data;

    if(DEBUG)
        printk("vvsfs - readdir\n");

    // get the directory inode from file
    dir = file_inode(filp);

    // get the vvsfs specific inode information from dir inode
    vi = VVSFS_I(dir);

    // calculate the number of entries in the directory
    num_dirs = dir->i_size / VVSFS_DENTRYSIZE;
    if(DEBUG)
        _vvsfs_debug_print(__func__, "num_dirs %d", num_dirs);

    // get the superblock object from the inode; we'll need this
    // for reading/writing to disk blocks
    sb = dir->i_sb;

    if(DEBUG)
        printk("Number of entries %d fpos %Ld\n", num_dirs, filp->f_pos);

    // Read all directory entries from disk to memory, and "emit" those entries
    // to dentry cache.

    data = kzalloc(vi->i_db_count * VVSFS_BLOCKSIZE, GFP_KERNEL);
    if(!data)
        return -ENOMEM;



    for(i = 0; i < vi->i_db_count; ++i) {

        printk("readdir - reading dno: %d, disk block: %d", vi->i_data[i], vvsfs_get_data_block(vi->i_data[i]));
        bh = sb_bread(sb, vvsfs_get_data_block(vi->i_data[i]));
        if(!bh) {
            kfree(data);
            return -EIO;
        }
        memcpy(data + i * VVSFS_BLOCKSIZE, bh->b_data, VVSFS_BLOCKSIZE);
        brelse(bh);
    }

    for(i = ctx->pos / VVSFS_DENTRYSIZE; i < num_dirs && filp->f_pos < dir->i_size; ++i) {
        dent = (struct vvsfs_dir_entry*)(data + i * VVSFS_DENTRYSIZE);
        if(!dir_emit(ctx, dent->name, strnlen(dent->name, VVSFS_MAXNAME), dent->inode_number, DT_UNKNOWN)) {
            if(DEBUG)
                printk("vvsfs -- readdir - failed dir_emit");
            break;
        }
        ctx->pos += VVSFS_DENTRYSIZE;
    }
    kfree(data);

    // update access time
    dir->i_atime = current_time(dir);
    mark_inode_dirty(dir);

    if(DEBUG)
        printk("vvsfs - readdir - done");

    return 0;
}


/**
 * @brief Looks up a dentry in a directory.
 *
 * This function searches for a dentry in the directory associated with the given inode. If the dentry is found,
 * an inode is allocated and filled with the information from the disk, and the dentry is then associated with this
 * inode.
 *
 * @param dir Pointer to the inode of the directory to search.
 * @param dentry Pointer to the dentry to look for.
 * @param flags Flags for the lookup (not used in this implementation).
 *
 * @return Returns a pointer to the dentry if found, NULL if the dentry is not found, or an error pointer
 *         in case of failure. Possible error pointer values:
 *         - -ENOMEM if memory allocation fails.
 *         - -EIO if a block read operation fails.
 *         - -EACCES if inode retrieval fails.
 * @paragraph // vvsfs_lookup - A file/directory name in a directory. It basically attaches // the inode of the file to
 * the directory entry.
 * @note The function reads all data blocks of the directory into a kernel buffer, then iterates through the directory
 * entries. If a matching entry is found, it retrieves the corresponding inode, associates the dentry with the inode,
 *       and returns the dentry. The kernel buffer is freed before returning from the function.
 * @todo modify this fucntion for indrect inode table
 */
static struct dentry* vvsfs_lookup(struct inode* dir, struct dentry* dentry, unsigned int flags) {
    int                      num_dirs;
    int                      i;
    struct vvsfs_inode_info* vi;
    struct inode*            inode = NULL;
    struct vvsfs_dir_entry*  dent;
    struct buffer_head*      bh;
    struct super_block*      sb;
    char*                    data;

    if(DEBUG)
        _vvsfs_debug_print(__func__, "lookup %s", dentry->d_name.name);

    sb = dir->i_sb;

    vi       = VVSFS_I(dir);
    num_dirs = dir->i_size / VVSFS_DENTRYSIZE;

    data = kzalloc(vi->i_db_count * VVSFS_BLOCKSIZE, GFP_KERNEL);
    if(!data)
        return ERR_PTR(-ENOMEM);

    for(i = 0; i < vi->i_db_count; ++i) {
        printk("lookup - reading dno: %d, disk block: %d", vi->i_data[i], vvsfs_get_data_block(vi->i_data[i]));

        bh = sb_bread(sb, vvsfs_get_data_block(vi->i_data[i]));
        if(!bh) {
            kfree(data);
            return ERR_PTR(-EIO);
        }
        memcpy(data + i * VVSFS_BLOCKSIZE, bh->b_data, VVSFS_BLOCKSIZE);
        brelse(bh);
    }

    for(i = 0; i < num_dirs; ++i) {
        dent = (struct vvsfs_dir_entry*)(data + i * VVSFS_DENTRYSIZE);

        if((strlen(dent->name) == dentry->d_name.len) &&
           strncmp(dent->name, dentry->d_name.name, dentry->d_name.len) == 0) {
            inode = vvsfs_iget(dir->i_sb, dent->inode_number);
            if(!inode) {
                return ERR_PTR(-EACCES);
            }
            d_add(dentry, inode);
            break;
        }
    }
    kfree(data);
    return NULL;
}

// vvsfs_new_inode - find and construct a new inode.
// @dir: the inode of the parent directory where the new inode is supposed to be
// attached to.
// @mode: the mode information of the new inode
//
// This is a helper function for the inode operation "create" (implemented in
// vvsfs_create() ). It takes care of reserving an inode block on disk (by
// modifiying the inode bitmap), creating an VFS inode object (in memory) and
// attach filesystem-specific information to that VFS inode.
struct inode* vvsfs_new_inode(const struct inode* dir, umode_t mode) {
    struct vvsfs_inode_info* inode_info;
    struct super_block*      sb;
    struct vvsfs_sb_info*    sbi;
    struct inode*            inode;
    unsigned long            dno, ino;
    int                      i;

    if(DEBUG)
        printk("vvsfs - new inode\n");

    // get the filesystem specific info for the super block. The sbi object
    // contains the inode bitmap.
    sb  = dir->i_sb;
    sbi = sb->s_fs_info;

    /*
        Find a spare inode in the vvsfs.
        The vvsfs_reserve_inode_block() will attempt to find the first free
       inode and allocates it, and returns the inode number. Note that the inode
       number is *not* the same as the disk block address on disk.
    */
    ino = vvsfs_reserve_inode_block(sbi->imap);
    if(BAD_INO(ino))
        return ERR_PTR(-ENOSPC);

    /*
        Find a spare data block.
        By default, a new data block is reserved for the new inode.
        This is probably a bit wasteful if the file/directory does not need it
        immediately.
        The `dno` here represents a data block position within the data bitmap
        so it's not the actual disk block location.
     */
    dno = vvsfs_reserve_data_block(sbi->dmap);
    if(dno == 0) {
        vvsfs_free_inode_block(sbi->imap, ino); // if we failed to allocate data block, release the
                                                // inode block. and return an error code.
        return ERR_PTR(-ENOSPC);
    }

    /* create a new VFS (in memory) inode */
    inode = new_inode(sb);
    if(!inode) {
        // if failed, release the inode/data blocks so they can be reused.
        vvsfs_free_inode_block(sbi->imap, ino);
        vvsfs_free_data_block(sbi->dmap, dno);
        return ERR_PTR(-ENOMEM);
    }

    // fill in various information for the VFS inode.
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
    inode_init_owner(&init_user_ns, inode, dir, mode);
#else
    inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
#endif
    inode->i_ino   = ino;
    inode->i_ctime = inode->i_mtime = inode->i_atime = current_time(inode);
    inode->i_mode                                    = mode;
    inode->i_size                                    = 0;
    inode->i_blocks                                  = (VVSFS_BLOCKSIZE / VVSFS_SECTORSIZE);
    // increment the link counter. This basically increments inode->i_nlink,
    // but that member cannot be modified directly. Use instead set_nlink to set
    // it to a specific value.
    set_nlink(inode, 1);

    // check if the inode is for a directory, using the macro S_ISDIR
    if(S_ISDIR(mode)) {
        inode->i_op  = &vvsfs_dir_inode_operations;
        inode->i_fop = &vvsfs_dir_operations;
    } else {
        inode->i_op  = &vvsfs_file_inode_operations;
        inode->i_fop = &vvsfs_file_operations;
        // if the inode is a file, set the address space operations
        inode->i_mapping->a_ops = &vvsfs_as_operations;
    }

    /*
        Now fill in the filesystem specific information.
        This is done by first obtaining the vvsfs_inode_info struct from
        the VFS inode using the VVSFS_I macro.
     */
    inode_info             = VVSFS_I(inode);
    inode_info->i_db_count = 1;
    inode_info->i_data[0]  = dno;
    for(i = 1; i < VVSFS_N_BLOCKS; ++i)
        inode_info->i_data[i] = 0;

    // Make sure you hash the inode, so that VFS can keep track of its "dirty"
    // status and writes it to disk if needed.
    insert_inode_hash(inode);

    // Mark the inode as "dirty". This will inform the VFS that this inode needs
    // to be written to disk. The procedure for writing to disk is implemented
    // in vvsfs_write_inode() (as part of the "super" operations).
    mark_inode_dirty(inode);

    if(DEBUG)
        printk("vvsfs - new_inode - done");
    return inode;
}



// This is a helper function for the "create" inode operation. It adds a new
// entry to the list of directory entries in the parent directory.
/**
 * @todo modify this fucntion for indrect inode table
 */
static int vvsfs_add_new_entry(struct inode* dir, struct dentry* dentry, struct inode* inode) {
    struct vvsfs_inode_info* dir_info = VVSFS_I(dir);
    struct super_block*      sb       = dir->i_sb;
    struct vvsfs_sb_info*    sbi      = sb->s_fs_info;
    struct vvsfs_dir_entry*  dent;
    struct buffer_head*      bh;
    int                      num_dirs;
    uint32_t                 d_pos, d_off, dno, newblock;

    // calculate the number of entries from the i_size of the directory's inode.
    num_dirs = dir->i_size / VVSFS_DENTRYSIZE;
    if(num_dirs >= VVSFS_MAX_DENTRIES)
        return -ENOSPC;

    // Calculate the position of the new entry within the data blocks
    d_pos = num_dirs / VVSFS_N_DENTRY_PER_BLOCK;
    d_off = num_dirs % VVSFS_N_DENTRY_PER_BLOCK;

    /* If the block is not yet allocated, allocate it. */
    if(d_pos >= dir_info->i_db_count) {
        printk("vvsfs - create - add new data block for directory entry");
        newblock = vvsfs_reserve_data_block(sbi->dmap);
        if(newblock == 0)
            return -ENOSPC;
        dir_info->i_data[d_pos] = newblock;
        dir_info->i_db_count++;
    }

    /* Update the on-disk structure */

    dno = dir_info->i_data[d_pos];
    printk("vvsfs - add_new_entry - reading dno: %d, d_pos: %d, block: %d", dno, d_pos, vvsfs_get_data_block(dno));

    // Note that the i_data contains the data block position within the data
    // bitmap, This needs to be converted to actual disk block position if you
    // want to read it, using vvsfs_get_data_block().
    bh = sb_bread(sb, vvsfs_get_data_block(dno));
    if(!bh)
        return -ENOMEM;
    dent = (struct vvsfs_dir_entry*)(bh->b_data + d_off * VVSFS_DENTRYSIZE);
    strncpy(dent->name, dentry->d_name.name, dentry->d_name.len);
    dent->name[dentry->d_name.len] = '\0';
    dent->inode_number             = inode->i_ino;
    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);

    if(DEBUG)
        printk("vvsfs - add_new_entry - directory entry (%s, %d) added to "
               "block %d",
               dent->name, dent->inode_number, vvsfs_get_data_block(dir_info->i_data[d_pos]));

    dir->i_size   = (num_dirs + 1) * VVSFS_DENTRYSIZE;
    dir->i_blocks = dir_info->i_db_count * (VVSFS_BLOCKSIZE / VVSFS_SECTORSIZE);
    dir->i_atime  = current_time(dir);
    dir->i_mtime  = current_time(dir);
    mark_inode_dirty(dir);
    return 0;
}

// The "create" operation for inode.
// This is called when a new file/directory is created.
static int
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
vvsfs_create(struct user_namespace* namespace,
#else
vvsfs_create(struct mnt_idmap* namespace,
#endif
             struct inode* dir, struct dentry* dentry, umode_t mode, bool excl) {
    struct vvsfs_inode_info* dir_info;
    int                      ret;
    struct buffer_head*      bh;
    struct inode*            inode;

    if(DEBUG)
        printk("vvsfs - create : %s\n", dentry->d_name.name);

    if(dentry->d_name.len > VVSFS_MAXNAME) {
        printk("vvsfs - create - file name too long");
        return -ENAMETOOLONG;
    }

    dir_info = VVSFS_I(dir);
    if(!dir_info) {
        printk("vvsfs - create - vi_dir null!");
        return -EINVAL;
    }

    // create a new inode for the new file/directory
    inode = vvsfs_new_inode(dir, mode);


    if(IS_ERR(inode)) {
        printk("vvsfs - create - new_inode error!");
        brelse(bh);
        return -ENOSPC;
    }


    // add the file/directory to the parent directory's list
    // of entries -- on disk.
    ret = vvsfs_add_new_entry(dir, dentry, inode);
    if(ret != 0) {
        return ret;
    }
    // add information on inode
    inode->i_uid   = current_uid();
    inode->i_gid   = current_gid();
    inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
    // attach the new inode object to the VFS directory entry object.
    d_instantiate(dentry, inode);

    printk("File created %ld\n", inode->i_ino);
    return 0;
}



//----------------Assignment Implementation----------------//

/**
 * @todo This method need to be modifies to fit indirect block table
 * @brief find the block no and offset of a dentry in a directory block
 * @param inode
 * @param dentry
 * @param[out] block
 * @param[out] offset
 * @return 0 on success, -1 on entry not found and errno on error
 * @author Dai
 * @date 14/10/2023
 */
static int vvsfs_find_entry(struct inode* inode, struct dentry* dentry, int* block, int* offset) {
    struct vvsfs_inode_info* dir_info = VVSFS_I(inode);
    struct super_block*      sb       = inode->i_sb;
    // struct vvsfs_sb_info*    sbi      = sb->s_fs_info;
    struct buffer_head*     bh;
    struct vvsfs_dir_entry* dir_entry_buf;
    // int                      need_syn    = 0;
    int num_dirs    = inode->i_size / VVSFS_DENTRYSIZE;
    int dir_checked = 0;
    int i           = 0;
    int j           = 0;
    if(!S_ISDIR(inode->i_mode)) {
        return ENOTDIR;
    }

    for(i = 0; i < dir_info->i_db_count; i++) {
        bh = sb_bread(sb, vvsfs_get_data_block(dir_info->i_data[i]));
        if(!bh)
            return -ENOMEM;
        // iterate through detnries
        dir_entry_buf = (struct vvsfs_dir_entry*)(bh->b_data);

        for(j = 0; j < VVSFS_N_DENTRY_PER_BLOCK; j++) {
            if(dir_checked > num_dirs) {
                brelse(bh);
                return -1;
                break; // checked through all direcories
            }
            if(strcmp(dir_entry_buf->name, dentry->d_name.name) == 0) {
                // found dentry, modify the buffeer, and sync
                *block  = i;
                *offset = j;
                brelse(bh);
                return 0;
            } else {
                dir_entry_buf++;
            }
            dir_checked++;
        }
        brelse(bh);
    }
    return -1;
}

/**
 * @brief inlie fucntrion for processing the erro return by vvsfs_find_entry
 */
static inline int vvsfs_find_entry_erro(int vvsfs_find_entry_rtn) {
    if(vvsfs_find_entry_rtn == -1) {
        if(DEBUG)
            _vvsfs_debug_print(__func__, "vvsfs_delete_entry - entry not found");
        return -1;
    }
    if(vvsfs_find_entry_rtn != 0) {
        if(DEBUG)
            _vvsfs_debug_print(__func__, "vvsfs_delete_entry - find_entry_rtn %d", vvsfs_find_entry_rtn);
        return vvsfs_find_entry_rtn;
    }
    return 0;
}

int is_block_empty(struct buffer_head* bh);
/**
 * @todo this fucntio need to be modified to fit the indirect block
 * @brief give inode dir, remove dentry and write back ?
 * @param dir inode to the directory
 * @param dentry dentry of file to be deleted
 * @return 0 if success; errno on failure; -1 on entry not found
 * @author Dai
 * @date 16/10/2023
 */
static int vvsfs_delete_entry(struct inode* v_inode, struct dentry* dentry) {

    struct vvsfs_inode_info* dir_info = VVSFS_I(v_inode);
    struct super_block*      sb       = v_inode->i_sb;
    // struct vvsfs_sb_info*    sbi      = sb->s_fs_info;
    struct vvsfs_dir_entry* dent;
    struct buffer_head*     bh;
    struct buffer_head*     tail_bh;
    struct vvsfs_dir_entry* dir_entry_buf;
    // int                      i;
    // int                      j;
    // int                      need_syn = 0;
    int num_dirs = v_inode->i_size / VVSFS_DENTRYSIZE;
    int find_entry_rtn;
    int block  = 0;
    int offset = 0;
    // finding page and offset of the last directory entry
    int tail_pos = num_dirs / VVSFS_N_DENTRY_PER_BLOCK;
    int tail_off = ((num_dirs % VVSFS_N_DENTRY_PER_BLOCK) - 1);
    if(DEBUG)
        _vvsfs_debug_print(__func__, "enter");
    if(tail_off == -1) {
        tail_pos--;
        tail_off = VVSFS_N_DENTRY_PER_BLOCK - 1;
    }
    _vvsfs_debug_print(__func__, "start removing %s", dentry->d_name.name);
    // find direcroty

    find_entry_rtn = vvsfs_find_entry(v_inode, dentry, &block, &offset);
    if(find_entry_rtn == -1) {
        _vvsfs_debug_print(__func__, "vvsfs_delete_entry - entry not found");
        return -1;
    }
    if(find_entry_rtn != 0) {
        _vvsfs_debug_print(__func__, "vvsfs_delete_entry - find_entry_rtn %d", find_entry_rtn);
        return find_entry_rtn;
    }

    // read the block (target dir and dir for swap)
    bh = sb_bread(sb, vvsfs_get_data_block(dir_info->i_data[block]));
    if(!bh) {
        _vvsfs_debug_print(__func__, "vvsfs_delete_entry - read target bh failed");
        return -ENOMEM;
    }
    tail_bh = sb_bread(sb, vvsfs_get_data_block(dir_info->i_data[tail_pos]));

    if(!tail_bh) {
        _vvsfs_debug_print(__func__, "vvsfs_delete_entry - read tail bh failed");
        brelse(bh);
        return -ENOMEM;
    }

    // swap the last entry to the target entry
    dir_entry_buf = (struct vvsfs_dir_entry*)(bh->b_data + offset * VVSFS_DENTRYSIZE);
    dent          = (struct vvsfs_dir_entry*)(tail_bh->b_data + tail_off * VVSFS_DENTRYSIZE);
    memcpy(dir_entry_buf, dent, VVSFS_DENTRYSIZE);
    // clear the last entry
    memset(dent, 0, VVSFS_DENTRYSIZE);

    // Update the inode size and times
    v_inode->i_size -= VVSFS_DENTRYSIZE;
    v_inode->i_atime = current_time(v_inode);
    v_inode->i_mtime = current_time(v_inode);

    // Sync the buffers
    mark_buffer_dirty(bh);
    mark_buffer_dirty(tail_bh);
    sync_dirty_buffer(bh);
    sync_dirty_buffer(tail_bh);

    // Release the buffers
    brelse(tail_bh);
    brelse(bh);

    // Mark inode as dirty
    mark_inode_dirty(v_inode);

    _vvsfs_debug_print(__func__, "vvsfs_delete_entry - done");
    return 0;
}

/*check whether is empty*/
int is_block_empty(struct buffer_head* bh) {
    int                     i;
    struct vvsfs_dir_entry* entry;
    for(i = 0; i < VVSFS_N_DENTRY_PER_BLOCK; i++) {
        entry = (struct vvsfs_dir_entry*)(bh->b_data + i * VVSFS_DENTRYSIZE);
        if(entry->inode_number != 0) { // Assuming that an inode_num of 0 means the entry is empty
            return 0;                  // Block is not empty
        }
    }
    return 1; // Block is empty
}

// vvsfs_add_new_entry already inplemented in the template


/**
 * @todo this fucntion needs to be modified to fit the indirect block
 * @brief We do not allow renaming to a fiel taht already exists
 * @param dir directory containing the file
 * @param dentry detnry of the file to be renamed
 * @param new_name
 * @return 0 on success , -1 on original entry do not exist , errno on failure
 * @author Dai
 * @date 2023/10/16
 */
static int vvsfs_rename_entry(struct inode* dir, struct dentry* dentry, const char* new_name) {
    struct buffer_head*      bh;
    struct vvsfs_inode_info* dir_info = VVSFS_I(dir);
    struct super_block*      sb       = dir->i_sb;
    struct vvsfs_dir_entry*  dent;
    // int                     i              = 0;
    int           block          = 0;
    int           offset         = 0;
    int           find_entry_rtn = vvsfs_find_entry(dir, dentry, &block, &offset);
    struct inode* dentry_inode   = dentry->d_inode;
    // error catching
    if(strlen(new_name) > VVSFS_MAXNAME) {
        _vvsfs_debug_print(__func__, "vvsfs_rename_entry - new name too long");
        return -ENAMETOOLONG;
    }
    vvsfs_find_entry_erro(find_entry_rtn);

    // read the entry
    bh = sb_bread(sb, vvsfs_get_data_block(dir_info->i_data[block]));
    if(!bh) {
        _vvsfs_debug_print(__func__, "vvsfs_rename_entry - read target bh failed");
        return -ENOMEM;
    }

    dent = (struct vvsfs_dir_entry*)(bh->b_data + offset * VVSFS_DENTRYSIZE);
    // change name of the dent
    strncpy(dent->name, new_name, strlen(new_name));
    // udpate time on inode of the dent


    dentry_inode->i_mtime = current_time(dentry_inode);
    dentry_inode->i_atime = current_time(dentry_inode);
    mark_inode_dirty(dentry_inode);

    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);
    return 0;
}

/**
 * @note if correct it does not need to be modified to fit indirect block
 * @brief removing link to a file in a directory
 * @param parent_dir inode to the directory
 * @param target_dentry dentry of file to be deleted
 * @return 0(on success) ,kernel ERRONO on failure
 * @author Dai
 * @date 2023/10/16
 */
static int vvsfs_unlink(struct inode* parent_dir, struct dentry* target_dentry) {
    struct super_block* sb;
    // struct vvsfs_inode*      disk_inode;
    struct vvsfs_inode_info* inode_info;
    // struct buffer_head*      bh;
    struct vvsfs_sb_info* sysinfo;
    // uint32_t                 inode_block;
    // uint32_t                 inode_offset;
    int                 rm_entry_rtn;
    struct buffer_head* indir_block_buffer;
    int                 i;
    uint32_t*           indir_pointers;
    rm_entry_rtn = vvsfs_delete_entry(parent_dir, target_dentry);
    if(rm_entry_rtn == 0) {
        // update file inforamtion
        inode_dec_link_count(target_dentry->d_inode);
        if(target_dentry->d_inode->i_link == 0) {

            printk("896");

            // free the inode
            sb                 = parent_dir->i_sb;
            sysinfo            = sb->s_fs_info;
            inode_info         = VVSFS_I(target_dentry->d_inode);
            indir_block_buffer = NULL;
            indir_pointers     = NULL;

            if(DEBUG)
                _vvsfs_debug_print(__func__, "freeing inode %ld and %d datablocks", target_dentry->d_inode->i_ino,
                                   inode_info->i_db_count);

            // find amount of blocksin the inod3e
            i = 0;


            if(inode_info->i_db_count > VVSFS_DIRECT_BLOCKS) {
                _vvsfs_debug_print(__func__, "freeing indirect block");
                // Case 1: The data block requests is pointed by a direct pointer (iblock < 14)
                // free blocks in the indirect block
                indir_block_buffer = sb_bread(sb, vvsfs_get_data_block(inode_info->i_data[VVSFS_DIRECT_BLOCKS]));
                indir_pointers     = (uint32_t*)indir_block_buffer->b_data;

                for(i = 0; i < (inode_info->i_db_count - VVSFS_DIRECT_BLOCKS); i++) {
                    _vvsfs_debug_print(__func__, "Free indirect block %Ld", indir_pointers[i]);
                    vvsfs_free_data_block(sysinfo->dmap, indir_pointers[i]);
                }
                brelse(indir_block_buffer);

                for(i = 0; i < VVSFS_N_BLOCKS; i++) {
                    vvsfs_free_data_block(sysinfo->dmap, inode_info->i_data[i]);
                }
            } else {
                // Case 2: no indirect block need to be freed
                _vvsfs_debug_print(__func__, "freeing direct block");
                for(i = 0; i < inode_info->i_db_count; i++) {
                    vvsfs_free_data_block(sysinfo->dmap, inode_info->i_data[i]);
                    printk("free data");
                }
            }

            // printk()
            vvsfs_free_inode_block(sysinfo->imap, d_inode(target_dentry)->i_ino);
            printk("inode data");
        }
        // mari inode dirty
        mark_inode_dirty(target_dentry->d_inode);
        _vvsfs_debug_print(__func__, "new dir entry count %d", parent_dir->i_size / VVSFS_DENTRYSIZE);
        return 0;
    }
    // handling error
    return rm_entry_rtn;
}


/**
 * @brief // The `mkdir` operation for directory. It simply calls vvsfs_create, with the added flag of S_IFDIR
 * (signifying this is a directory).
 * @paragraph the time already updated in vvsfs_create
 * @param namespace
 * @param dir inode to the directory
 * @param dentry dentry of file to be deleted
 * @param mode ???
 * @return 0 on success, -errno on failure
 * @author Template code
 */
static int vvsfs_mkdir(struct user_namespace* namespace, struct inode* dir, struct dentry* dentry, umode_t mode) {
    return vvsfs_create(namespace, dir, dentry, mode | S_IFDIR, 0);
}

/**
 * @todo not sure if we need to modify for indriect blocks
 * @brief the 'rmdir' operatin for directory
 * @param inode parent directory removing
 * @param dent directory to be removed
 * @return 0 on success
 * @author Dai
 * @date 2023/10/16
 */
static int vvsfs_rmdir(struct inode* parent_dir, struct dentry* target_dentry) {
    _vvsfs_debug_print(__func__, "call rmdir");

    // chekc target_dentry is empty
    if(target_dentry->d_inode->i_size != 0) {
        return -ENOTEMPTY;
    }

    return vvsfs_unlink(parent_dir, target_dentry);
}

/**
 * @note may be not need for indirect blocks
 * @brief renaming fucntion for renaming directories and files
 * @param mnt_idmap
 * @param old_inode pointer to vfs inode of the directory containing renamming object
 * @param dentry the object to be renamed(linkage betreen filename and inode)
 * @param new_inode the new directory where you object is moved to
 * @param dentry the new dentry for the object(proving new name) (if exitst the inode will be give)
 * @param flags RENAME_NOREPLACE, RENAME_EXCHANGE
 * @paragraph RENAME_NO_REPLACE: if the new name exists, return an -EEXIST
 * @paragraph RENAME_EXCHANGE: if the new name exists, swap the two files(could be different type)
 * @paragraph try to find memory leaks, I am not sure about it
 * @return 0 on success and errno on failure
 * @author Dai
 * @date 14/10/2023
 */
static int vvsfs_rename(struct user_namespace* user_namespace, struct inode* old_inode, struct dentry* old_dentry,
                        struct inode* new_inode, struct dentry* new_dentry, unsigned int flags) {
    struct buffer_head *    old_bh, *new_bh;
    int                     block_old, block_new;
    int                     offset_old, offset_new;
    int                     new_entry_exists; // 0 if exists -1 if not
    int                     vvsfs_find_entry_rtn;
    int                     add_new_entry_rtn;
    int                     rm_entry_rtn;
    struct vvsfs_dir_entry* old_dir_entry_buf;
    struct vvsfs_dir_entry* new_dir_entry_buf;
    struct vvsfs_dir_entry  temp;
    if(DEBUG) {
        _vvsfs_debug_print(__func__, "enter");
        _vvsfs_debug_print(__func__, "moving %s in %s to %s in %s ", old_dentry->d_name.name, old_inode->i_sb->s_id,
                           new_dentry->d_name.name, new_inode->i_sb->s_id);
    }


    // check the flgs
    if(flags != 0 && flags != RENAME_NOREPLACE && flags != RENAME_EXCHANGE) {
        _vvsfs_debug_print(__func__, "vvsfs_rename - invalid flags");
        return -EINVAL;
    }
    // fidn the inode in original directory
    vvsfs_find_entry_rtn = vvsfs_find_entry(old_inode, old_dentry, &block_old, &offset_old);
    vvsfs_find_entry_erro(vvsfs_find_entry_rtn);
    // check if the new entry exists
    vvsfs_find_entry_rtn = vvsfs_find_entry(new_inode, new_dentry, &block_new, &offset_new);
    if(vvsfs_find_entry_rtn != 0 && vvsfs_find_entry_rtn != -1) {
        _vvsfs_debug_print(__func__, "error finding new entry %d", vvsfs_find_entry_rtn);
        return vvsfs_find_entry_rtn;
    }
    new_entry_exists = vvsfs_find_entry_rtn;
    if(vvsfs_find_entry_rtn == 0 && flags == RENAME_NOREPLACE) {
        _vvsfs_debug_print(__func__, "vvsfs_rename - new entry exists");
        return -EEXIST;
    }

    if(new_entry_exists == -1) {
        if(DEBUG)
            _vvsfs_debug_print(__func__, "vvsfs_rename - new entry does not exist");
        // just rename the entry if in same dir
        if(old_inode->i_ino == new_inode->i_ino) {
            vvsfs_rename_entry(old_inode, old_dentry, new_dentry->d_name.name);
            return 0;
        } else {
            // we are in differnt directory
            // add to new directory
            add_new_entry_rtn = vvsfs_add_new_entry(new_inode, new_dentry, old_inode);
            if(add_new_entry_rtn != 0) {
                _vvsfs_debug_print(__func__, "vvsfs_rename - add_new_entry_rtn errno %d", add_new_entry_rtn);
                return add_new_entry_rtn;
            }
            // remove old driectory
            rm_entry_rtn = vvsfs_delete_entry(old_inode, old_dentry);
            if(rm_entry_rtn != 0) {
                _vvsfs_debug_print(__func__, "vvsfs_rename - rm_entry_rtn errno %d", rm_entry_rtn);
                return rm_entry_rtn;
            }
            return 0;
        }
    } else if(new_entry_exists == 0 && flags == RENAME_EXCHANGE) {
        if(DEBUG)
            _vvsfs_debug_print(__func__, "vvsfs_rename - new entry exists and flags is exchange");
        // swap the two entries
        // read the two entries
        old_bh = sb_bread(old_inode->i_sb, vvsfs_get_data_block(old_inode->i_ino));
        if(!old_bh) {
            _vvsfs_debug_print(__func__, "vvsfs_rename - read target bh failed");
            return -ENOMEM;
        }
        new_bh = sb_bread(new_inode->i_sb, vvsfs_get_data_block(new_inode->i_ino));
        if(!new_bh) {
            _vvsfs_debug_print(__func__, "vvsfs_rename - read target bh failed");
            brelse(old_bh);
            return -ENOMEM;
        }
        // swap the two entries
        old_dir_entry_buf = (struct vvsfs_dir_entry*)(old_bh->b_data + offset_old * VVSFS_DENTRYSIZE);
        new_dir_entry_buf = (struct vvsfs_dir_entry*)(new_bh->b_data + offset_new * VVSFS_DENTRYSIZE);

        memcpy(&temp, old_dir_entry_buf, VVSFS_DENTRYSIZE);
        memcpy(old_dir_entry_buf, new_dir_entry_buf, VVSFS_DENTRYSIZE);
        memcpy(new_dir_entry_buf, &temp, VVSFS_DENTRYSIZE);
        // sync the buffer
        mark_buffer_dirty(old_bh);
        sync_dirty_buffer(old_bh);
        mark_buffer_dirty(new_bh);
        sync_dirty_buffer(new_bh);
        // release the buffer
        brelse(new_bh);
        brelse(old_bh);
        return 0;
    } else { // new file exists and flags is not exchange
        _vvsfs_debug_print(__func__, "vvsfs_rename - new entry exists");
        return -EEXIST;
    }
}


// File operations; leave as is. We are using the generic VFS implementations
// to take care of read/write/seek/fsync. The read/write operations rely on the
// address space operations, so there's no need to modify these.
static struct file_operations vvsfs_file_operations = {
    llseek : generic_file_llseek,
    fsync : generic_file_fsync,
    read_iter : generic_file_read_iter,
    write_iter : generic_file_write_iter,
};

static struct inode_operations vvsfs_file_inode_operations = {

};

static struct file_operations vvsfs_dir_operations = {
    .llseek = generic_file_llseek,
    .read   = generic_read_dir,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0)
    .iterate = vvsfs_readdir,
#else
    .iterate_shared = vvsfs_readdir,
#endif
    .fsync = generic_file_fsync,
};

static struct inode_operations vvsfs_dir_inode_operations = {
    create : vvsfs_create,
    lookup : vvsfs_lookup,
    mkdir : vvsfs_mkdir,
    unlink : vvsfs_unlink,
    rmdir : vvsfs_rmdir,
    rename : vvsfs_rename,
};

// This implements the super operation for writing a 'dirty' inode to disk
// Note that this does not sync the actual data blocks pointed to by the inode;
// it only saves the meta data (e.g., the data block pointers, but not the
// actual data contained in the data blocks). Data blocks sync is taken care of
// by file and directory operations.
static int vvsfs_write_inode(struct inode* inode, struct writeback_control* wbc) {
    struct super_block*      sb;
    struct vvsfs_inode*      disk_inode;
    struct vvsfs_inode_info* inode_info;
    struct buffer_head*      bh;
    uint32_t                 inode_block, inode_offset;
    int                      i;

    if(DEBUG)
        printk("vvsfs - write_inode");

    // get the vvsfs_inode_info associated with this (VFS) inode from cache.
    inode_info = VVSFS_I(inode);

    sb           = inode->i_sb;
    inode_block  = vvsfs_get_inode_block(inode->i_ino);
    inode_offset = vvsfs_get_inode_offset(inode->i_ino);

    printk("vvsfs - write_inode - ino: %ld, block: %d, offset: %d", inode->i_ino, inode_block, inode_offset);
    bh = sb_bread(sb, inode_block);
    if(!bh)
        return -EIO;

    disk_inode                      = (struct vvsfs_inode*)((uint8_t*)bh->b_data + inode_offset);
    disk_inode->i_mode              = inode->i_mode;
    disk_inode->i_size              = inode->i_size;
    disk_inode->i_data_blocks_count = inode_info->i_db_count;
    disk_inode->i_links_count       = inode->i_nlink;
    for(i = 0; i < VVSFS_N_BLOCKS; ++i)
        disk_inode->i_block[i] = inode_info->i_data[i];

    // TODO: if you have additional data added to the on-disk inode structure,
    // you need to sync it here.

    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);

    if(DEBUG)
        printk("vvsfs - write_inode done: %ld\n", inode->i_ino);
    return 0;
}

// This function is needed to initiate the inode cache, to allow us to attach
// filesystem specific inode information.
// You don't need to modify this.
int vvsfs_init_inode_cache(void) {
    if(DEBUG)
        printk("vvsfs - init inode cache ");

    vvsfs_inode_cache = kmem_cache_create("vvsfs_cache", sizeof(struct vvsfs_inode_info), 0, 0, NULL);
    if(!vvsfs_inode_cache)
        return -ENOMEM;
    return 0;
}

// De-allocate the inode cache
void vvsfs_destroy_inode_cache(void) {
    if(DEBUG)
        printk("vvsfs - destroy_inode_cache ");

    kmem_cache_destroy(vvsfs_inode_cache);
}

// This implements the super operation to allocate a new inode.
// This will be called everytime a request to allocate a
// new (in memory) inode is made. By default, this is handled by VFS,
// which allocates an VFS inode structure. But we override it here
// so that we can attach filesystem-specific information (.e.g,
// pointers to data blocks).
// It is unlikely that you will need to modify this function directly;
// you can simply add whatever additional information you want to attach
// to the vvsfs_inode_info structure.
static struct inode* vvsfs_alloc_inode(struct super_block* sb) {
    struct vvsfs_inode_info* c_inode = kmem_cache_alloc(vvsfs_inode_cache, GFP_KERNEL);

    if(DEBUG)
        printk("vvsfs - alloc_inode ");

    if(!c_inode)
        return NULL;

    inode_init_once(&c_inode->vfs_inode);
    return &c_inode->vfs_inode;
}

// Deallocate the inode cache.
static void vvsfs_destroy_inode(struct inode* inode) {
    struct vvsfs_inode_info* c_inode = container_of(inode, struct vvsfs_inode_info, vfs_inode);
    kmem_cache_free(vvsfs_inode_cache, c_inode);
}

// vvsfs_iget - get the inode from the super block
// This function will either return the inode that corresponds to a given inode
// number (ino), if it's already in the cache, or create a new inode object, if
// it's not in the cache. Note that this is very similar to vvsfs_new_inode,
// except that the requested inode is supposed to be allocated on-disk already.
// So don't use this to create a completely new inode that has not been
// allocated on disk.
struct inode* vvsfs_iget(struct super_block* sb, unsigned long ino) {
    struct inode*            inode;
    struct vvsfs_inode*      disk_inode;
    struct vvsfs_inode_info* inode_info;
    struct buffer_head*      bh;
    uint32_t                 inode_block;
    uint32_t                 inode_offset;
    int                      i;

    if(DEBUG) {
        printk("vvsfs - iget - ino : %d", (unsigned int)ino);
        printk(" super %p\n", sb);
    }

    inode = iget_locked(sb, ino);
    if(!inode)
        return ERR_PTR(-ENOMEM);
    if(!(inode->i_state & I_NEW))
        return inode;

    inode_info = VVSFS_I(inode);

    inode_block  = vvsfs_get_inode_block(ino);
    inode_offset = vvsfs_get_inode_offset(ino);

    bh = sb_bread(sb, inode_block);
    if(!bh) {
        printk("vvsfs - iget - failed sb_read");
        return ERR_PTR(-EIO);
    }

    disk_inode    = (struct vvsfs_inode*)(bh->b_data + inode_offset);
    inode->i_mode = disk_inode->i_mode;
    inode->i_size = disk_inode->i_size;

    // set the link count; note that we can't set inode->i_nlink directly; we
    // need to use the set_nlink function here.
    set_nlink(inode, disk_inode->i_links_count);
    inode->i_blocks = disk_inode->i_data_blocks_count * (VVSFS_BLOCKSIZE / VVSFS_SECTORSIZE);

    inode_info->i_db_count = disk_inode->i_data_blocks_count;
    /* store data blocks in cache */
    for(i = 0; i < VVSFS_N_BLOCKS; ++i)
        inode_info->i_data[i] = disk_inode->i_block[i];

    // Currently we just filled the various time information with the current
    // time, since we don't keep this information on disk. You will need to
    // change this if you save this information on disk.
    inode->i_ctime = inode->i_mtime = inode->i_atime = current_time(inode);

    if(S_ISDIR(inode->i_mode)) {
        inode->i_op  = &vvsfs_dir_inode_operations;
        inode->i_fop = &vvsfs_dir_operations;
    } else {
        inode->i_op             = &vvsfs_file_inode_operations;
        inode->i_fop            = &vvsfs_file_operations;
        inode->i_mapping->a_ops = &vvsfs_as_operations;
    }

    brelse(bh);

    unlock_new_inode(inode);

    return inode;
}

// put_super is part of the super operations. This
// is called when the filesystem is being unmounted, to deallocate
// memory space taken up by the super block info.
static void vvsfs_put_super(struct super_block* sb) {
    struct vvsfs_sb_info* sbi = sb->s_fs_info;

    if(DEBUG)
        printk("vvsfs - put_super\n");

    if(sbi) {
        kfree(sbi->imap);
        kfree(sbi->dmap);
        kfree(sbi);
    }
}
/**
 * @name vvsfs_count_free_block
 * @param map map of the correspoining block
 * @param size size of the map
 * @return number of block avaliable
 * @brief count the number of free block in the map(of size)
 */
static u64 vvsfs_count_free_block(uint8_t* map, uint32_t size) {
    int     i     = 0;
    uint8_t j     = 0;
    u64     count = 0;
    for(i = 0; i < size / 8; ++i) {
        for(j = 0; j < 8; ++j) {
            if(i == 0 && j == 0)
                continue; // skip block 0 -- it is reserved.
            if((~map[i]) & (0x80 >> j)) {
                count++;
            }
        }
    }
    return count;
}

// statfs -- this is currently incomplete.
// See https://elixir.bootlin.com/linux/v5.15.89/source/fs/ext2/super.c#L1407
// for various stats that you need to provide.
static int vvsfs_statfs(struct dentry* dentry, struct kstatfs* buf) {
    struct super_block*   sb     = dentry->d_sb;
    struct vvsfs_sb_info* sbi    = sb->s_fs_info;
    uint8_t*              f_imap = sbi->imap;
    uint8_t*              f_dmap = sbi->dmap;
    u64                   free_iblocks;
    u64                   free_dblocks;
    u64                   free_blocks;
    free_iblocks = vvsfs_count_free_block(f_imap, VVSFS_IMAP_SIZE);
    free_dblocks = vvsfs_count_free_block(f_dmap, VVSFS_DMAP_SIZE);
    free_blocks  = free_iblocks + free_dblocks;

    if(DEBUG)
        printk("vvsfs - statfs\n");

    buf->f_namelen = VVSFS_MAXNAME;
    buf->f_type    = VVSFS_MAGIC;
    buf->f_bsize   = VVSFS_BLOCKSIZE;
    buf->f_blocks  = VVSFS_DMAP_SIZE + VVSFS_IMAP_SIZE;
    buf->f_bfree   = free_blocks;                        // number of free blocks both inode and data
    buf->f_frsize  = free_iblocks;                       // number of free inodes
    buf->f_files   = VVSFS_IMAP_SIZE * 8 - free_iblocks; // number of inodes already allocated
    buf->f_ffree   = free_iblocks;                       // total number of unallocated inodes
    buf->f_bavail  = VVSFS_MAXBLOCKS - 1;                // total number of free block avaliable to a non-super user
    buf->f_frsize  = 0;                                  // fragement size(maybe not supproted)
    buf->f_flags   = 0;                                  // mount flags for the file system
    // TODO: fill in other information about the file system.
    return 0;
}

// Fill the super_block structure with information specific to vvsfs
static int vvsfs_fill_super(struct super_block* s, void* data, int silent) {
    struct inode*         root_inode;
    int                   hblock;
    struct buffer_head*   bh;
    uint32_t              magic;
    struct vvsfs_sb_info* sbi;

    if(DEBUG)
        printk("vvsfs - fill super\n");

    s->s_flags = ST_NOSUID | SB_NOEXEC;
    s->s_op    = &vvsfs_ops;
    s->s_magic = VVSFS_MAGIC;

    hblock = bdev_logical_block_size(s->s_bdev);
    if(hblock > VVSFS_BLOCKSIZE) {
        printk("vvsfs - device blocks are too small!!");
        return -1;
    }

    sb_set_blocksize(s, VVSFS_BLOCKSIZE);

    /* Read first block of the superblock.
        For this basic version, it contains just the magic number. */

    bh    = sb_bread(s, 0);
    magic = *((uint32_t*)bh->b_data);
    if(magic != VVSFS_MAGIC) {
        printk("vvsfs - wrong magic number\n");
        return -EINVAL;
    }
    brelse(bh);

    /* Allocate super block info to load inode & data map */
    sbi = kzalloc(sizeof(struct vvsfs_sb_info), GFP_KERNEL);
    if(!sbi) {
        printk("vvsfs - error allocating vvsfs_sb_info");
        return -ENOMEM;
    }

    /* Load the inode map */
    sbi->imap = kzalloc(VVSFS_IMAP_SIZE, GFP_KERNEL);
    if(!sbi->imap)
        return -ENOMEM;
    bh = sb_bread(s, 1);
    if(!bh)
        return -EIO;
    memcpy(sbi->imap, bh->b_data, VVSFS_IMAP_SIZE);
    brelse(bh);

    /* Load the data map. Note that the data map occupies 2 blocks.  */
    sbi->dmap = kzalloc(VVSFS_DMAP_SIZE, GFP_KERNEL);
    if(!sbi->dmap)
        return -ENOMEM;
    bh = sb_bread(s, 2);
    if(!bh)
        return -EIO;
    memcpy(sbi->dmap, bh->b_data, VVSFS_BLOCKSIZE);
    brelse(bh);
    bh = sb_bread(s, 3);
    if(!bh)
        return -EIO;
    memcpy(sbi->dmap + VVSFS_BLOCKSIZE, bh->b_data, VVSFS_BLOCKSIZE);
    brelse(bh);

    /* Attach the bitmaps to the in-memory super_block s */
    s->s_fs_info = sbi;

    /* Read the root inode from disk */
    root_inode = vvsfs_iget(s, 1);

    if(IS_ERR(root_inode)) {
        printk("vvsfs - fill_super - error getting root inode");
        return PTR_ERR(root_inode);
    }

    /* Initialise the owner */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
    inode_init_owner(&init_user_ns, root_inode, NULL, root_inode->i_mode);
#else
    inode_init_owner(&nop_mnt_idmap, root_inode, NULL, root_inode->i_mode);
#endif
    mark_inode_dirty(root_inode);

    s->s_root = d_make_root(root_inode);

    if(!s->s_root) {
        printk("vvsfs - fill_super - failed setting up root directory");
        iput(root_inode);
        return -ENOMEM;
    }

    if(DEBUG)
        printk("vvsfs - fill super done\n");

    return 0;
}

// sync_fs super operation.
// This writes super block data to disk.
// For the current version, this is mainly the inode map and the data map.
static int vvsfs_sync_fs(struct super_block* sb, int wait) {
    struct vvsfs_sb_info* sbi = sb->s_fs_info;
    struct buffer_head*   bh;

    if(DEBUG) {
        printk("vvsfs -- sync_fs");
    }

    /* Write the inode map to disk */
    bh = sb_bread(sb, 1);
    if(!bh)
        return -EIO;
    memcpy(bh->b_data, sbi->imap, VVSFS_IMAP_SIZE);
    mark_buffer_dirty(bh);
    if(wait)
        sync_dirty_buffer(bh);
    brelse(bh);

    /* Write the data map */

    bh = sb_bread(sb, 2);
    if(!bh)
        return -EIO;
    memcpy(bh->b_data, sbi->dmap, VVSFS_BLOCKSIZE);
    mark_buffer_dirty(bh);
    if(wait)
        sync_dirty_buffer(bh);
    brelse(bh);

    bh = sb_bread(sb, 3);
    if(!bh)
        return -EIO;
    memcpy(bh->b_data, sbi->dmap + VVSFS_BLOCKSIZE, VVSFS_BLOCKSIZE);
    mark_buffer_dirty(bh);
    if(wait)
        sync_dirty_buffer(bh);
    brelse(bh);

    return 0;
}

static struct super_operations vvsfs_ops = {
    statfs : vvsfs_statfs,
    put_super : vvsfs_put_super,
    alloc_inode : vvsfs_alloc_inode,
    destroy_inode : vvsfs_destroy_inode,
    write_inode : vvsfs_write_inode,
    sync_fs : vvsfs_sync_fs,
};

// mounting the file system -- leave as is.
static struct dentry* vvsfs_mount(struct file_system_type* fs_type, int flags, const char* dev_name, void* data) {
    return mount_bdev(fs_type, flags, dev_name, data, vvsfs_fill_super);
}

static struct file_system_type vvsfs_type = {
    .owner    = THIS_MODULE,
    .name     = "vvsfs",
    .mount    = vvsfs_mount,
    .kill_sb  = kill_block_super,
    .fs_flags = FS_REQUIRES_DEV,
};

static int __init vvsfs_init(void) {
    int ret = vvsfs_init_inode_cache();
    if(ret) {
        printk("inode cache creation failed");
        return ret;
    }

    printk("Registering vvsfs\n");
    return register_filesystem(&vvsfs_type);
}

static void __exit vvsfs_exit(void) {
    printk("Unregistering the vvsfs.\n");
    unregister_filesystem(&vvsfs_type);
    vvsfs_destroy_inode_cache();
}

module_init(vvsfs_init);
module_exit(vvsfs_exit);
MODULE_LICENSE("GPL");
