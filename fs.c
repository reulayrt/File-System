// Copyright 12/12/2022, Ayrton Reulet - reulayrt@bu.edu

#include "disk.h"
/* 
    - 8,192 Disk Blocks
    - 4096 is the size of each Block
*/
#include <sys/types.h>
#include <limits.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

int make_fs(char *disk_name);
int mount_fs(char *disk_name);
int umount_fs(char *disk_name);
int fs_open(char *name);
int fs_close(int fildes);
int fs_create(char *name);
int fs_delete(char *name);
int fs_read(int fildes, void *buf, size_t nbyte);
int fs_write(int fildes, void *buf, size_t nbyte);
int fs_get_filesize(int fildes);
int fs_listfiles(char ***files);
int fs_lseek(int fildes, off_t offset);
int fs_truncate(int fildes, off_t length);

#define MAX_SIZE 1024*1024

struct supaBlock {  // $$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$ might need to use int? $$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$
    uint16_t used_block_bitmap_count;
    uint16_t used_block_bitmap_offset;
    uint16_t fat_blocks;  //Length of FAT in blocks
    uint16_t fat_index;  // First block of the FAT
    uint16_t dir_blocks;  // Length of directory in blocks
    uint16_t dir_index;  // First block of directory
};

struct FAT {  // $$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$ might need to use int? $$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$
    uint16_t file_type;
    uint16_t dir_offset;
    uint16_t single_out_offset[512];
    uint16_t fileSize;
    uint16_t blocks;
};

struct directory_entry {
    bool occupodo;  // Is this file - "slot" occupied
    unsigned int fatNum;
    char name[15];  // Might need to be 16?
    int ref_count;  // how many open file descriptors are there?
                    // ref_count > 0 -> connont delete file
};

struct fd {
    bool occupodo;
    unsigned int fatNum;
    unsigned int offset;
};


/* Global Variables Delclaration */
struct FAT* fatTable;  // The maximum number of file is 64
struct directory_entry* directory_entry_table;  // There can a maximum of 64 possible files
struct fd* file_descriptors;  // The maximum number of file descriptors is 32
struct supaBlock* supaBlock;

int fd_count = 0;

// There are 8,192 possible blocks
uint8_t* block_bitmap;


/*
    Helper Functions
*/
static bool get_bit(uint8_t* bitmap, int block) {
    int idx = block / CHAR_BIT;
    int block_idx = block % CHAR_BIT;
    return (bitmap[idx] & (1 << block_idx));
}

static void set_bit(uint8_t* bitmap, int block) {
    int idx = block / CHAR_BIT;
    int block_idx = block % CHAR_BIT;
    bitmap[idx] = bitmap[idx] | (1 << block_idx); 
}

static void clear_bit(uint8_t* bitmap, int block) {
    int idx = block / CHAR_BIT;
    int block_idx = block % CHAR_BIT;
    bitmap[idx] = bitmap[idx] & ~(1 << block_idx);
}


// ************ MANAGEMENT ROUTINES ************ //


// Creates a fresh (and empty) file system on the virtual disk with name disk_name
int make_fs(char *disk_name) {
    if (make_disk(disk_name)) return -1;
    if (open_disk(disk_name)) return -1;
    
    supaBlock = calloc(1, sizeof(struct supaBlock));  // First step is to initialize the super block
    
    supaBlock->used_block_bitmap_count = 1;
    supaBlock->used_block_bitmap_offset = 1;
    
    supaBlock->fat_blocks = 1;
    supaBlock->fat_index = 2;
    
    supaBlock->dir_blocks = 1;
    supaBlock->dir_index = 3;
    

    /*
        8182/8 = 1024 bytes, 1024 bytest / bytes = 256 initializations
        CHAR_BIT = 2 bit, 8 bit / 2 bits = 4 bytes
    */
    block_bitmap = calloc(DISK_BLOCKS/CHAR_BIT, sizeof(uint8_t));
    
    fatTable = calloc(64, sizeof(struct FAT));
    
    // Here we initialize the FAT table
    for (int i = 0; i < 64; i++) {
        fatTable[i].fileSize = 0;
        fatTable[i].blocks = 0;
    }
    
    directory_entry_table = calloc(64, sizeof(struct directory_entry));
    
    // Here we initalize the Directory Entry Table
    for (int i = 0; i < 64; i++) {  // FATs can be mapped here which might make things easier
        directory_entry_table[i].occupodo = false;
        directory_entry_table[i].ref_count = 0;
        directory_entry_table[i].fatNum = i;
    }
    
    // Initializing the bitmap
    set_bit(block_bitmap, 0);
    set_bit(block_bitmap, 1);
    set_bit(block_bitmap, 2);
    set_bit(block_bitmap, 3);
    
    /*
        Now that bitmap is initialized we can write the super block to virtual disk
    */
    if (block_write(0, (char *) supaBlock)) return -1;
    if (block_write(supaBlock->used_block_bitmap_offset, (char *) block_bitmap)) return -1;
    if (block_write(supaBlock->fat_index, (char *) fatTable)) return -1;
    if (block_write(supaBlock->dir_index, (char *) directory_entry_table)) return -1;
    
    // Free the allocated memory we created for saving to virtual disk
    free(supaBlock);
    free(block_bitmap);
    free(fatTable);
    free(directory_entry_table);
    free(file_descriptors);  // File descriptors were not behaving as expected, I hope this fixes it
    
    // Error checking closing the disk
    if (close_disk()) return -1;
    
    return 0;  // Return 0 on success
}

