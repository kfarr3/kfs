#ifndef KFS_PORT_H_
#define KFS_PORT_H_

#include "kfs.h"

/*  This file is to be ported to your Flash Chip.  */

// Set the Sector Size of your disks
#define SECTOR_SIZE 512

// returns sector count of disk, this should be the number of write sectors
unsigned int kfs_get_sector_count(void);

// Perform any initialization you require prior to reading/writing sectors
KFS_RET kfs_disk_initialize(void);

// Implement full sector read/write operations here
KFS_RET kfs_write_sector(const unsigned char *buff, unsigned int sector, unsigned int count);
KFS_RET kfs_read_sector(unsigned char *buff, unsigned int sector, unsigned int count);


#endif /*KFS_PORT_H_*/
