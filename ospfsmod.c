#include <linux/autoconf.h>
#include <linux/version.h>
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#include <linux/module.h>
#include <linux/moduleparam.h>
#include "ospfs.h"
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <asm/uaccess.h>
#include <linux/kernel.h>
#include <linux/sched.h>

/****************************************************************************
 * ospfsmod
 *
 *   This is the OSPFS module!  It contains both library code for your use,
 *   and exercises where you must add code.
 *
 ****************************************************************************/

/* Define eprintk() to be a version of printk(), which prints messages to
 * the console.
 * (If working on a real Linux machine, change KERN_NOTICE to KERN_ALERT or
 * KERN_EMERG so that you are sure to see the messages.  By default, the
 * kernel does not print all messages to the console.  Levels like KERN_ALERT
 * and KERN_EMERG will make sure that you will see messages.) */
#define eprintk(format, ...) printk(KERN_NOTICE format, ## __VA_ARGS__)

// The actual disk data is just an array of raw memory.
// The initial array is defined in fsimg.c, based on your 'base' directory.
extern uint8_t ospfs_data[];
extern uint32_t ospfs_length;

// A pointer to the superblock; see ospfs.h for details on the struct.
static ospfs_super_t * const ospfs_super =
	(ospfs_super_t *) &ospfs_data[OSPFS_BLKSIZE];

static int change_size(ospfs_inode_t *oi, uint32_t want_size);
static ospfs_direntry_t *find_direntry(ospfs_inode_t *dir_oi, const char *name, int namelen);


/*****************************************************************************
 * FILE SYSTEM OPERATIONS STRUCTURES
 *
 *   Linux filesystems are based around three interrelated structures.
 *
 *   These are:
 *
 *   1. THE LINUX SUPERBLOCK.  This structure represents the whole file system.
 *      Example members include the root directory and the number of blocks
 *      on the disk.
 *   2. LINUX INODES.  Each file and directory in the file system corresponds
 *      to an inode.  Inode operations include "mkdir" and "create" (add to
 *      directory).
 *   3. LINUX FILES.  Corresponds to an open file or directory.  Operations
 *      include "read", "write", and "readdir".
 *
 *   When Linux wants to perform some file system operation,
 *   it calls a function pointer provided by the file system type.
 *   (Thus, Linux file systems are object oriented!)
 *
 *   These function pointers are grouped into structures called "operations"
 *   structures.
 *
 *   The initial portion of the file declares all the operations structures we
 *   need to support ospfsmod: one for the superblock, several for different
 *   kinds of inodes and files.  There are separate inode_operations and
 *   file_operations structures for OSPFS directories and for regular OSPFS
 *   files.  The structures are actually defined near the bottom of this file.
 */

// Basic file system type structure
// (links into Linux's list of file systems it supports)
static struct file_system_type ospfs_fs_type;
// Inode and file operations for regular files
static struct inode_operations ospfs_reg_inode_ops;
static struct file_operations ospfs_reg_file_ops;
// Inode and file operations for directories
static struct inode_operations ospfs_dir_inode_ops;
static struct file_operations ospfs_dir_file_ops;
// Inode operations for symbolic links
static struct inode_operations ospfs_symlink_inode_ops;
// Other required operations
static struct dentry_operations ospfs_dentry_ops;
static struct super_operations ospfs_superblock_ops;



/*****************************************************************************
 * BITVECTOR OPERATIONS
 *
 *   OSPFS uses a free bitmap to keep track of free blocks.
 *   These bitvector operations, which set, clear, and test individual bits
 *   in a bitmap, may be useful.
 */

// bitvector_set -- Set 'i'th bit of 'vector' to 1.
static inline void
bitvector_set(void *vector, int i)
{
	((uint32_t *) vector) [i / 32] |= (1 << (i % 32));
}

// bitvector_clear -- Set 'i'th bit of 'vector' to 0.
static inline void
bitvector_clear(void *vector, int i)
{
	((uint32_t *) vector) [i / 32] &= ~(1 << (i % 32));
}

// bitvector_test -- Return the value of the 'i'th bit of 'vector'.
static inline int
bitvector_test(const void *vector, int i)
{
	return (((const uint32_t *) vector) [i / 32] & (1 << (i % 32))) != 0;
}



/*****************************************************************************
 * OSPFS HELPER FUNCTIONS
 */

// ospfs_size2nblocks(size)
//	Returns the number of blocks required to hold 'size' bytes of data.
//
//   Input:   size -- file size
//   Returns: a number of blocks

uint32_t
ospfs_size2nblocks(uint32_t size)
{
	return (size + OSPFS_BLKSIZE - 1) / OSPFS_BLKSIZE;
}


// ospfs_block(blockno)
//	Use this function to load a block's contents from "disk".
//
//   Input:   blockno -- block number
//   Returns: a pointer to that block's data

static void *
ospfs_block(uint32_t blockno)
{
	return &ospfs_data[blockno * OSPFS_BLKSIZE];
}


// ospfs_inode(ino)
//	Use this function to load a 'ospfs_inode' structure from "disk".
//
//   Input:   ino -- inode number
//   Returns: a pointer to the corresponding ospfs_inode structure

static inline ospfs_inode_t *
ospfs_inode(ino_t ino)
{
	ospfs_inode_t *oi;
	if (ino >= ospfs_super->os_ninodes)
		return 0;
	oi = ospfs_block(ospfs_super->os_firstinob);
	return &oi[ino];
}


// ospfs_inode_blockno(oi, offset)
//	Use this function to look up the blocks that are part of a file's
//	contents.
//
//   Inputs:  oi     -- pointer to a OSPFS inode
//	      offset -- byte offset into that inode
//   Returns: the block number of the block that contains the 'offset'th byte
//	      of the file

static inline uint32_t
ospfs_inode_blockno(ospfs_inode_t *oi, uint32_t offset)
{
	uint32_t blockno = offset / OSPFS_BLKSIZE;
	if (offset >= oi->oi_size || oi->oi_ftype == OSPFS_FTYPE_SYMLINK)
		return 0;
	else if (blockno >= OSPFS_NDIRECT + OSPFS_NINDIRECT) {
		uint32_t blockoff = blockno - (OSPFS_NDIRECT + OSPFS_NINDIRECT);
		uint32_t *indirect2_block = ospfs_block(oi->oi_indirect2);
		uint32_t *indirect_block = ospfs_block(indirect2_block[blockoff / OSPFS_NINDIRECT]);
		return indirect_block[blockoff % OSPFS_NINDIRECT];
	} else if (blockno >= OSPFS_NDIRECT) {
		uint32_t *indirect_block = ospfs_block(oi->oi_indirect);
		return indirect_block[blockno - OSPFS_NDIRECT];
	} else
		return oi->oi_direct[blockno];
}


// ospfs_inode_data(oi, offset)
//	Use this function to load part of inode's data from "disk",
//	where 'offset' is relative to the first byte of inode data.
//
//   Inputs:  oi     -- pointer to a OSPFS inode
//	      offset -- byte offset into 'oi's data contents
//   Returns: a pointer to the 'offset'th byte of 'oi's data contents
//
//	Be careful: the returned pointer is only valid within a single block.
//	This function is a simple combination of 'ospfs_inode_blockno'
//	and 'ospfs_block'.

static inline void *
ospfs_inode_data(ospfs_inode_t *oi, uint32_t offset)
{
	uint32_t blockno = ospfs_inode_blockno(oi, offset);
	return (uint8_t *) ospfs_block(blockno) + (offset % OSPFS_BLKSIZE);
}

static inline uint32_t
find_free_inode(void)
{
	uint32_t inode_no;
	ospfs_inode_t *symlink_ino;
	
	// Determine what inode we can use
	// Start at 2 since the first two inodes are special
	// Inode number 1 is the inode for the root directory of the file system.
	// Inode number 0 is reserved and must never be used. 
	for (inode_no = 2; inode_no < ospfs_super->os_ninodes; inode_no++) {
	symlink_ino = ospfs_inode(inode_no); //load inode structure corresponding to inode number from disk
	
	if (symlink_ino->oi_nlink == 0) //inode is free if link count reaches 0 => no hard links
	return inode_no;
	}
	//no free
	return 0;
}


