// kfs.c

#include <string.h>
#include <stdio.h>

#include "kfs_port.h"
#include "kfs.h"
#include "system.h"
#include "logger.h"
#include "pinout.h"
#include "driverlib/sysctl.h"

#define KFS_MAGIC	((unsigned int)(('K'<<0)|('F'<<8)|('S'<<16)|('\0'<<24)))
#define KFS_VERSION ((unsigned int)(('0'<<0)|('.'<<8)|('1'<<16)|('\0'<<24)))

static KFS_RET disk_state=KFS_BADDISK;
unsigned int next_update_ms = 0;

typedef struct
{
	unsigned char sector[SECTOR_SIZE];
	unsigned int  sector_number;
}_kfs_buffered_sectors;

typedef struct
{
	unsigned long long sector_start;		// file sector start
	unsigned long long sector_count;		// number of sectors allocated to this file

	unsigned long long start_index;		// byte index from sector_start to start of actual data (used for circular buffers)

	unsigned long long read_index;		// byte index from sector_start
	unsigned long long write_index;		// byte index from sector_start
	
	unsigned long long file_size;			// size in bytes of this file
	unsigned long long allocated_bytes;	// total size of file
}_kfs_file_def;

typedef struct
{
	unsigned int kfs_magic; 		// 4 byte magic to indicate raw_fd system
	unsigned int kfs_version; 		// raw_fd version number 
	unsigned long long sector_count;
	_kfs_file_def files[4]; 		// config, firmware, event, log files
}_kfs;


static _kfs kfs;
_kfs_buffered_sectors buffered_sectors[4];

static KFS_RET _kfs_initialize_disk(unsigned long *reported_sector_count)
{
	if (read_input(SD_SW)) return KFS_NOT_INSTALLED;
	if (kfs_disk_initialize()!=KFS_SUCCESS) return KFS_BADDISK;
	*reported_sector_count=kfs_get_sector_count();
	return KFS_SUCCESS;
}

static char *kfs_size_str(unsigned long long size, char *size_str)
{
	if (size<1024) {sprintf(size_str, "%4lldb", size); return size_str;}
	size/=1024;
	if (size<1024) {sprintf(size_str, "%4lldk", size); return size_str;}
	size/=1024;
	
	sprintf(size_str, "%4lldm", size);
	return size_str;
}

//Periodicially checks the SD install
void kfs_periodic(void)
{
	if (next_update_ms < uptime_ms)
	{
	   //No SD card was detected
	   if (disk_state == KFS_NOT_INSTALLED)
	   {
		  if (read_input(SD_SW) == 0) //SD card was inserted
		  {
			  KFS_RET ret = kfs_init();
			  debug_printf("SD State changed, re-initializing...\r\n");
			  if ((ret==KFS_UNFORMATTED)||(ret==KFS_BAD_VERSION)||(ret==KFS_MISMATCH_SECTOR_COUNT))
			  {
				   debug_printf("Formatting disk...");
				   ret = kfs_format();
				   debug_printf("%s\r\n", kfs_strerror(ret));

				   debug_printf("Re-Mounting...");
				   ret = kfs_init();
				   debug_printf("%s\r\n", kfs_strerror(ret));
			  }
		  }
	   }
	   else if (read_input(SD_SW))
	   {
		  debug_printf("SD State changed, re-initializing...\r\n");
		  KFS_RET ret = kfs_init();
		  if ((ret==KFS_UNFORMATTED)||(ret==KFS_BAD_VERSION)||(ret==KFS_MISMATCH_SECTOR_COUNT))
		  {
			   debug_printf("Formatting disk...");
			   ret = kfs_format();
			   debug_printf("%s\r\n", kfs_strerror(ret));

			   debug_printf("Re-Mounting...");
			   ret = kfs_init();
			   debug_printf("%s\r\n", kfs_strerror(ret));
		  }
	   }
	   next_update_ms = uptime_ms + 1000;
	}
}

KFS_RET kfs_disk_state(void)
{
	return disk_state;
}

