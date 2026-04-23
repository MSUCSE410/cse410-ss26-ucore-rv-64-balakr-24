#ifndef FILE_H
#define FILE_H

#include "fs.h"
#include "proc.h"
#include "types.h"

#define PIPESIZE (512)
#define FILEPOOLSIZE (NPROC * FD_BUFFER_SIZE)

// in-memory copy of an inode,it can be used to quickly locate file entities on disk
struct inode {
	uint dev; // Device number
	uint inum; // Inode number

	// Kernel-only counter, Reference count
	// how many pointers in the system's memory are currently "holding" this inode
	int ref; 
	// 0 (Invalid): The kernel has an entry for this inode in memory, but it hasn't actually read the data (like size or block addresses) from the disk yet
	// 1 (Valid): The kernel has successfully called bread and filled this structure with the real data from the disk
	int valid; // inode has been read from disk?
	// types: dir, data file
	// 0 means empty
	// tells the kernel which functions are allowed
	short type; // copy of disk inode
	// Disk-based counter
	// tracks how many "names" or "nicknames" this file has in the filesystem
	short nlink;
	
	uint size;
	uint addrs[NDIRECT + 1];
	// LAB4: You may need to add link count here
};

// Defines a file in memory that provides information about the current use of the file and the corresponding inode location
struct file {
	enum { FD_NONE = 0, FD_INODE, FD_STDIO } type;
	int ref; // reference count
	char readable;
	char writable;
	struct inode *ip; // FD_INODE
	uint off;
};

//A few specific fd
enum {
	STDIN = 0,
	STDOUT = 1,
	STDERR = 2,
};

extern struct file filepool[FILEPOOLSIZE];

void fileclose(struct file *);
struct file *filealloc();
int fileopen(char *, uint64);
uint64 inodewrite(struct file *, uint64, uint64);
uint64 inoderead(struct file *, uint64, uint64);
struct file *stdio_init(int);
int show_all_files();

#endif // FILE_H