/*****************************************************************************
 * LOW-LEVEL FILE SYSTEM FUNCTIONS
 * There are no exercises in this section, and you don't need to understand
 * the code.
 */

// ospfs_mk_linux_inode(sb, ino)
//	Linux's in-memory 'struct inode' structure represents disk
//	objects (files and directories).  Many file systems have their own
//	notion of inodes on disk, and for such file systems, Linux's
//	'struct inode's are like a cache of on-disk inodes.
//
//	This function takes an inode number for the OSPFS and constructs
//	and returns the corresponding Linux 'struct inode'.
//
//   Inputs:  sb  -- the relevant Linux super_block structure (one per mount)
//	      ino -- OSPFS inode number
//   Returns: 'struct inode'

static struct inode *
ospfs_mk_linux_inode(struct super_block *sb, ino_t ino)
{
	ospfs_inode_t *oi = ospfs_inode(ino);
	struct inode *inode;

	if (!oi)
		return 0;
	if (!(inode = new_inode(sb)))
		return 0;

	inode->i_ino = ino;
	// Make it look like everything was created by root.
	inode->i_uid = inode->i_gid = 0;
	inode->i_size = oi->oi_size;

	if (oi->oi_ftype == OSPFS_FTYPE_REG) {
		// Make an inode for a regular file.
		inode->i_mode = oi->oi_mode | S_IFREG;
		inode->i_op = &ospfs_reg_inode_ops;
		inode->i_fop = &ospfs_reg_file_ops;
		inode->i_nlink = oi->oi_nlink;

	} else if (oi->oi_ftype == OSPFS_FTYPE_DIR) {
		// Make an inode for a directory.
		inode->i_mode = oi->oi_mode | S_IFDIR;
		inode->i_op = &ospfs_dir_inode_ops;
		inode->i_fop = &ospfs_dir_file_ops;
		inode->i_nlink = oi->oi_nlink + 1 /* dot-dot */;

	} else if (oi->oi_ftype == OSPFS_FTYPE_SYMLINK) {
		// Make an inode for a symbolic link.
		inode->i_mode = S_IRUSR | S_IRGRP | S_IROTH
			| S_IWUSR | S_IWGRP | S_IWOTH
			| S_IXUSR | S_IXGRP | S_IXOTH | S_IFLNK;
		inode->i_op = &ospfs_symlink_inode_ops;
		inode->i_nlink = oi->oi_nlink;

	} else
		panic("OSPFS: unknown inode type!");

	// Access and modification times are now.
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	return inode;
}


// ospfs_fill_super, ospfs_get_sb
//	These functions are called by Linux when the user mounts a version of
//	the OSPFS onto some directory.  They help construct a Linux
//	'struct super_block' for that file system.

static int
ospfs_fill_super(struct super_block *sb, void *data, int flags)
{
	struct inode *root_inode;

	sb->s_blocksize = OSPFS_BLKSIZE;
	sb->s_blocksize_bits = OSPFS_BLKSIZE_BITS;
	sb->s_magic = OSPFS_MAGIC;
	sb->s_op = &ospfs_superblock_ops;

	if (!(root_inode = ospfs_mk_linux_inode(sb, OSPFS_ROOT_INO))
	    || !(sb->s_root = d_alloc_root(root_inode))) {
		iput(root_inode);
		sb->s_dev = 0;
		return -ENOMEM;
	}

	return 0;
}

static int
ospfs_get_sb(struct file_system_type *fs_type, int flags, const char *dev_name, void *data, struct vfsmount *mount)
{
	return get_sb_single(fs_type, flags, data, ospfs_fill_super, mount);
}


// ospfs_delete_dentry
//	Another bookkeeping function.

static int
ospfs_delete_dentry(struct dentry *dentry)
{
	return 1;
}


/*****************************************************************************
 * DIRECTORY OPERATIONS
 *
 * COMPLETED EXERCISE: Finish 'ospfs_dir_readdir' and 'ospfs_symlink'.
 */

// ospfs_dir_lookup(dir, dentry, ignore)
//	This function implements the "lookup" directory operation, which
//	looks up a named entry.
//
//	We have written this function for you.
//
//   Input:  dir    -- The Linux 'struct inode' for the directory.
//		       You can extract the corresponding 'ospfs_inode_t'
//		       by calling 'ospfs_inode' with the relevant inode number.
//	     dentry -- The name of the entry being looked up.
//   Effect: Looks up the entry named 'dentry'.  If found, attaches the
//	     entry's 'struct inode' to the 'dentry'.  If not found, returns
//	     a "negative dentry", which has no inode attachment.

static struct dentry *
ospfs_dir_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *ignore)
{
	// Find the OSPFS inode corresponding to 'dir'
	ospfs_inode_t *dir_oi = ospfs_inode(dir->i_ino);
	struct inode *entry_inode = NULL;
	int entry_off;

	// Make sure filename is not too long
	if (dentry->d_name.len > OSPFS_MAXNAMELEN)
		return (struct dentry *) ERR_PTR(-ENAMETOOLONG);

	// Mark with our operations
	dentry->d_op = &ospfs_dentry_ops;

	// Search through the directory block
	for (entry_off = 0; entry_off < dir_oi->oi_size;
	     entry_off += OSPFS_DIRENTRY_SIZE) {
		// Find the OSPFS inode for the entry
		ospfs_direntry_t *od = ospfs_inode_data(dir_oi, entry_off);

		// Set 'entry_inode' if we find the file we are looking for
		if (od->od_ino > 0
		    && strlen(od->od_name) == dentry->d_name.len
		    && memcmp(od->od_name, dentry->d_name.name, dentry->d_name.len) == 0) {
			entry_inode = ospfs_mk_linux_inode(dir->i_sb, od->od_ino);
			if (!entry_inode)
				return (struct dentry *) ERR_PTR(-EINVAL);
			break;
		}
	}

	// We return a dentry whether or not the file existed.
	// The file exists if and only if 'entry_inode != NULL'.
	// If the file doesn't exist, the dentry is called a "negative dentry".

	// d_splice_alias() attaches the inode to the dentry.
	// If it returns a new dentry, we need to set its operations.
	if ((dentry = d_splice_alias(entry_inode, dentry)))
		dentry->d_op = &ospfs_dentry_ops;
	return dentry;
}


// ospfs_dir_readdir(filp, dirent, filldir)
//   This function is called when the kernel reads the contents of a directory
//   (i.e. when file_operations.readdir is called for the inode).
//
//   Inputs:  filp	-- The 'struct file' structure correspoding to
//			   the open directory.
//			   The most important member is 'filp->f_pos', the
//			   File POSition.  This remembers how far into the
//			   directory we are, so if the user calls 'readdir'
//			   twice, we don't forget our position.
//			   This function must update 'filp->f_pos'.
//	      dirent	-- Used to pass to 'filldir'.
//	      filldir	-- A pointer to a callback function.
//			   This function should call 'filldir' once for each
//			   directory entry, passing it six arguments:
//		  (1) 'dirent'.
//		  (2) The directory entry's name.
//		  (3) The length of the directory entry's name.
//		  (4) The 'f_pos' value corresponding to the directory entry.
//		  (5) The directory entry's inode number.
//		  (6) DT_REG, for regular files; DT_DIR, for subdirectories;
//		      or DT_LNK, for symbolic links.
//			   This function should stop returning directory
//			   entries either when the directory is complete, or
//			   when 'filldir' returns < 0, whichever comes first.
//
//   Returns: 1 at end of directory, 0 if filldir returns < 0 before the end
//     of the directory, and -(error number) on error.
//
//   EXERCISE(x): Finish implementing this function.