// Mounts a file system that is stored on a virtual disk with name disk_name
int mount_fs(char *disk_name) {
    // Check if open disk creates an error
    if (open_disk(disk_name)) return -1;
    
    /*
        Initializing the virtual disk for everything
        by allocating the necessary mem for each
    */
    supaBlock = calloc(1, sizeof(struct supaBlock));
    block_bitmap = calloc(DISK_BLOCKS/CHAR_BIT, sizeof(uint8_t));
    fatTable = calloc(64, sizeof(struct FAT));
    directory_entry_table = calloc(64, sizeof(struct directory_entry));
    file_descriptors = calloc(32, sizeof(struct fd));
    
    // Here we are loading all the meta information that we will be needing
    if (block_read(0, (char*) supaBlock)) return -1;
    if (block_read(supaBlock->used_block_bitmap_offset, (char *) block_bitmap)) return -1;
    if (block_read(supaBlock->fat_index, (char *) fatTable)) return -1;
    if (block_read(supaBlock->dir_index, (char *) directory_entry_table)) return -1;
    
    for (int i = 0; i < 32; i++) {  // Initialize the file_descriptors
        file_descriptors[i].occupodo = false;
        file_descriptors[i].offset = 0;
    }
    
    return 0;  // Return 0 on success
}

// unmounts your file system from a virtual disk with name disk_name
int umount_fs(char *disk_name) {

    /*
        We have to write all the meta data back so that the disk can contantly show the changes made to the file systme
    */
    if (block_write(0, (char *) supaBlock)) return -1;
    if (block_write(supaBlock->used_block_bitmap_offset, (char *) block_bitmap)) return -1;
    if (block_write(supaBlock->fat_index, (char *) fatTable)) return -1;
    if (block_write(supaBlock->dir_index, (char *) directory_entry_table)) return -1;
    
    
    // Check error on closing disk
    if (close_disk(disk_name)) return -1;
    
    return 0;  // return 0 on success
}

/*
    The file specified by name is opened for reading and writing, and the file descriptor
    corresponding to this file is returned to the calling function
*/
int fs_open(char *name) {
    int file_count;
    
    // If the directory entry is used and the name matched the current iteration use that iteration
    for (file_count = 0; file_count < 64; file_count++) {
        if (directory_entry_table[file_count].occupodo && !strcmp(name, directory_entry_table[file_count].name))
            break;
    }
    
    // If there is no file with that name return error
    if (file_count == 64) return -1;
    
    // There is already a file descriptor created
    if (fd_count >= 32) return -1;
    
    int curr_fd;
    
    for (curr_fd = 0; curr_fd < 32; curr_fd++) {
        if (!file_descriptors[curr_fd].occupodo) break;
    }
    
    // If there isn't an inactive file descriptor
    if (curr_fd == 32) return -1;
    
    /*
        Here we are initializing the file descriptor
    */
    file_descriptors[curr_fd].occupodo = true;
    file_descriptors[curr_fd].fatNum = directory_entry_table[file_count].fatNum;
    // Setting the offset to the start of the file
    file_descriptors[curr_fd].offset = 0;
    directory_entry_table[curr_fd].ref_count++;
    fd_count++;
    
    return curr_fd;  // Return the file desctiptor on success
}

// The file descriptor fildes is closed.
int fs_close(int fildes) {
    // If file descriptor is inactive or doesn't exit return error
    if (fildes < 0 || fildes >= 32 || !file_descriptors[fildes].occupodo) 
        return -1;
    
    file_descriptors[fildes].occupodo = false;  // Close file descriptor
    fd_count--;  // Decrement the file descriptor count
    
    // Going through to decrement the ref_count var
    for (int i = 0; i < 64; i++) {
        if (file_descriptors[fildes].fatNum == directory_entry_table[fildes].fatNum)
            directory_entry_table[fildes].ref_count--;
    }
    
    return 0;  // Return 0 on success
}