KFS_RET kfs_init(void)
{
	next_update_ms = uptime_ms + 5000; //Wait 3 seconds before starting periodic checks
	unsigned long reported_sector_count;
	
	if (read_input(SD_SW)) { disk_state = KFS_NOT_INSTALLED; goto done; }
	if (_kfs_initialize_disk(&reported_sector_count)!=KFS_SUCCESS) { disk_state = KFS_BADDISK; goto done; }
	
	// Get filesystem information
	if (kfs_read_sector(buffered_sectors[0].sector, 0, 1)!=KFS_SUCCESS)
	{
		if (kfs_read_sector(buffered_sectors[0].sector, 0, 1)!=KFS_SUCCESS)
		{
			disk_state = KFS_BADDISK; 
			goto done;
		} 
	}
	memcpy(&kfs, buffered_sectors[0].sector, sizeof(_kfs));
	
	if (kfs.kfs_magic!=KFS_MAGIC) 					{ disk_state = KFS_UNFORMATTED; 			goto done; }
	if (kfs.kfs_version!=KFS_VERSION) 				{ disk_state = KFS_BAD_VERSION; 			goto done; }
	if (kfs.sector_count!=reported_sector_count) 	{ disk_state = KFS_MISMATCH_SECTOR_COUNT;	goto done; }

	memset(buffered_sectors, 0, sizeof(buffered_sectors));
	disk_state=KFS_SUCCESS;
	goto done;

done:
	return disk_state;	
}

KFS_RET kfs_sync(void)
{
	if (read_input(SD_SW)) return KFS_NOT_INSTALLED;
	memcpy(buffered_sectors[0].sector, &kfs, sizeof(_kfs));
	
	disk_state = KFS_SUCCESS;
	
	if (kfs_write_sector(buffered_sectors[0].sector, 0, 1)!=KFS_SUCCESS)
	{
		if (kfs_write_sector(buffered_sectors[0].sector, 0, 1)!=KFS_SUCCESS)
		{
			disk_state = KFS_BADDISK;
		}
		else
		{
			log_event(EVENT_NUMBER_DISK_101);
		}
	}
	
	return disk_state;
}

KFS_RET kfs_format(void)
{
	unsigned long reported_sector_count;
	unsigned long sectors_used=1;
	
	if (read_input(SD_SW)) return (disk_state=KFS_NOT_INSTALLED);
	if (_kfs_initialize_disk(&reported_sector_count)!=KFS_SUCCESS) return (disk_state=KFS_BADDISK);

	kfs.kfs_magic   = KFS_MAGIC;
	kfs.kfs_version = KFS_VERSION;
	kfs.sector_count= reported_sector_count;
	
	// Setup Firmware
	kfs.files[KFS_FIRMWARE_FD_INDEX].sector_start=sectors_used;
	kfs.files[KFS_FIRMWARE_FD_INDEX].sector_count=(KFS_FIRMWARE_SIZE_BYTES/SECTOR_SIZE);
	kfs.files[KFS_FIRMWARE_FD_INDEX].start_index=0;
	kfs.files[KFS_FIRMWARE_FD_INDEX].read_index=0;
	kfs.files[KFS_FIRMWARE_FD_INDEX].write_index=0;
	kfs.files[KFS_FIRMWARE_FD_INDEX].file_size=0;
	kfs.files[KFS_FIRMWARE_FD_INDEX].allocated_bytes=kfs.files[KFS_FIRMWARE_FD_INDEX].sector_count*SECTOR_SIZE;
	sectors_used+=kfs.files[KFS_FIRMWARE_FD_INDEX].sector_count;
	
	// Setup Config
	kfs.files[KFS_CONFIG_FD_INDEX].sector_start=sectors_used;
	kfs.files[KFS_CONFIG_FD_INDEX].sector_count=(KFS_CONFIG_SIZE_BYTES/SECTOR_SIZE);
	kfs.files[KFS_CONFIG_FD_INDEX].start_index=0;
	kfs.files[KFS_CONFIG_FD_INDEX].read_index=0;
	kfs.files[KFS_CONFIG_FD_INDEX].write_index=0;
	kfs.files[KFS_CONFIG_FD_INDEX].file_size=0;
	kfs.files[KFS_CONFIG_FD_INDEX].allocated_bytes=kfs.files[KFS_CONFIG_FD_INDEX].sector_count*SECTOR_SIZE;
	sectors_used+=kfs.files[KFS_CONFIG_FD_INDEX].sector_count;
	
	// Setup Events
	kfs.files[KFS_EVENT_FD_INDEX].sector_start=sectors_used;
	kfs.files[KFS_EVENT_FD_INDEX].sector_count=(KFS_EVENT_SIZE_BYTES/SECTOR_SIZE);
	kfs.files[KFS_EVENT_FD_INDEX].start_index=0;
	kfs.files[KFS_EVENT_FD_INDEX].read_index=0;
	kfs.files[KFS_EVENT_FD_INDEX].write_index=0;
	kfs.files[KFS_EVENT_FD_INDEX].file_size=0;
	kfs.files[KFS_EVENT_FD_INDEX].allocated_bytes=kfs.files[KFS_EVENT_FD_INDEX].sector_count*SECTOR_SIZE;
	sectors_used+=kfs.files[KFS_EVENT_FD_INDEX].sector_count;
	
	// Setup Logs
	kfs.files[KFS_LOG_FD_INDEX].sector_start=sectors_used;
	kfs.files[KFS_LOG_FD_INDEX].sector_count=reported_sector_count-sectors_used;
	kfs.files[KFS_LOG_FD_INDEX].start_index=0;
	kfs.files[KFS_LOG_FD_INDEX].read_index=0;
	kfs.files[KFS_LOG_FD_INDEX].write_index=0;
	kfs.files[KFS_LOG_FD_INDEX].file_size=0;
	kfs.files[KFS_LOG_FD_INDEX].allocated_bytes=kfs.files[KFS_LOG_FD_INDEX].sector_count*SECTOR_SIZE;
	sectors_used+=kfs.files[KFS_LOG_FD_INDEX].sector_count;
	
	return kfs_sync();
}