static int
ospfs_dir_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct inode *dir_inode = filp->f_dentry->d_inode;
	ospfs_inode_t *dir_oi = ospfs_inode(dir_inode->i_ino);
	uint32_t f_pos = filp->f_pos;
	int r = 0;		/* Error return value, if any */
	int ok_so_far = 0;	/* Return value from 'filldir' */

	uint32_t file_offset = 0; /* The offset in the inode */
	int file_type; /* Looked-up value for each directory entry's filetype */

	// f_pos is an offset into the directory's data, plus two.
	// The "plus two" is to account for "." and "..".
	if (r == 0 && f_pos == 0) {
		ok_so_far = filldir(dirent, ".", 1, f_pos, dir_inode->i_ino, DT_DIR);
		if (ok_so_far >= 0)
			f_pos++;
	}

	if (r == 0 && ok_so_far >= 0 && f_pos == 1) {
		ok_so_far = filldir(dirent, "..", 2, f_pos, filp->f_dentry->d_parent->d_inode->i_ino, DT_DIR);
		if (ok_so_far >= 0)
			f_pos++;
	}

	// actual entries
	while (r == 0 && ok_so_far >= 0 && f_pos >= 2) {
		ospfs_direntry_t *od;
		ospfs_inode_t *entry_oi;

		// If at the end of the directory, return 1
		
		file_offset = (f_pos - 2) * OSPFS_DIRENTRY_SIZE;
		if (file_offset >= dir_oi->oi_size)
		{
			r = 1;
			filp->f_pos = f_pos;
			return 1;
		}

		/* Get a pointer to the next entry (od) in the directory.
		 * The file system interprets the contents of a
		 * directory-file as a sequence of ospfs_direntry structures.
		 * You will find 'f_pos' and 'ospfs_inode_data' useful.
		 *
		 * Then use the fields of that file to fill in the directory
		 * entry.  To figure out whether a file is a regular file or
		 * another directory, use 'ospfs_inode' to get the directory
		 * entry's corresponding inode, and check out its 'oi_ftype'
		 * member.
		 *
		 * Make sure you ignore blank directory entries!  (Which have
		 * an inode number of 0.)
		 *
		 * If the current entry is successfully read (the call to
		 * filldir returns >= 0), or the current entry is skipped,
		 * your function should advance f_pos by the proper amount to
		 * advance to the next directory entry.
		 */

		// Get the appropriate directory entry from offset
		od = ospfs_inode_data(dir_oi, file_offset);
		entry_oi = ospfs_inode(od->od_ino);

		if (entry_oi == NULL || od->od_ino == 0) //move on if we find empty result
		{
		       f_pos++;
		       continue;
		}

		 // Lookup the filetype of the file for filldir
		 switch (entry_oi->oi_ftype)
		 {
			//Regular file
		 	case OSPFS_FTYPE_REG: 
		 		file_type = DT_REG;
		 		break;
			// Directory
		 	case OSPFS_FTYPE_DIR: 	
		 		file_type = DT_DIR;
		 		break;
			// Symbolic link
		 	case OSPFS_FTYPE_SYMLINK: 
		 		file_type = DT_LNK;
		 		break;

		 	default:	//we don;t know what type
		 		r = -EIO;
		 		continue;
		 		break;
		 }

		 // call the call back function
		ok_so_far = filldir(dirent, od->od_name, strlen(od->od_name), f_pos, od->od_ino, file_type);
		f_pos++;
	}

	// Save the file position and return!
	filp->f_pos = f_pos;
	return r;
}


// ospfs_unlink(dirino, dentry)
//   This function is called to remove a file.
//
//   Inputs: dirino  -- You may ignore this.
//           dentry  -- The 'struct dentry' structure, which contains the inode
//                      the directory entry points to and the directory entry's
//                      directory.
//
//   Returns: 0 if success and -ENOENT on entry not found.
//
//   COMPLETED EXERCISE: Make sure that deleting symbolic links works correctly.

static int
ospfs_unlink(struct inode *dirino, struct dentry *dentry)
{
	ospfs_inode_t *oi = ospfs_inode(dentry->d_inode->i_ino);
	ospfs_inode_t *dir_oi = ospfs_inode(dentry->d_parent->d_inode->i_ino);
	int entry_off;
	ospfs_direntry_t *od;

	od = NULL; // silence compiler warning; entry_off indicates when !od
	for (entry_off = 0; entry_off < dir_oi->oi_size;
	     entry_off += OSPFS_DIRENTRY_SIZE) {
		od = ospfs_inode_data(dir_oi, entry_off);
		if (od->od_ino > 0
		    && strlen(od->od_name) == dentry->d_name.len
		    && memcmp(od->od_name, dentry->d_name.name, dentry->d_name.len) == 0)
			break;
	}

	if (entry_off == dir_oi->oi_size) {
		printk("<1>ospfs_unlink should not fail!\n");
		return -ENOENT;
	}

	od->od_ino = 0;
	oi->oi_nlink--;

	//lower the link count of parent dir
	dir_oi->oi_nlink--;

	//no more link and if file type is not symbolic
	if (oi->oi_nlink == 0 && oi->oi_ftype != OSPFS_FTYPE_SYMLINK)
		change_size(oi, 0);

	return 0;
}



/*****************************************************************************
 * FREE-BLOCK BITMAP OPERATIONS
 *
 * COMPLETED EXERCISE: Implement these functions.
 */

// allocate_block()
//	Use this function to allocate a block.
//
//   Inputs:  none
//   Returns: block number of the allocated block,
//	      or 0 if the disk is full
//
//   This function searches the free-block bitmap, which starts at Block 2, for
//   a free block, allocates it (by marking it non-free), and returns the block
//   number to the caller.  The block itself is not touched.
//
//   Note:  A value of 0 for a bit indicates the corresponding block is
//      allocated; a value of 1 indicates the corresponding block is free.
//
//   You can use the functions bitvector_set(), bitvector_clear(), and
//   bitvector_test() to do bit operations on the map.

static uint32_t
allocate_block(void)
{
	void *freemap = ospfs_block(OSPFS_FREEMAP_BLK);
	uint32_t i;
	uint32_t r = 0;

	for(i = OSPFS_FREEMAP_BLK; i < ospfs_super->os_nblocks; i++)
	{
		if(bitvector_test(freemap, i))
		{
			bitvector_clear(freemap, i);
			r = i;
			break;
		}
	}
	return r;
}


// free_block(blockno)
//	Use this function to free an allocated block.
//
//   Inputs:  blockno -- the block number to be freed
//   Returns: none
//
//   This function should mark the named block as free in the free-block
//   bitmap.  (You might want to program defensively and make sure the block
//   number isn't obviously bogus: the boot sector, superblock, free-block
//   bitmap, and inode blocks must never be freed.  But this is not required.)

static void
free_block(uint32_t blockno)
{
	// We can't free any block after the last inode block
	uint32_t last_inode_block = ospfs_super->os_firstinob + OSPFS_BLKINODES;
	void *freemap = ospfs_block(OSPFS_FREEMAP_BLK);

	//sanity check
	if(blockno >= ospfs_super->os_ninodes || blockno <= last_inode_block)
		return;

	// Free the block
	bitvector_set(freemap, blockno);
}


/*****************************************************************************
 * FILE OPERATIONS
 *
 * COMPLETED EXERCISE: Finish off change_size, read, and write.
 *
 * The find_*, add_block, and remove_block functions are only there to support
 * the change_size function.  If you prefer to code change_size a different
 * way, then you may not need these functions.
 *
 */

// The following functions are used in our code to unpack a block number into
// its consituent pieces: the doubly indirect block number (if any), the
// indirect block number (which might be one of many in the doubly indirect
// block), and the direct block number (which might be one of many in an
// indirect block).  We use these functions in our implementation of
// change_size.