// Creates a new file with name name in the root directory of your file system.
int fs_create(char *name) {

    // Return error if file name is longer than max allowed (16)
    if (strlen(name) > 15) return -1;
    
    // Checking to see if file is alread in the directory entry table
    for (int i = 0; i < 64; i++) {
        if (directory_entry_table[i].occupodo && !strcmp(name, directory_entry_table[i].name))
            return -1;
    }
    
    int idx;
    for (idx = 0; idx < 64; idx++) {  // Finding an open slot in the directory entry table
        if (!directory_entry_table[idx].occupodo) 
            break;
    }
    
    // File cannot exceed the max file limit of 64
    if (idx == 64) return -1;
    
    // making the name == "name" the input of funtion
    strcpy(directory_entry_table[idx].name, name);
    directory_entry_table[idx].occupodo = true;
    
    return 0;  // Return 0 on success
}

/*
    Deletes the file with name name from the root directory of your file system and
    frees all data blocks and meta-information that correspond to that file. 
*/
int fs_delete(char *name) {
    int idx;
    // Searching for the name specified in the directory_entry_table
    // Must be active as well
    for (idx = 0; idx < 64; idx++) {
        if (directory_entry_table[idx].occupodo && !strcmp(name, directory_entry_table[idx].name))
            break;
    }
    
    // Return error is name not found
    if (idx == 64) return -1;
    
    unsigned int fatNum = directory_entry_table[idx].fatNum;
    
    int fd_idx;
    for (fd_idx = 0; fd_idx < 32; fd_idx++) {  // Search for file descriptor
        // check if file is open, error if open
        if (file_descriptors[fd_idx].occupodo && fatNum == file_descriptors[fd_idx].fatNum)
            return -1;
    }
    
    directory_entry_table[idx].occupodo = false;  // No longer active
    
    return 0;  // Return 0 on success
}

/*
    Attempts to read nbyte bytes of data from the file referenced by the descriptor
    fildes into the buffer pointed to by buf
*/
int fs_read(int fildes, void *buf, size_t nbyte) {
    // If file descriptor is inactive or doesn't exit return error
    if (fildes < 0 || fildes >= 32 || !file_descriptors[fildes].occupodo) 
        return -1;
    
    unsigned int fatNum = file_descriptors[fildes].fatNum;
    
    // Only read nbyte upto the size of the file
    if (nbyte > fatTable[fatNum].fileSize)
        nbyte = fatTable[fatNum].fileSize;
    
    unsigned int file_offset = file_descriptors[fildes].offset;
    int block_idx = 0;
    
    if (file_offset > fatTable[fatNum].fileSize) return -1;
    
    while (file_offset > BLOCK_SIZE) {
        file_offset -= BLOCK_SIZE;
        block_idx++;
    }
    
    // If the offset is greater than the number of blocks available
    if (block_idx > fatTable[fatNum].blocks) return -1;  // $$$$$$$$$$$$$$$$$$$$$$$$$$ Might edit for extra credit
    
    unsigned int first_block = fatTable[fatNum].single_out_offset[block_idx];
    int left = nbyte;
    
    char current_block[BLOCK_SIZE];
    int bytes_read = 0;
    
    char str[nbyte+1];
    memset(str, 0, nbyte+1);
    
    while (left > 0) {
        if (block_read(first_block, current_block)) return -1;  // Attempting to read current block
        
        for (int i = file_offset; i < fatTable[fatNum].fileSize; i++) {  // Here we read the current block into buf
            str[bytes_read] = current_block[i];
            bytes_read++;
            file_descriptors[fildes].offset++;
            left--;
            if (left == 0) break;
        }
        
        if (left > 0 && file_descriptors[fildes].offset != fatTable[fatNum].fileSize) {
            file_offset = 0;
            block_idx++;
            first_block = fatTable[fatNum].single_out_offset[block_idx];
        }
        else break;
    }
    
    str[bytes_read] = '\0';
    
    memcpy(buf, str, nbyte+1);
    memcpy(buf, buf, nbyte);
    
    return bytes_read;
}