KFS_RET kfs_open(int fd_index, unsigned int flags)
{
	if (read_input(SD_SW)) return KFS_NOT_INSTALLED;
	if (fd_index>=4) return KFS_UNKNOWN_FILE;

	if (disk_state!=KFS_SUCCESS)
	{
		debug_printf("Mounting...");
	    kfs_init();
	   	debug_printf("%s\r\n", kfs_strerror(disk_state));
	    
	    if ((disk_state==KFS_UNFORMATTED)||(disk_state==KFS_BAD_VERSION)||(disk_state==KFS_MISMATCH_SECTOR_COUNT))
	    {
	    	debug_printf("Formatting disk...");
	    	kfs_format();
	    	debug_printf("%s\r\n", kfs_strerror(disk_state));
	    	
	    	debug_printf("Re-Mounting...");
	    	kfs_init();
	   		debug_printf("%s\r\n", kfs_strerror(disk_state));
	    }
		
		if (disk_state!=KFS_SUCCESS) return disk_state;
	}

	buffered_sectors[fd_index].sector_number=0;
	
	
	if (flags&KFS_TRUNCATE)
	{
		kfs.files[fd_index].start_index=0;
		kfs.files[fd_index].file_size=0;
	}
	
	kfs.files[fd_index].read_index=kfs.files[fd_index].start_index;	
	kfs.files[fd_index].write_index = (kfs.files[fd_index].file_size+kfs.files[fd_index].start_index)%kfs.files[fd_index].allocated_bytes;

	//debug_printf("OPEN: start=%d, size=%d, write=%d\r\n", kfs.files[fd_index].start_index, kfs.files[fd_index].file_size, kfs.files[fd_index].write_index);
	return KFS_SUCCESS;	
}
	
int kfs_eof(int fd_index)
{
	return (kfs.files[fd_index].read_index==kfs.files[fd_index].write_index);
}

unsigned long long kfs_file_size(int fd_index)
{
	return kfs.files[fd_index].file_size;
}

unsigned long long kfs_file_allocated_size(int fd_index)
{
	return kfs.files[fd_index].allocated_bytes;
}
	