// int32_t indir2_index(uint32_t b)
//	Returns the doubly-indirect block index for file block b.
//
// Inputs:  b -- the zero-based index of the file block (e.g., 0 for the first
//		 block, 1 for the second, etc.)
// Returns: 0 if block index 'b' requires using the doubly indirect
//	       block, -1 if it does not.
//
// COMPLETED EXERCISE: Fill in this function.

static int32_t
indir2_index(uint32_t b)
{
	if(b < OSPFS_NDIRECT + OSPFS_NINDIRECT)
		return -1;

	return 0;
}


// int32_t indir_index(uint32_t b)
//	Returns the indirect block index for file block b.
//
// Inputs:  b -- the zero-based index of the file block
// Returns: -1 if b is one of the file's direct blocks;
//	    0 if b is located under the file's first indirect block;
//	    otherwise, the offset of the relevant indirect block within
//		the doubly indirect block.
//
// COMPLETED EXERCISE: Fill in this function.

static int32_t
indir_index(uint32_t b)
{
	//if in direct area
	if(b < OSPFS_NDIRECT)
		return -1;

	//no need for double but need for indir
	if(indir2_index(b) == -1)
		return 0;

	//need doubly indirect block
	b -= OSPFS_NDIRECT + OSPFS_NINDIRECT;
	return b / OSPFS_NINDIRECT;
}


// int32_t indir_index(uint32_t b)
//	Returns the indirect block index for file block b.
//
// Inputs:  b -- the zero-based index of the file block
// Returns: the index of block b in the relevant indirect block or the direct
//	    block array.
//
// COMPLETED EXERCISE: Fill in this function.

static int32_t
direct_index(uint32_t b)
{
	if(b < OSPFS_NDIRECT)
		return b;

	b -= OSPFS_NDIRECT;
	return b % OSPFS_NINDIRECT;
}


// add_block(ospfs_inode_t *oi)
//   Adds a single data block to a file, adding indirect and
//   doubly-indirect blocks if necessary. (Helper function for
//   change_size).
//
// Inputs: oi -- pointer to the file we want to grow
// Returns: 0 if successful, < 0 on error.  Specifically:
//          -ENOSPC if you are unable to allocate a block
//          due to the disk being full or
//          -EIO for any other error.
//          If the function is successful, then oi->oi_size
//          should be set to the maximum file size in bytes that could
//          fit in oi's data blocks.  If the function returns an error,
//          then oi->oi_size should remain unchanged. Any newly
//          allocated blocks should be erased (set to zero).
//
// COMPLETED EXERCISE: Finish off this function.
//
// Remember that allocating a new data block may require allocating
// as many as three disk blocks, depending on whether a new indirect
// block and/or a new indirect^2 block is required. If the function
// fails with -ENOSPC or -EIO, then you need to make sure that you
// free any indirect (or indirect^2) blocks you may have allocated!
//
// Also, make sure you:
//  1) zero out any new blocks that you allocate
//  2) store the disk block number of any newly allocated block
//     in the appropriate place in the inode or one of the
//     indirect blocks.
//  3) update the oi->oi_size field

static int
add_block(ospfs_inode_t *oi)
{
	// current number of blocks in file
	uint32_t n = ospfs_size2nblocks(oi->oi_size);

	// keep track of allocations to free in case of -ENOSPC
	uint32_t allocated[2] = { 0, 0 };

	int32_t index_indir2;
	int32_t index_indir;
	int32_t index_direct;

	uint32_t indir_block = 0;
	uint32_t double_indir_block = 0;
	//uint32_t data_block;
	
	uint32_t *indir_data = NULL;
	uint32_t *double_indir_data = NULL;

	const int INDIR_2_CONST = 0;
	const int INDIR_1_CONST = 1;
	int r = -ENOSPC;

	/* COMPLETED EXERCISE: Your code here */

	if(n == OSPFS_MAXFILEBLKS)
		return -ENOSPC;

	index_indir2 = indir2_index(n);
	index_indir  = indir_index(n);
	index_direct = direct_index(n);

	
	//eprintk("index_indir2 is :%d\n",index_indir2 );
	//eprintk("index_indir  is :%d\n",index_indir );
	//eprintk("index_direct is :%d\n",index_direct);
	
	if(index_indir == -1) //data block in inode->direct
	{
		// Sanity check
		if(oi->oi_direct[index_direct] != 0)
			return -EIO;

		if((allocated[0] = allocate_block()) == 0)
		{
			free_block(allocated[0]);
			return -ENOSPC;
		}
		memset(ospfs_block(allocated[0]), 0, OSPFS_BLKSIZE);
		oi->oi_direct[index_direct] = allocated[0];
		eprintk("allocating direct block of index %d, using block:%d.\n",index_direct,allocated[0]);
		oi->oi_size = (n + 1) * OSPFS_BLKSIZE;
		return 0;
	}
	//indir mechanisms
	if(index_indir2 == 0) // If we need the double indirect block
	{
		if(oi->oi_indirect2 == 0) // and it is null, allocate it
		{
			if((allocated[INDIR_2_CONST] = allocate_block()) == 0)
				goto ERR_HANDLE;

			double_indir_block = allocated[INDIR_2_CONST];
			double_indir_data = ospfs_block(double_indir_block);
			//eprintk("allocating indir2 ptr, using block:%d.\n",double_indir_block);
			//zero out
			memset(double_indir_data, 0, OSPFS_BLKSIZE);
		}
		else // otherwise use the existing indirect2
		{
			double_indir_block = oi->oi_indirect2;
			//eprintk("using existed indir2 ptr, using block:%d.\n",double_indir_block);
			double_indir_data = ospfs_block(double_indir_block);
		}

		// Allocate the indirect block if needed
		if(double_indir_data[index_indir] == 0)
		{
			if((allocated[INDIR_1_CONST] = allocate_block()) == 0)
				goto ERR_HANDLE;

			indir_block = allocated[INDIR_1_CONST];
			indir_data = ospfs_block(indir_block);
			//eprintk("allocating dir2->indir ptr, using block:%d.\n",indir_block);
			//zero out
			memset(indir_data, 0, OSPFS_BLKSIZE);
		}
		
		if((indir_data[index_direct] = allocate_block()) == 0)
			goto ERR_HANDLE;
		memset(ospfs_block(indir_data[index_direct]), 0, OSPFS_BLKSIZE);
		//update size linkage
		oi->oi_size = (n + 1) * OSPFS_BLKSIZE;
		
		if(oi->oi_indirect2 == 0)
			oi->oi_indirect2 = double_indir_block;

		if(double_indir_data[index_indir] == 0)
			double_indir_data[index_indir] = indir_block;
			
		eprintk("allocating indirect2 block using block:%d.\n",double_indir_block);
		eprintk("allocating direct block using block:%d.\n",indir_data[index_direct]);
	}

	// not using dir2 nor do we have enough direct
	// we use the indirect block
	else
	{
		//if indir not created yet
		if(oi->oi_indirect == 0)
		{
			if((oi->oi_indirect = allocate_block()) == 0)
				goto ERR_HANDLE;
			eprintk("allocating indirect1 block using block:%d.\n",oi->oi_indirect);
			indir_block = oi->oi_indirect;
			memset(ospfs_block(indir_block), 0, OSPFS_BLKSIZE);
		}
		else//already have indir
			indir_block = oi->oi_indirect;
			
		//indir_block = allocated[INDIR_1_CONST];
		indir_data = ospfs_block(indir_block);
		//zero ou
		//adding
		if((indir_data[index_direct] = allocate_block()) == 0)
			goto ERR_HANDLE;
		memset(ospfs_block(indir_data[index_direct]), 0, OSPFS_BLKSIZE);
		//update size linkage
		oi->oi_size = (n + 1) * OSPFS_BLKSIZE;
		
		eprintk("allocating direct under indirect: %d, data block using block:%d.\n",oi->oi_indirect,indir_data[index_direct]);
	}

	// Sanity check that there is no existing allocated block
	/*if(indir_data[index_direct] != 0)
	{
		r = -EIO;
		goto ERR_HANDLE;
	}
	*/
	// Allocate the actual block
	//if((indir_data[index_direct] = allocate_block()) == 0)
	//	goto ERR_HANDLE;

	//zero out
	//memset(ospfs_block(indir_data[index_direct]), 0, OSPFS_BLKSIZE);

	//update size
	//oi->oi_size = (n + 1) * OSPFS_BLKSIZE;

	//update linkage
	//doubly
	/*if(index_indir2 == 0)
	{
		if(oi->oi_indirect2 == 0)
			oi->oi_indirect2 = double_indir_block;

		if(double_indir_data[index_indir] == 0)
			double_indir_data[index_indir] = indir_block;
			
		eprintk("allocating indirect2 block using block:%d.\n",double_indir_block);
	}
	//singly
	else if(oi->oi_indirect == 0)
	{
		oi->oi_indirect = indir_block;
		eprintk("allocating indirect1 block using block:%d.\n",indir_block);
	}
	eprintk("allocating direct block using block:%d.\n",indir_data[index_direct]);
	*/
	return 0;

	ERR_HANDLE:
		if(allocated[INDIR_2_CONST] != 0)
			free_block(allocated[INDIR_2_CONST]);

		if(allocated[INDIR_1_CONST] != 0)
			free_block(allocated[INDIR_1_CONST]);

		return r;
}


