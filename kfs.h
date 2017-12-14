#ifndef KFS_H_
#define KFS_H_

/*  The first x files are fixed size while the final file is a rolling log */

/* To change the number of fixed files, create the two #defines here for _SIZE_BYTES
 * and _FD_INDEX, then also update the kfs.c functions kfs_format and kfs_print_stats */

#define KFS_CONFIG_SIZE_BYTES		(100*1024*1024)
#define KFS_FIRMWARE_SIZE_BYTES		(10*1024*1024)
#define KFS_EVENT_SIZE_BYTES		(200*1024*1024)
	
#define KFS_FIRMWARE_FD_INDEX 	((int)0)
#define KFS_CONFIG_FD_INDEX 	((int)1)
#define KFS_EVENT_FD_INDEX		((int)2)
#define KFS_LOG_FD_INDEX		((int)3)

typedef enum
{
	KFS_SUCCESS				= -200,
	KFS_BADDISK,
	KFS_WRITE_ERROR,
	KFS_READ_ERROR,
	KFS_SEEK_ERROR,
	
	KFS_BAD_VERSION,
	KFS_UNFORMATTED,
	KFS_MISMATCH_SECTOR_COUNT,
	
	KFS_UNKNOWN_FILE,
	KFS_NOT_INSTALLED,
	
}KFS_RET;

#define KFS_TRUNCATE 	(1<<0)

#define KFS_SEEK_RELATIVE 	1
#define KFS_SEEK_ABSOLUTE 	2

KFS_RET kfs_disk_state(void);

KFS_RET kfs_init(void); 	// Initialize, call once
KFS_RET kfs_sync(void); 	// Write buffered sectors, 
KFS_RET kfs_format(void); 	// Format the disk, also done internally if it is unformatted upon initialization or sync
KFS_RET kfs_open(int fd_index, unsigned int flags); // Open a file, each file can be opened itself
KFS_RET kfs_seek(int fd_index, long long offset, unsigned int type); // Move read index, KFS_SEEK_RELATIVE, KFS_SEEK_ABSOLUTE
int kfs_eof(int fd_index); // determine if we are at the end of the file
unsigned long long kfs_file_size(int fd_index); // Number of bytes in file
unsigned long long kfs_file_allocated_size(int fd_index); // Maximum number of bytes allocated to this file
int kfs_read(int fd_index, void *buffer, unsigned int length); // read length bytes into buffer from fd_index
int kfs_write(int fd_index, void *buffer, unsigned int length); // write length bytes from buffer to fd_index
char *kfs_gets(int fd_index, char *buffer, unsigned int max_length); // get a string of max_length from fd_index and put into buffer
void kfs_print_stats(void); // Print useful information on disk
char *kfs_strerror(KFS_RET error); // turn KFS_RET to a string for pretty printing
void kfs_periodic(void); // Call in idle task to monitor for disk insert/removal

#endif /*KFS_H_*/