KFS_RET kfs_seek(int fd_index, long long offset, unsigned int type)
{
	//debug_printf("SEEK: start=%d, read=%d,  size=%d, allocated=%d\r\n", kfs.files[fd_index].start_index, kfs.files[fd_index].read_index, kfs.files[fd_index].file_size, kfs.files[fd_index].allocated_bytes);
	
	if (type==KFS_SEEK_ABSOLUTE)
	{
		if (offset>kfs.files[fd_index].file_size) return KFS_SEEK_ERROR;
		
		kfs.files[fd_index].read_index=(kfs.files[fd_index].start_index+offset)%kfs.files[fd_index].allocated_bytes;
	}
	else if (type==KFS_SEEK_RELATIVE)
	{
		 if ((offset+kfs.files[fd_index].read_index)>kfs.files[fd_index].file_size) return KFS_SEEK_ERROR;
		 
		 kfs.files[fd_index].read_index=(kfs.files[fd_index].read_index+offset)%kfs.files[fd_index].allocated_bytes;
	}
	
	//debug_printf("SEEK DONE: read=%d\r\n", kfs.files[fd_index].read_index);
	
	return KFS_SUCCESS;	
}
	
static int kfs_internal_write(int fd_index, unsigned long long byte_offset, void *buffer, unsigned int length)
{
	int bytes_to_copy;
	unsigned int sector_number;
	int bytes_written=0;
	
	//debug_printf("kfs_internal_write: byte_offset=%d, length=%d\r\n", byte_offset, length);
	disk_state = KFS_SUCCESS;
	
	buffered_sectors[fd_index].sector_number=0;

	if ((byte_offset+length)>kfs.files[fd_index].allocated_bytes)
	{
		//debug_printf("kfs_internal_write: writing past end of file\r\n");
		if ((byte_offset+length)>kfs.files[fd_index].allocated_bytes) return 0;
		length=kfs.files[fd_index].allocated_bytes-byte_offset;
		//debug_printf("length is now %d\r\n", length);
	}
	
	if (byte_offset%SECTOR_SIZE)
	{
		//debug_printf("kfs_internal_write: writing to a non-sector aligned index\r\n");
	
		sector_number=kfs.files[fd_index].sector_start+(byte_offset/SECTOR_SIZE);
		//debug_printf("kfs_write: reading from sector %d\r\n", sector_number);
		if (kfs_read_sector(buffered_sectors[fd_index].sector, sector_number, 1)!=KFS_SUCCESS)
		{
			if (kfs_read_sector(buffered_sectors[fd_index].sector, sector_number, 1)!=KFS_SUCCESS)
			{
				return KFS_BADDISK;
			}
			else
			{
				log_event(EVENT_NUMBER_DISK_201);
			}
		}
		
		bytes_to_copy=SECTOR_SIZE-(byte_offset%SECTOR_SIZE);
		if (bytes_to_copy>length) bytes_to_copy=length;
		
		//debug_printf("kfs_write: bytes_to_copy = %d to offset %d\r\n", bytes_to_copy, (byte_offset%SECTOR_SIZE));
		
		memcpy(buffered_sectors[fd_index].sector+(byte_offset%SECTOR_SIZE), buffer, bytes_to_copy);
		if (kfs_write_sector(buffered_sectors[fd_index].sector, kfs.files[fd_index].sector_start+(byte_offset/SECTOR_SIZE), 1)!=KFS_SUCCESS)
		{
			if (kfs_write_sector(buffered_sectors[fd_index].sector, kfs.files[fd_index].sector_start+(byte_offset/SECTOR_SIZE), 1)!=KFS_SUCCESS)
			{			
				return KFS_BADDISK;
			}
			else
			{
				log_event(EVENT_NUMBER_DISK_101);
			}
		}

		buffer = (unsigned char*)buffer + bytes_to_copy;
		length-=bytes_to_copy;
		bytes_written+=bytes_to_copy;
		byte_offset+=bytes_to_copy;
	}

	while(length>0)
	{
		if (length>SECTOR_SIZE) bytes_to_copy=SECTOR_SIZE;
		else                    bytes_to_copy=length;
		
		//debug_printf("kfs_internal_write full write bytes_to_copy = %d\r\n", bytes_to_copy);
		
		if (kfs_read_sector(buffered_sectors[fd_index].sector, kfs.files[fd_index].sector_start+(byte_offset/SECTOR_SIZE), 1)!=KFS_SUCCESS)
		{
			if (kfs_read_sector(buffered_sectors[fd_index].sector, kfs.files[fd_index].sector_start+(byte_offset/SECTOR_SIZE), 1)!=KFS_SUCCESS)
			{
				return KFS_BADDISK;
			}
			else
			{
				log_event(EVENT_NUMBER_DISK_201);
			}
		}
		
		memcpy(buffered_sectors[fd_index].sector, buffer, bytes_to_copy);

		if (kfs_write_sector(buffered_sectors[fd_index].sector, kfs.files[fd_index].sector_start+(byte_offset/SECTOR_SIZE), 1)!=KFS_SUCCESS)
		{
			if (kfs_write_sector(buffered_sectors[fd_index].sector, kfs.files[fd_index].sector_start+(byte_offset/SECTOR_SIZE), 1)!=KFS_SUCCESS)
			{
				return KFS_BADDISK;
			}
			else
			{
				log_event(EVENT_NUMBER_DISK_101);
			}
		}
		
		buffer = (unsigned char*)buffer + bytes_to_copy;
		length-=bytes_to_copy;
		byte_offset+=bytes_to_copy;
		bytes_written+=bytes_to_copy;
	}
	
	//debug_printf("File Size = %d\r\n", kfs.files[fd->fd_index].file_size);
	
	return bytes_written;
}