// remove_block(ospfs_inode_t *oi)
//   Removes a single data block from the end of a file, freeing
//   any indirect and indirect^2 blocks that are no
//   longer needed. (Helper function for change_size)
//
// Inputs: oi -- pointer to the file we want to shrink
// Returns: 0 if successful, < 0 on error.
//          If the function is successful, then oi->oi_size
//          should be set to the maximum file size that could
//          fit in oi's blocks.  If the function returns -EIO (for
//          instance if an indirect block that should be there isn't),
//          then oi->oi_size should remain unchanged.
//
// COMPLETED EXERCISE: Finish off this function.
//
// Remember that you must free any indirect and doubly-indirect blocks
// that are no longer necessary after shrinking the file.  Removing a
// single data block could result in as many as 3 disk blocks being
// deallocated.  Also, if you free a block, make sure that
// you set the block pointer to 0.  Don't leave pointers to
// deallocated blocks laying around!

static int
remove_block(ospfs_inode_t *oi)
{
	// current number of blocks in file
	uint32_t n = ospfs_size2nblocks(oi->oi_size);

	int32_t index_indir2;
	int32_t index_indir;
	int32_t index_direct;

	uint32_t indir_block = 0;
	uint32_t double_indir_block = 0;

	uint32_t *indir_data = NULL;
	uint32_t *double_indir_data = NULL;

	/* COMPLETED EXERCISE: Your code here */

	// If there are no allocated blocks, (size is already 0) return
	if(n == 0)
		return 0;
	else
		n--;

	index_indir2 = indir2_index(n);
	index_indir  = indir_index(n);
	index_direct = direct_index(n);

	if(index_indir == -1) // The block is directly stored in the inode
	{
		//should not happen
		if(oi->oi_direct[index_direct] == 0)
			return -EIO;

		free_block(oi->oi_direct[index_direct]);
		oi->oi_direct[index_direct] = 0;
		oi->oi_size = n * OSPFS_BLKSIZE;
		return 0;
	}

	if(index_indir2 == 0) // We need to use a two level indirect
	{
		//should not happen
		if(oi->oi_indirect2 == 0)
			return -EIO;

		double_indir_block = oi->oi_indirect2;
		double_indir_data = ospfs_block(double_indir_block);

		indir_block = double_indir_data[index_indir];
		indir_data = ospfs_block(indir_block);
	}
	else // last block in indirect block
	{
		indir_block = oi->oi_indirect;
		indir_data = ospfs_block(indir_block);
	}

	if(indir_data == NULL)
		return -EIO;

	// Free the last data block
	free_block(indir_data[index_direct]);
	indir_data[index_direct] = 0;
	oi->oi_size = n * OSPFS_BLKSIZE;

	//if the last data block is the only on for indir
	//we remove indir with it
	if(index_direct == 0)
	{
		free_block(indir_block);

		if(index_indir2 == -1)
			oi->oi_indirect = 0;
		else
			double_indir_data[index_indir] = 0;

		//if the dir block being removed is the only indir that indir2 has
		//we remove indir2 with it
		if(index_indir == 0 && index_indir2 == 0)
		{
			free_block(double_indir_block);
			oi->oi_indirect2 = 0;
		}
	}

	return 0;
}


// change_size(oi, want_size)
//	Use this function to change a file's size, allocating and freeing
//	blocks as necessary.
//
//   Inputs:  oi	-- pointer to the file whose size we're changing
//	      want_size -- the requested size in bytes
//   Returns: 0 on success, < 0 on error.  In particular:
//		-ENOSPC: if there are no free blocks available
//		-EIO:    an I/O error -- for example an indirect block should
//			 exist, but doesn't
//	      If the function succeeds, the file's oi_size member should be
//	      changed to want_size, with blocks allocated as appropriate.
//	      Any newly-allocated blocks should be erased (set to 0).
//	      If there is an -ENOSPC error when growing a file,
//	      the file size and allocated blocks should not change from their
//	      original values!!!
//            (However, if there is an -EIO error, do not worry too much about
//	      restoring the file.)
//
//   If want_size has the same number of blocks as the current file, life
//   is good -- the function is pretty easy.  But the function might have
//   to add or remove blocks.
//
//   If you need to grow the file, then do so by adding one block at a time
//   using the add_block function you coded above. If one of these additions
//   fails with -ENOSPC, you must shrink the file back to its original size!
//
//   If you need to shrink the file, remove blocks from the end of
//   the file one at a time using the remove_block function you coded above.
//
//   Also: Don't forget to change the size field in the metadata of the file.
//         (The value that the final add_block or remove_block set it to
//          is probably not correct).
//
//   COMPLETED EXERCISE: Finish off this function.

static int
change_size(ospfs_inode_t *oi, uint32_t new_size)
{
	uint32_t old_size = oi->oi_size;
	int r = 0;

	while (ospfs_size2nblocks(oi->oi_size) < ospfs_size2nblocks(new_size)) {
		r = add_block(oi);

		//if we don't have enough free blocks too accommandate,
		//set new_size to old size and shrink it back to its original
		if(r == -ENOSPC)
		{
			new_size = old_size;
			break;
		}

		if(r == -EIO)
			return -EIO;
	}
	while (ospfs_size2nblocks(oi->oi_size) > ospfs_size2nblocks(new_size)) {
		if(remove_block(oi) == -EIO)
			return -EIO;
	}

	// Reset the size back to what it was if the file grew, or down to what it shrank to
	oi->oi_size = new_size;
	return r;
}


// ospfs_notify_change
//	This function gets called when the user changes a file's size,
//	owner, or permissions, among other things.
//	OSPFS only pays attention to file size changes (see change_size above).
//	We have written this function for you -- except for file quotas.

static int
ospfs_notify_change(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = dentry->d_inode;
	ospfs_inode_t *oi = ospfs_inode(inode->i_ino);
	int retval = 0;

	if (attr->ia_valid & ATTR_SIZE) {
		// We should not be able to change directory size
		if (oi->oi_ftype == OSPFS_FTYPE_DIR)
			return -EPERM;
		if ((retval = change_size(oi, attr->ia_size)) < 0)
			goto out;
	}

	if (attr->ia_valid & ATTR_MODE)
		// Set this inode's mode to the value 'attr->ia_mode'.
		oi->oi_mode = attr->ia_mode;

	if ((retval = inode_change_ok(inode, attr)) < 0
	    || (retval = inode_setattr(inode, attr)) < 0)
		goto out;

    out:
	return retval;
}