/*
    Attempts to write nbyte bytes of data to the file referenced by the descriptor fildes
    from the buffer pointed to by buf
*/
int fs_write(int fildes, void *buf, size_t nbyte) {
    // If file descriptor is inactive or doesn't exit return error
    if (fildes < 0 || fildes >= 32 || !file_descriptors[fildes].occupodo) 
        return -1;
    
    unsigned int fatNum = file_descriptors[fildes].fatNum;
    
    // check if nbyte+fileSize > max file size (1Mib)
    // Making sure that nbyte + the current fileSize isn't > 1MB
    if (nbyte + fatTable[fatNum].fileSize > MAX_SIZE)
        nbyte = MAX_SIZE - fatTable[fatNum].fileSize;
    
    int num_blocks_needed = nbyte / BLOCK_SIZE;
    if (nbyte % BLOCK_SIZE) ++num_blocks_needed;  // what blocks are needed
    
    while (fatTable[fatNum].blocks < num_blocks_needed) { // Allocating the perfect amount of blocks
        int idx;
        for (idx = 0; idx < DISK_BLOCKS; idx++) {
            if (!get_bit(block_bitmap, idx)) {
                fatTable[fatNum].single_out_offset[fatTable[fatNum].blocks] = idx;
                set_bit(block_bitmap, idx);
                fatTable[fatNum].blocks++;
                break;
            }
        }
        // If no more free blocks
        if (idx == DISK_BLOCKS) break;
    }
    
    unsigned int file_offset = file_descriptors[fildes].offset;
    unsigned int first_block = fatTable[fatNum].single_out_offset[0];
    int block_idx = 0;
    
    while (file_offset > BLOCK_SIZE) {  // Here we are relocating the beginning block
        file_offset -= BLOCK_SIZE;
        block_idx++;
        first_block = fatTable[fatNum].single_out_offset[block_idx];
    }
    
    int left = nbyte;
    char current_block[BLOCK_SIZE];
    int bytes_written;
    
    while (left > 0) {
        if (block_read(first_block, current_block)) return -1;
        
        for (int i = file_offset; i < BLOCK_SIZE; i++) {
            current_block[i] = *((char*) buf + bytes_written);
            bytes_written++;
            file_descriptors[fildes].offset++;
            left--;
            if (left == 0) break;
        }
        
        if (block_write(first_block, current_block)) return -1;
        
        // After than block is finished check if there is still stuff left to write
        if (left > 0) {
            file_offset = 0;
            block_idx++;
            first_block = fatTable[fatNum].single_out_offset[block_idx];
        }
    }
        
    fatTable[fatNum].fileSize += nbyte;  // Adjusting the *new file size
    
    return bytes_written;
}

//  Returns the current size of the file referenced by the file descriptor fildes
int fs_get_filesize(int fildes) {
    // If file descriptor is inactive or doesn't exit return error
    if (fildes < 0 || fildes >= 32 || !file_descriptors[fildes].occupodo) 
        return -1;

    for (int i = 0; i < 64; i++) {
        if (directory_entry_table[i].fatNum == file_descriptors[fildes].fatNum) {
            return fatTable[directory_entry_table[i].fatNum].fileSize;
        }
    }
    
    return -1;
}

// Creates and populates an array of all filenames currently known to the file system
int fs_listfiles(char ***files) {
   char* list[64];  // Making a list to grab all files initially
    
    int idx = 0;  // Need idx along with i because idx isn't guarunteed to increase
    for (int i = 0; i < 64; i++) {
        if (directory_entry_table[i].occupodo) {
            list[idx] = directory_entry_table[i].name;
            idx++;
        }
    }

    char* file_list[idx+1];  // Creating the array of the files
    
    // Adding to list and making the last byte NULL
    for (int i = 0; i < idx; i++)
        file_list[i] = list[i];
    
    file_list[idx] = NULL;
    
    *files = file_list;

    return 0;  // return 0 on sucecss
}

/*
    This function sets the file pointer (the offset used for read and write operations) associated with
    the file descriptor fildes to the argument offset.
*/
int fs_lseek(int fildes, off_t offset) {
    // error if fd doesnt exist or if it is inactive
    if (fildes < 0 || fildes >= 32 || !file_descriptors[fildes].occupodo) 
        return -1;
    
    unsigned int pos = file_descriptors[fildes].fatNum;
    
    // error if offset > file or < 0
    if (offset > fatTable[pos].fileSize || offset < 0) return -1;
    
    file_descriptors[fildes].offset = offset;
    
    return 0;
}

/*
    This function causes the file referenced by fildes to be truncated to length bytes in size.
*/
int fs_truncate(int fildes, off_t length) {
    // If file descriptor is inactive or doesn't exit return error
    if (fildes < 0 || fildes >= 32 || !file_descriptors[fildes].occupodo) 
        return -1;
    
    // The length specified is NOT in bounds
    if (length < 0 || length > DISK_BLOCKS * BLOCK_SIZE) return -1;
    
    unsigned int fatNum = file_descriptors[fildes].fatNum;
    
    // Error is length is greater than the file size
    if (length > fatTable[fatNum].fileSize) return -1;
    
    if (file_descriptors[fildes].offset > length)
        file_descriptors[fildes].offset = length;
    
    int num_blocks_needed = length / BLOCK_SIZE;
    // Extra block for remainder
    if (length % BLOCK_SIZE) ++num_blocks_needed;
    
    int num_blocks_current = fatTable[fatNum].blocks;
    

    // Now we can get rid of the excess
    if (num_blocks_current > num_blocks_needed) {
        clear_bit(block_bitmap, num_blocks_current-1);
        num_blocks_current--;
    }
    
    fatTable[fatNum].blocks = num_blocks_needed;
    fatTable[fatNum].fileSize = length;
    
    return 0;
}