static int kfs_internal_read(int fd_index, unsigned long long byte_offset, void *buffer, unsigned int length)
{
	unsigned int bytes_read=0;
	unsigned int bytes_to_copy;
	unsigned int sector_number;
	
	//debug_printf("kfs_read: file_size=%d, byte_index=%d, length=%d\r\n", kfs.files[fd->fd_index].file_size, fd->byte_index, length);
	disk_state = KFS_SUCCESS;
	
	if ((byte_offset+length)>kfs.files[fd_index].allocated_bytes)
	{
		//debug_printf("kfs_read: writing past end of file\r\n");
		if ((byte_offset+length)>kfs.files[fd_index].allocated_bytes) return 0;
		length=kfs.files[fd_index].allocated_bytes-byte_offset;
	}
	
	if (byte_offset%SECTOR_SIZE)
	{
		//debug_printf("kfs_read: reading from a non-sector aligned index\r\n");

		sector_number=kfs.files[fd_index].sector_start+(byte_offset/SECTOR_SIZE);
		
		if (buffered_sectors[fd_index].sector_number!=sector_number+1)
		{	
			//debug_printf("kfs_read: reading from sector %d\r\n", sector_number);
			buffered_sectors[fd_index].sector_number=sector_number+1;	
			if (kfs_read_sector(buffered_sectors[fd_index].sector, sector_number, 1)!=KFS_SUCCESS)
			{
				debug_printf("kfs_internal_read: Failed reading disk once, going to try again\r\n");
				if (kfs_read_sector(buffered_sectors[fd_index].sector, sector_number, 1)!=KFS_SUCCESS)
				{
					return KFS_BADDISK;
				}
				else
				{
					log_event(EVENT_NUMBER_DISK_201);
				}
			}
				
		}
		else
		{
			//debug_printf("kfs_read: SAVING!  Not reading from sector, already buffered\r\n");
		}
		
		bytes_to_copy=SECTOR_SIZE-(byte_offset%SECTOR_SIZE);
		if (bytes_to_copy>length) bytes_to_copy=length;
		
		//debug_printf("kfs_read: bytes_to_copy = %d\r\n", bytes_to_copy);
		
		memcpy(buffer, buffered_sectors[fd_index].sector+(byte_offset%SECTOR_SIZE), bytes_to_copy);
		bytes_read+=bytes_to_copy;
		buffer = (unsigned char*)buffer + bytes_to_copy;
		byte_offset+=bytes_to_copy;
		length-=bytes_to_copy;
	}
	
	while(length>0)
	{
		sector_number=kfs.files[fd_index].sector_start+(byte_offset/SECTOR_SIZE);
		
		if (buffered_sectors[fd_index].sector_number!=sector_number+1)
		{	
			//debug_printf("kfs_read: reading from sector %d\r\n", sector_number);
			buffered_sectors[fd_index].sector_number=sector_number+1;	
			if (kfs_read_sector(buffered_sectors[fd_index].sector, sector_number, 1)!=KFS_SUCCESS)
			{
				debug_printf("kfs_internal_read: Failed reading disk once, going to try again, tried to read sector %d\r\n", sector_number);
				//extern int extra_debug;
				//extra_debug=1;
				if (kfs_read_sector(buffered_sectors[fd_index].sector, sector_number, 1)!=KFS_SUCCESS)
				{
					debug_printf("kfs_internal_read: Nope...still bad, returning bad disk\r\n");
					return KFS_BADDISK;
				}
				else
				{
					log_event(EVENT_NUMBER_DISK_201);
				}
			}
		}
		else
		{
			//debug_printf("kfs_read: SAVING!  Not reading from sector, already buffered\r\n");
		}
		
		if (length<SECTOR_SIZE) bytes_to_copy=length;
		else                    bytes_to_copy=SECTOR_SIZE;
		
		//debug_printf("kfs_read bytes_to_copy = %d\r\n", bytes_to_copy);
		
		memcpy(buffer, buffered_sectors[fd_index].sector, bytes_to_copy);
		byte_offset+=bytes_to_copy;		
		bytes_read+=bytes_to_copy;
		buffer = (unsigned char*)buffer + bytes_to_copy;
		length-=bytes_to_copy;
	}
	
	return bytes_read;
}