// ospfs_read
//	Linux calls this function to read data from a file.
//	It is the file_operations.read callback.
//
//   Inputs:  filp	-- a file pointer
//            buffer    -- a user space ptr where data should be copied
//            count     -- the amount of data requested
//            f_pos     -- points to the file position
//   Returns: Number of chars read on success, -(error code) on error.
//
//   This function copies the corresponding bytes from the file into the user
//   space ptr (buffer).  Use copy_to_user() to accomplish this.
//   The current file position is passed into the function
//   as 'f_pos'; read data starting at that position, and update the position
//   when you're done.
//
//   EXERCISE(x): Complete this function.

static ssize_t
ospfs_read(struct file *filp, char __user *buffer, size_t count, loff_t *f_pos)
{
	ospfs_inode_t *oi = ospfs_inode(filp->f_dentry->d_inode->i_ino);
	int retval = 0;
	size_t amount = 0;
	
	if (*f_pos + count < *f_pos) { //overflow detection
		return -EIO;
	}
	
	if (*f_pos >= oi->oi_size) {
		count = 0;
	} else if (*f_pos + count > oi->oi_size)
		count = oi->oi_size - *f_pos;
	
	
	// Copy the data to user block by block
	while (amount < count && retval >= 0) {
		uint32_t blockno = ospfs_inode_blockno(oi, *f_pos);
		uint32_t n;
		char *data;
		
		uint32_t data_offset; // Data offset from the start of the block
		uint32_t bytes_left_to_copy = count - amount;
		
		// ospfs_inode_blockno returns 0 on error
		if (blockno == 0) {
			retval = -EIO;
			goto done;
		}
		
		data = ospfs_block(blockno);
		
		// Figure out how much data is left in this block to read.
		// Copy data into user space. Return -EFAULT if unable to write
		// into user space.
		// Use variable 'n' to track number of bytes moved.
		/* COMPLETED EXERCISE: Your code here */
		
		data_offset = *f_pos % OSPFS_BLKSIZE; //block size is 1024 bytes
		
		n = OSPFS_BLKSIZE - data_offset;
		
		// Copy bytes either until we hit the end
		// of the block or satisfy the user
		if (n > bytes_left_to_copy) {
			n = bytes_left_to_copy;
		}
		
		// Copy_to_user return the number of bytes that could not be copied. On success, this will be 0
		if (copy_to_user(buffer, data + data_offset, n) > 0) {//copy to buffer
			return -EFAULT;
		}
		
		buffer += n;
		amount += n;
		*f_pos += n;
	}
	
	done:
	return (retval >= 0 ? amount : retval);
}


// ospfs_write
//	Linux calls this function to write data to a file.
//	It is the file_operations.write callback.
//
//   Inputs:  filp	-- a file pointer
//            buffer    -- a user space ptr where data should be copied from
//            count     -- the amount of data to write
//            f_pos     -- points to the file position
//   Returns: Number of chars written on success, -(error code) on error.
//
//   This function copies the corresponding bytes from the user space ptr
//   into the file.  Use copy_from_user() to accomplish this. Unlike read(),
//   where you cannot read past the end of the file, it is OK to write past
//   the end of the file; this should simply change the file's size.
//
//   EXERCISE(x): Complete this function.

static ssize_t
ospfs_write(struct file *filp, const char __user *buffer, size_t count, loff_t *f_pos)
{
	ospfs_inode_t *oi = ospfs_inode(filp->f_dentry->d_inode->i_ino);
	int retval = 0;
	//amount written
	size_t amount = 0;
	size_t newsize;
	// Support files opened with the O_APPEND flag.  To detect O_APPEND,
	// use struct file's f_flags field and the O_APPEND bit.
	/* EXERCISE: Your code here */

	if(filp->f_flags & O_APPEND)
		*f_pos = oi->oi_size;
	newsize = *f_pos + count;
	// If the user is writing past the end of the file, change the file's
	// size to accomodate the request.  (Use change_size().)
	/* EXERCISE: Your code here */

	//avoid shirnking data
	//if(*f_pos + count < *f_pos)
	//	return -EIO;

	//change size as needed
	//we need enough blocks so we can copy data from user
	if(newsize >= oi->oi_size)
	{
		retval = change_size(oi, newsize);
		if(retval != 0)
			return retval;
	}
		
	// Copy data block by block
	while (amount < count && retval >= 0) {
		uint32_t blockno;
		uint32_t n;
		//int32_t added = 0;
		char *data;

		uint32_t data_offset; // Data offset from the start of the block
		uint32_t bytes_left_to_copy = count - amount;

		
		blockno = ospfs_inode_blockno(oi, *f_pos);

		if (blockno == 0) {
			retval = -EIO;
			goto done;
		}

		data = ospfs_block(blockno);

		// Figure out how much data is left in this block to write.
		// Copy data from user space. Return -EFAULT if unable to read
		// read user space.
		// Keep track of the number of bytes moved in 'n'.
		/* EXERCISE(x): Your code here */

		data_offset = *f_pos % OSPFS_BLKSIZE;
		
		n = OSPFS_BLKSIZE - data_offset;

		// Copy bytes either until we hit the end
		// of the block or satisfy the user
		if(n > bytes_left_to_copy)
			n = bytes_left_to_copy;

		data += *f_pos % OSPFS_BLKSIZE;
		
		if(copy_from_user(data, buffer, n) > 0)
		{
			return -EFAULT;
			goto done;
		}
		//added = (*f_pos + n) - oi->oi_size;

		//if(added < 0)
		//	added = 0;

		//oi->oi_size += added;
		buffer += n;
		amount += n;
		*f_pos += n;
	}

    done:
	return (retval >= 0 ? amount : retval);
}


// find_direntry(dir_oi, name, namelen)
//	Looks through the directory to find an entry with name 'name' (length
//	in characters 'namelen').  Returns a pointer to the directory entry,
//	if one exists, or NULL if one does not.
//
//   Inputs:  dir_oi  -- the OSP inode for the directory
//	      name    -- name to search for
//	      namelen -- length of 'name'.  (If -1, then use strlen(name).)
//
//	We have written this function for you.

static ospfs_direntry_t *
find_direntry(ospfs_inode_t *dir_oi, const char *name, int namelen)
{
	int off;
	if (namelen < 0)
		namelen = strlen(name);
	for (off = 0; off < dir_oi->oi_size; off += OSPFS_DIRENTRY_SIZE) {
		ospfs_direntry_t *od = ospfs_inode_data(dir_oi, off);
		if (od->od_ino
		    && strlen(od->od_name) == namelen
		    && memcmp(od->od_name, name, namelen) == 0)
			return od;
	}
	return 0;
}


// create_blank_direntry(dir_oi)
//	'dir_oi' is an OSP inode for a directory.
//	Return a blank directory entry in that directory.  This might require
//	adding a new block to the directory.  Returns an error pointer (see
//	below) on failure.
//
// ERROR POINTERS: The Linux kernel uses a special convention for returning
// error values in the form of pointers.  Here's how it works.
//	- ERR_PTR(errno): Creates a pointer value corresponding to an error.
//	- IS_ERR(ptr): Returns true iff 'ptr' is an error value.
//	- PTR_ERR(ptr): Returns the error value for an error pointer.
//	For example:
//
//	static ospfs_direntry_t *create_blank_direntry(...) {
//		return ERR_PTR(-ENOSPC);
//	}
//	static int ospfs_create(...) {
//		...
//		ospfs_direntry_t *od = create_blank_direntry(...);
//		if (IS_ERR(od))
//			return PTR_ERR(od);
//		...
//	}
//
//	The create_blank_direntry function should use this convention.
//
// COMPLETED EXERCISE: Write this function.

static ospfs_direntry_t *
create_blank_direntry(ospfs_inode_t *dir_oi)
{
	// Outline:
	// 1. Check the existing directory data for an empty entry.  Return one
	//    if you find it.
	// 2. If there's no empty entries, add a block to the directory.
	//    Use ERR_PTR if this fails; otherwise, clear out all the directory
	//    entries and return one of them.
	
	uint32_t new_size;
	ospfs_direntry_t *od;
	int retval = 0, offset;
	
	if (dir_oi->oi_ftype != OSPFS_FTYPE_DIR) {
		return ERR_PTR(-EIO);
	}
	
	for (offset = 0; offset < dir_oi->oi_size; offset += OSPFS_DIRENTRY_SIZE) {
		od = ospfs_inode_data(dir_oi, offset);
		//See the header ospfs_direntry. It says that:
		//If the inode number is 0, then the directory entry is EMPTY
		if (od->od_ino == 0) {
			return od;
		}
	}
	
	// If no free entries were found, add a block
	// ospfs_size2nblocks(size) returns the number of blocks required to hold 'size' bytes of data.
	new_size = (ospfs_size2nblocks(dir_oi->oi_size) + 1) * OSPFS_BLKSIZE;
	retval = change_size(dir_oi, new_size);
	
	if(retval != 0) { //change_size returns 0 on success
		return ERR_PTR(retval);
	}
	
	dir_oi->oi_size = new_size;
	
	//Note that in the above loop, offset stops when it is greater than or equal to dir_oi->oi_size.
	//Now, we change the size, we don't need to add anything to offset.
	return ospfs_inode_data(dir_oi, offset);
}

// ospfs_link(src_dentry, dir, dst_dentry
//   Linux calls this function to create hard links.
//   It is the ospfs_dir_inode_ops.link callback.
//
//   Inputs: src_dentry   -- a pointer to the dentry for the source file.  This
//                           file's inode contains the real data for the hard
//                           linked filae.  The important elements are:
//                             src_dentry->d_name.name
//                             src_dentry->d_name.len
//                             src_dentry->d_inode->i_ino
//           dir          -- a pointer to the containing directory for the new
//                           hard link.
//           dst_dentry   -- a pointer to the dentry for the new hard link file.
//                           The important elements are:
//                             dst_dentry->d_name.name
//                             dst_dentry->d_name.len
//                             dst_dentry->d_inode->i_ino
//                           Two of these values are already set.  One must be
//                           set by you, which one?
//   Returns: 0 on success, -(error code) on error.  In particular:
//               -ENAMETOOLONG if dst_dentry->d_name.len is too large, or
//			       'symname' is too long;
//               -EEXIST       if a file named the same as 'dst_dentry' already
//                             exists in the given 'dir';
//               -ENOSPC       if the disk is full & the file can't be created;
//               -EIO          on I/O error.
//
//   EXERCISE(x): Complete this function.

static int
ospfs_link(struct dentry *src_dentry, struct inode *dir, struct dentry *dst_dentry) {
	// Note: source entry and destination entry have the same inode number since this is hard link.
	
	ospfs_inode_t *dir_oi = ospfs_inode(dir->i_ino);
	ospfs_inode_t *src_oi = ospfs_inode(src_dentry->d_inode->i_ino);
	ospfs_direntry_t *new_entry;
	
	// Some error handling code. See the function header requirement above.
	
	if (dir_oi == NULL || dir_oi->oi_ftype != OSPFS_FTYPE_DIR || src_oi->oi_nlink + 1 == 0) {
		return -EIO;
	}
	
	if (dst_dentry->d_name.len > OSPFS_MAXNAMELEN) {
		return -ENAMETOOLONG;
	}
	
	if (find_direntry(dir_oi, dst_dentry->d_name.name, dst_dentry->d_name.len) != NULL) {
		return -EEXIST;
	}
	
	// Since this is hard link. inode structure are the same for both original file and the hard link
	// We only need to allocate a new directory entry for the hard link.
	
	new_entry = create_blank_direntry(dir_oi);
	
	if (IS_ERR(new_entry)) {
		return PTR_ERR(new_entry);
	}
	
	if (new_entry == NULL) {
		return -EIO;
	}
	
	// Initialize the new directory entry
	new_entry->od_ino = src_dentry->d_inode->i_ino;
	memcpy(new_entry->od_name, dst_dentry->d_name.name, dst_dentry->d_name.len);
	new_entry->od_name[dst_dentry->d_name.len] = '\0';
	
	// Increase the link count on the source file.
	// Note that we can only have hard link on regular file.
	src_oi->oi_nlink++;
	
	return 0;
}

// ospfs_create
//   Linux calls this function to create a regular file.
//   It is the ospfs_dir_inode_ops.create callback.
//
//   Inputs:  dir	-- a pointer to the containing directory's inode
//            dentry    -- the name of the file that should be created
//                         The only important elements are:
//                         dentry->d_name.name: filename (char array, not null
//                            terminated)
//                         dentry->d_name.len: length of filename
//            mode	-- the permissions mode for the file (set the new
//			   inode's oi_mode field to this value)
//	      nd	-- ignore this
//   Returns: 0 on success, -(error code) on error.  In particular:
//               -ENAMETOOLONG if dentry->d_name.len is too large;
//               -EEXIST       if a file named the same as 'dentry' already
//                             exists in the given 'dir';
//               -ENOSPC       if the disk is full & the file can't be created;
//               -EIO          on I/O error.
//
//   We have provided strictly less skeleton code for this function than for
//   the others.  Here's a brief outline of what you need to do:
//   1. Check for the -EEXIST error and find an empty directory entry using the
//	helper functions above.
//   2. Find an empty inode.  Set the 'entry_ino' variable to its inode number.
//   3. Initialize the directory entry and inode.
//
//   COMPLETED EXERCISE: Complete this function.

static int
ospfs_create(struct inode *dir, struct dentry *dentry, int mode, struct nameidata *nd)
{
	ospfs_inode_t *dir_oi = ospfs_inode(dir->i_ino); // load ospfs_inode structure from disk
	uint32_t entry_ino = 0;
	
	ospfs_inode_t *file_oi = NULL;
	ospfs_direntry_t *new_entry = NULL;
	//uint32_t block_no = 0;
	struct inode *i;
	
	if (dir_oi->oi_ftype != OSPFS_FTYPE_DIR) { //check if file type is directory
		return -EIO;
	}
	
	if (dentry->d_name.len > OSPFS_MAXNAMELEN) { //check if the name is too large
		return -ENAMETOOLONG;
	}
	
	if (find_direntry(dir_oi, dentry->d_name.name, dentry->d_name.len) != NULL) { //check if file name already exists
		return -EEXIST;
	}
	
	// Step 1: Create a new inode for new file
	
	// Get an inode 
	entry_ino = find_free_inode(); // Tuan: I have this function impelemented above.
	// return inode number if it finds a free inode. return 0 otherwise.	
	// How do we know if an inode is free? when its link count equals 0.	
	
	if(entry_ino == 0) {
		return -ENOSPC;
	}
	
	file_oi = ospfs_inode(entry_ino); // load ospfs_inode structure from disk
	
	if (file_oi == NULL) {
		return -EIO;
	}
	
	// Initialize the new inode structure with correct values
	file_oi->oi_size = 0; //File size
	file_oi->oi_ftype = OSPFS_FTYPE_REG;
	file_oi->oi_nlink = 1; //Number of hard links
	file_oi->oi_mode = mode; //File permission mode
	
	// Step 2: Create a new directory entry for new file
	
	// As part of this lab, we need to implement create_blank_direntry
	new_entry = create_blank_direntry(dir_oi); //create a blank directory entry in that directory
	//This function returns a pointer to a directory entry which we can modify.
	
	if(IS_ERR(new_entry)) {
		return PTR_ERR(new_entry);
	}
	
	// Initialize the new directory entry with correct values
	new_entry->od_ino = entry_ino;
	memcpy(new_entry->od_name, dentry->d_name.name, dentry->d_name.len);
	new_entry->od_name[dentry->d_name.len] = '\0';
	
	/* Execute this code after your function has successfully created the
	  file.  Set entry_ino to the created file's inode number before
	  getting here. */
	i = ospfs_mk_linux_inode(dir->i_sb, entry_ino);
	if (!i)
		return -ENOMEM;
	d_instantiate(dentry, i);
	return 0;
}