int kfs_read(int fd_index, void *buffer, unsigned int length)
{
    unsigned long long read_index;
    unsigned long long write_index;
    unsigned long long allocated_bytes;
    
    int bytes_read;
    
    unsigned int copy1=0;
    unsigned int copy2=0;
    spi_lock(SPI_LOCK_SD, 1);

    read_index      = kfs.files[fd_index].read_index;
    write_index     = kfs.files[fd_index].write_index;
    allocated_bytes = kfs.files[fd_index].allocated_bytes;

	disk_state = KFS_SUCCESS;

	if (read_index==write_index) { spi_unlock(SPI_LOCK_SD); return 0; }

    if (write_index>read_index)
    {
        copy1=write_index-read_index;
        copy2=0;
        if (copy1>length) copy1=length;
    }
    else
    {
        copy1=allocated_bytes-read_index;
        copy2=write_index;
        
    }
    
    if ((copy1+copy2)>length)
    {
        if (copy1>length)
        {
            copy1=length;
            copy2=0;
        }
        else
        {
            copy2=(length-copy1);
        }
    }

    //SysCtlDelay(system_clock_speed/600000);
    if (copy1>0)
    {
	    if ((bytes_read=kfs_internal_read(fd_index, read_index, buffer, copy1))!=copy1)
	    {
	    	if (bytes_read<0) disk_state=(KFS_RET)bytes_read;
	    	debug_printf("kfs_read: ERROR: copy1 failed: copy1=%d, bytes_read=%d\r\n", copy1, bytes_read);
	    	spi_unlock(SPI_LOCK_SD);
	    	return 0;
	    }
    }
    
    length-=copy1;
    read_index+=copy1;
    if (read_index>=allocated_bytes) read_index=0;

	if (copy2>0)
	{    
	    //debug_printf("READ2: read_index=%d, copy2=%d\r\n", read_index, copy2);
	    if ((bytes_read=kfs_internal_read(fd_index, read_index, ((unsigned char*)buffer)+copy1, copy2))!=copy2)
	    {
	    	if (bytes_read<0) disk_state=(KFS_RET)bytes_read;
	    	debug_printf("kfs_read: ERROR: copy2 failed: copy2=%d, bytes_read=%d\r\n", copy2, bytes_read);
	    	spi_unlock(SPI_LOCK_SD);
	    	return 0;
	    }
	}
    length-=copy2;
    read_index+=copy2;
    if (read_index>=allocated_bytes) read_index=0;
    
    kfs.files[fd_index].read_index=read_index;
    
    //debug_printf("kfs: copy1=%d, copy2=%d\r\n", copy1, copy2);
    //debug_printf("kfs_read END: start=%d, read=%d, write=%d, size=%d\r\n\r\n", kfs.files[fd_index].start_index, kfs.files[fd_index].read_index, kfs.files[fd_index].write_index, kfs.files[fd_index].file_size);
	spi_unlock(SPI_LOCK_SD);
    return copy1+copy2;
}