// ospfs_symlink(dirino, dentry, symname)
//   Linux calls this function to create a symbolic link.
//   It is the ospfs_dir_inode_ops.symlink callback.
//
//   Inputs: dir     -- a pointer to the containing directory's inode
//           dentry  -- the name of the file that should be created
//                      The only important elements are:
//                      dentry->d_name.name: filename (char array, not null
//                           terminated)
//                      dentry->d_name.len: length of filename
//           symname -- the symbolic link's destination
//
//   Returns: 0 on success, -(error code) on error.  In particular:
//               -ENAMETOOLONG if dentry->d_name.len is too large, or
//			       'symname' is too long;
//               -EEXIST       if a file named the same as 'dentry' already
//                             exists in the given 'dir';
//               -ENOSPC       if the disk is full & the file can't be created;
//               -EIO          on I/O error.
//
//   COMPLETED EXERCISE: Complete this function.

static int
ospfs_symlink(struct inode *dir, struct dentry *dentry, const char *symname)
{
	ospfs_inode_t *dir_oi = ospfs_inode(dir->i_ino);
	uint32_t entry_ino = 0;

	ospfs_symlink_inode_t *symlink_ino = NULL; 
	ospfs_direntry_t *od;

	char *qmark;
	char *colon;

	(void)dir_oi; // Silences compiler warning

	// Error conditions!

	// directory checking
	if (dir_oi->oi_ftype != OSPFS_FTYPE_DIR)
		return -EIO;

	// Name too long?
	else if (dentry->d_name.len > OSPFS_MAXNAMELEN)
		return -ENAMETOOLONG;

	// Name in use?
	else if (find_direntry(dir_oi, dentry->d_name.name, dentry->d_name.len) != NULL)
		return -EEXIST;

	// Determine what inode we can use... helps us detect out of space errors
	// Start at 2 since the first two inodes are special
	entry_ino = find_free_inode();
	symlink_ino = (ospfs_symlink_inode_t *) ospfs_inode(entry_ino);

	//no free inode found
	if(entry_ino == 0)
		return -ENOSPC;
	//fail getting data
	if(symlink_ino == NULL)
		return -EIO;

	// Get our new entry
	od = create_blank_direntry(dir_oi);
	if (IS_ERR(od))
		return PTR_ERR(od);

	//strpbrk returns the first instance of appeard character
	qmark = strpbrk(symname, "?");
	colon = strpbrk(symname, ":");
	//format :  root?root:notroot  stored as "?root:notroot" -jingyu
	if(qmark && colon && colon > qmark) // conditional symlink
	{
		size_t root_path_len = colon - qmark + 1;//4
		size_t other_path_len = strlen(colon);	//7

		if(root_path_len + other_path_len > OSPFS_MAXNAMELEN)
			return -ENAMETOOLONG;

		symlink_ino->oi_size = strlen(qmark) + 1;
		
		//the whole conditional link name is stored in oi_symlink
		//separated by a '\0'
		strncpy(symlink_ino->oi_symlink, qmark, root_path_len - 1);
		symlink_ino->oi_symlink[root_path_len - 1] = '\0';

		strncpy(symlink_ino->oi_symlink + root_path_len, colon, other_path_len);
		symlink_ino->oi_symlink[symlink_ino->oi_size] = '\0';
	}
	else // regular symlink
	{
		size_t name_len = strlen(symname);
		if (name_len > OSPFS_MAXSYMLINKLEN)
			return -ENAMETOOLONG;

		symlink_ino->oi_size = name_len;
		strncpy(symlink_ino->oi_symlink, symname, symlink_ino->oi_size);
		symlink_ino->oi_symlink[symlink_ino->oi_size] = '\0';
	}

	// Set the meta information for the symlink inode.
	symlink_ino->oi_ftype = OSPFS_FTYPE_SYMLINK;
	symlink_ino->oi_nlink = 1;
	// Set the meta information for direntry
	strncpy(od->od_name, dentry->d_name.name, dentry->d_name.len);
	od->od_name[dentry->d_name.len] = 0;
	od->od_ino = entry_ino;

	dir_oi->oi_nlink++;

	// Instructor-provided code
	{
		struct inode *i = ospfs_mk_linux_inode(dir->i_sb, entry_ino);
	
		if (!i)
			return -ENOMEM;
		d_instantiate(dentry, i);
	}

	return 0;
}


// ospfs_follow_link(dentry, nd)
//   Linux calls this function to follow a symbolic link.
//   It is the ospfs_symlink_inode_ops.follow_link callback.
//
//   Inputs: dentry -- the symbolic link's directory entry
//           nd     -- to be filled in with the symbolic link's destination
//
//   Exercise: Expand this function to handle conditional symlinks.  Conditional
//   symlinks will always be created by users in the following form
//     root?/path/1:/path/2.
//   (hint: Should the given form be changed in any way to make this method
//   easier?  With which character do most functions expect C strings to end?)

static void *
ospfs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	ospfs_symlink_inode_t *oi =
		(ospfs_symlink_inode_t *) ospfs_inode(dentry->d_inode->i_ino);
	//keep track of the offset in symlink
	char *off;
	//conditional:
	// If root, give a pointer to the start of the symlink
	// When it hit null byte it will return
	if(oi->oi_symlink[0] == '?'){
		if(current->uid == 0)
		{
			nd_set_link(nd, oi->oi_symlink + 1);
			return (void *) 0;
		}
		// Otherwise we follow till after ':'
		off = oi->oi_symlink;
		while(*off != '\0')
			off++;
	
		// To make sure that second dir is set for conditional sym
		if(off[1] != ':' || off - oi->oi_symlink >= oi->oi_size)
			return ERR_PTR(-EIO);
	
		// set link past '\0' and ':'
		nd_set_link(nd, off + 2);
		return (void *) 0;
	}
	else{
		// If not conditional, great, just set and return
	
		nd_set_link(nd, oi->oi_symlink);
		return (void *) 0;
	}
}


// Define the file system operations structures mentioned above.

static struct file_system_type ospfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "ospfs",
	.get_sb		= ospfs_get_sb,
	.kill_sb	= kill_anon_super
};

static struct inode_operations ospfs_reg_inode_ops = {
	.setattr	= ospfs_notify_change
};

static struct file_operations ospfs_reg_file_ops = {
	.llseek		= generic_file_llseek,
	.read		= ospfs_read,
	.write		= ospfs_write
};

static struct inode_operations ospfs_dir_inode_ops = {
	.lookup		= ospfs_dir_lookup,
	.link		= ospfs_link,
	.unlink		= ospfs_unlink,
	.create		= ospfs_create,
	.symlink	= ospfs_symlink
};

static struct file_operations ospfs_dir_file_ops = {
	.read		= generic_read_dir,
	.readdir	= ospfs_dir_readdir
};

static struct inode_operations ospfs_symlink_inode_ops = {
	.readlink	= generic_readlink,
	.follow_link	= ospfs_follow_link
};

static struct dentry_operations ospfs_dentry_ops = {
	.d_delete	= ospfs_delete_dentry
};

static struct super_operations ospfs_superblock_ops = {
};


// Functions used to hook the module into the kernel!

static int __init init_ospfs_fs(void)
{
	eprintk("Loading ospfs module...\n");
	return register_filesystem(&ospfs_fs_type);
}

static void __exit exit_ospfs_fs(void)
{
	unregister_filesystem(&ospfs_fs_type);
	eprintk("Unloading ospfs module\n");
}

module_init(init_ospfs_fs)
module_exit(exit_ospfs_fs)

// Information about the module
MODULE_AUTHOR("Jingyu liu and Zhinan guan");
MODULE_DESCRIPTION("OSPFS");
MODULE_LICENSE("GPL");
//Some helper functions are provided by Tuan