int kfs_write(int fd_index, void *buffer, unsigned int length)
{
	unsigned long long start_index;
    unsigned long long write_index;
    unsigned long long file_size;
    unsigned long long allocated_bytes;
    unsigned long long next;
    int bytes_written;
    
    int copy1=0;
    int copy2=0;

	spi_unlock(SPI_LOCK_SD);
	start_index     = kfs.files[fd_index].start_index;
    write_index     = kfs.files[fd_index].write_index;
    file_size       = kfs.files[fd_index].file_size;
    allocated_bytes = kfs.files[fd_index].allocated_bytes;
    
    disk_state = KFS_SUCCESS;
    
    if (length>(allocated_bytes-file_size-1)) length=(allocated_bytes-file_size-1);
    
    if (length==1)
    {
        next=write_index+1;
        if (next>=allocated_bytes) next=0;
        if (next==start_index) { spi_unlock(SPI_LOCK_SD); return 0; }
        kfs_internal_write(fd_index, next, buffer, 1);
        kfs.files[fd_index].write_index=next;
        kfs.files[fd_index].file_size++;
        spi_unlock(SPI_LOCK_SD);
        return 1;
    }
            
    // 1: copy from write_index first_copy bytes
    // 2: copy from 0           second_copy bytes
    
    
    if (start_index>write_index)
    {
        copy1=start_index-write_index-1;
        if (copy1>length) copy1=length;
        copy2=0;
    }
    else if (write_index>=start_index)
    {
        copy1=allocated_bytes-write_index;
        copy2=start_index;
        
        if (start_index==0) copy1--;
        else                copy2--;
        
        if ((copy1+copy2)>length)
        {
            if (copy1>length)
            {
                copy1=length;
                copy2=0;
            }
            else
            {
                copy2=(length-copy1);
            }
        }
    }
    
    if ((copy1+copy2)!=length)
    {
    	debug_printf("kfs_write_1: ERROR: copy1=%d, copy2=%d, length=%d\r\n", copy1, copy2, length);
    	spi_unlock(SPI_LOCK_SD);
    	return 0;
    }
    
    //debug_printf("Writing (copy1) %d bytes, write_index=%d, copy1=%d, copy2=%d\r\n", length, write_index, copy1, copy2);
    
	if (copy1>0)
	{    
	    if ((bytes_written=kfs_internal_write(fd_index, write_index, buffer, copy1))!=copy1)
	    {
	    	if (bytes_written<0) disk_state=(KFS_RET)bytes_written;
	    	debug_printf("kfs_write_2: ERROR on copy1: copy1=%d, bytes_written=%d\r\n", copy1, bytes_written);
	    	spi_unlock(SPI_LOCK_SD);
	    	return 0;
	    }
	}
    

    length-=copy1;
    write_index+=copy1;
    if (write_index>=allocated_bytes) write_index=0;
    
    if (copy2>0)
    {
    	//debug_printf("Writing (copy2) %d bytes, write_index=%d, copy1=%d, copy2=%d\r\n", length, write_index, copy1, copy2);
	    if ((bytes_written=kfs_internal_write(fd_index, write_index, ((unsigned char*)buffer)+copy1, copy2))!=copy2)
	    {
	    	if (bytes_written<0) disk_state=(KFS_RET)bytes_written;
	    	debug_printf("kfs_write_2: ERROR on copy2: copy2=%d, bytes_written=%d\r\n", copy2, bytes_written);
	    	spi_unlock(SPI_LOCK_SD);
	    	return 0;
	    }
    }
    
    length-=copy2;
    write_index+=copy2;
    if (write_index>=allocated_bytes) write_index=0;
    kfs.files[fd_index].write_index=write_index;
    kfs.files[fd_index].file_size+=(copy1+copy2);
    
    //debug_printf("kfs_write END: start=%d, read=%d, write=%d, size=%d\r\n\r\n", kfs.files[fd_index].start_index, kfs.files[fd_index].read_index, kfs.files[fd_index].write_index, kfs.files[fd_index].file_size);
	spi_unlock(SPI_LOCK_SD);
    return copy1+copy2;
}

char *kfs_gets(int fd_index, char *buffer, unsigned int max_length)
{
	int length = 0;
	unsigned char c;
	char *p1 = buffer;
	char s[2];

	// Read bytes until buffer gets filled
	while (length < max_length - 1) 
	{
		if (kfs_read(fd_index, s, 1)!=1) break;  // Break on EOF or error
		c = s[0];

		if (c == '\r') continue;	// Strip '\r'
		*p1++ = c;
		length++;
		if (c == '\n') break;		// Break on EOL
	}
	*p1 = '\0';
	return length ? buffer : NULL;			// When no data read (eof or error), return with error.	
}

void kfs_print_stats(void)
{
	char size_str1[20];
	
	if (kfs.kfs_magic!=KFS_MAGIC)
	{
		debug_printf("KFS invalid MAGIC\r\n");
		return;
	}
	
	if (kfs.kfs_version!=KFS_VERSION)
	{
		debug_printf("KFS invalid VERSION\r\n");
		return;
	}
	
	debug_printf("MAGIC:        %s\r\n", (unsigned char*)&kfs.kfs_magic);
	debug_printf("VERSION:      %s\r\n", (unsigned char*)&kfs.kfs_version);
	debug_printf("Sector Count: %d\r\n", kfs.sector_count);
	debug_printf("Sector Size:  %d\r\n", SECTOR_SIZE);
	
	debug_printf("FIRMWARE: %8lld-%8lld (%8d) %8lldb / %s\r\n", kfs.files[KFS_FIRMWARE_FD_INDEX].sector_start, kfs.files[KFS_FIRMWARE_FD_INDEX].sector_start+kfs.files[KFS_FIRMWARE_FD_INDEX].sector_count-1, kfs.files[KFS_FIRMWARE_FD_INDEX].sector_count, kfs.files[KFS_FIRMWARE_FD_INDEX].file_size, kfs_size_str(kfs.files[KFS_FIRMWARE_FD_INDEX].allocated_bytes, size_str1));
	debug_printf("CONFIG:   %8lld-%8lld (%8d) %8lldb / %s\r\n", kfs.files[KFS_CONFIG_FD_INDEX].sector_start,   kfs.files[KFS_CONFIG_FD_INDEX].sector_start  +kfs.files[KFS_CONFIG_FD_INDEX].sector_count  -1, kfs.files[KFS_CONFIG_FD_INDEX].sector_count,   kfs.files[KFS_CONFIG_FD_INDEX].file_size,   kfs_size_str(kfs.files[KFS_CONFIG_FD_INDEX].allocated_bytes,   size_str1));
	debug_printf("EVENT     %8lld-%8lld (%8d) %8lldb / %s\r\n", kfs.files[KFS_EVENT_FD_INDEX].sector_start,    kfs.files[KFS_EVENT_FD_INDEX].sector_start   +kfs.files[KFS_EVENT_FD_INDEX].sector_count   -1, kfs.files[KFS_EVENT_FD_INDEX].sector_count,    kfs.files[KFS_EVENT_FD_INDEX].file_size,    kfs_size_str(kfs.files[KFS_EVENT_FD_INDEX].allocated_bytes,    size_str1));
	debug_printf("LOG       %8lld-%8lld (%8d) %8lldb / %s\r\n", kfs.files[KFS_LOG_FD_INDEX].sector_start,      kfs.files[KFS_LOG_FD_INDEX].sector_start     +kfs.files[KFS_LOG_FD_INDEX].sector_count     -1, kfs.files[KFS_LOG_FD_INDEX].sector_count,      kfs.files[KFS_LOG_FD_INDEX].file_size,      kfs_size_str(kfs.files[KFS_LOG_FD_INDEX].allocated_bytes,      size_str1));
}

char *kfs_strerror(KFS_RET error)
{
	switch(error)
	{
		case KFS_SUCCESS: 					return "KFS_SUCCESS";
		case KFS_BADDISK:					return "KFS_BADDISK";
		case KFS_READ_ERROR:				return "KFS_READ_ERROR";
		case KFS_WRITE_ERROR:				return "KFS_WRITE_ERROR";
		case KFS_SEEK_ERROR:				return "KFS_SEEK_ERROR";
		case KFS_BAD_VERSION:				return "KFS_BAD_VERSION";
		case KFS_UNFORMATTED:				return "KFS_UNFORMATTED";
		case KFS_MISMATCH_SECTOR_COUNT:		return "KFS_MISMATCH_SECTOR_COUNT";
		case KFS_UNKNOWN_FILE:				return "KFS_UNKNOWN_FILE";
		case KFS_NOT_INSTALLED:				return "KFS_NOT_INSTALLED";
		default:							return "KFS_UNKNOWN";
	}
}

/***   End Of File   ***/
