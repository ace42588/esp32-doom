/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *  Misc system stuff needed by Doom, implemented for Linux.
 *  Mainly timer handling, and ENDOOM/ENDBOOM.
 *
 *-----------------------------------------------------------------------------
 */

 #include <stdio.h>

 #include <stdarg.h>
 #include <stdlib.h>
 #include <ctype.h>
 #include <signal.h>
 #ifdef _MSC_VER
 #define    F_OK    0    /* Check for file existence */
 #define    W_OK    2    /* Check for write permission */
 #define    R_OK    4    /* Check for read permission */
 #include <io.h>
 #include <direct.h>
 #else
 #include <unistd.h>
 #endif
 #include <sys/stat.h>
 
 
 
 #include "config.h"
 #include <unistd.h>
 #include <sched.h>
 #include <fcntl.h>
 #include <sys/stat.h>
 #include <errno.h>
 
 #include "m_argv.h"
 #include "lprintf.h"
 #include "doomtype.h"
 #include "doomdef.h"
 #include "lprintf.h"
 #include "m_fixed.h"
 #include "r_fps.h"
 #include "i_system.h"
 #include "i_joy.h"
 
 #include "freertos/FreeRTOS.h"
 #include "freertos/task.h"
 
 #include "esp_partition.h"
 #include "esp_err.h"
 #include "esp_log.h"
 
 #ifdef __GNUG__
 #pragma implementation "i_system.h"
 #endif
 #include "i_system.h"
 
 #include <sys/time.h>
 #include <stdbool.h> // Added for bool type
 
 int realtime=0;
 
 void I_uSleep(unsigned long usecs)
 {
	 vTaskDelay(usecs/1000);
 }
 
 static unsigned long getMsTicks() {
   struct timeval tv;
   struct timezone tz;

   gettimeofday(&tv, &tz);
 
   //convert to ms
   unsigned long now = tv.tv_usec/1000+tv.tv_sec*1000;
   return now;
 }
 
 int I_GetTime_RealTime (void)
 {
   struct timeval tv;
   struct timezone tz;
   unsigned long thistimereply;
 
   gettimeofday(&tv, &tz);
 
   thistimereply = (tv.tv_sec * TICRATE + (tv.tv_usec * TICRATE) / 1000000);
 
   return thistimereply;
 
 }
 
 const int displaytime=0;
 
 fixed_t I_GetTimeFrac (void)
 {
   unsigned long now;
   fixed_t frac;
 
   now = getMsTicks();
 
   if (tic_vars.step == 0)
	 return FRACUNIT;
   else
   {
	 frac = (fixed_t)((now - tic_vars.start + displaytime) * FRACUNIT / tic_vars.step);
	 if (frac < 0)
	   frac = 0;
	 if (frac > FRACUNIT)
	   frac = FRACUNIT;
	 return frac;
   }
 }
 
 
 void I_GetTime_SaveMS(void)
 {
   if (!movement_smooth)
	 return;
 
   tic_vars.start = getMsTicks();
   tic_vars.next = (unsigned int) ((tic_vars.start * tic_vars.msec + 1.0f) / tic_vars.msec);
   tic_vars.step = tic_vars.next - tic_vars.start;
 }
 
 unsigned long I_GetRandomTimeSeed(void)
 {
	 return 4; //per https://xkcd.com/221/
 }
 
 const char* I_GetVersionString(char* buf, size_t sz)
 {
   sprintf(buf,"%s v%s (http://prboom.sourceforge.net/)",PACKAGE,VERSION);
   return buf;
 }
 
 const char* I_SigString(char* buf, size_t sz, int signum)
 {
   return buf;
 }
 
 extern unsigned char *doom1waddata;
 
 typedef struct {
	 const esp_partition_t* part;
	 esp_partition_mmap_handle_t handle;
	 const void *mmap_ptr;
	 int offset;
	 bool is_open;  // Track if this descriptor is actually open
 } FileDesc;
 
 const char* flash_wads[] = {
	 "DOOM1.WAD",
	 "doom2.wad",
	 "prboom-plus.wad"
};

const char* flash_gwa_files[] = {
	 "DOOM1.GWA",
	 "doom2.gwa",
	 "prboom-plus.gwa"
};

#define MAX_N_FILES 4  // Increased from 2 to handle more concurrent files
static FileDesc fds[MAX_N_FILES];
static bool fds_initialized = false;

// Initialize file descriptor array
static void init_fds(void) {
	if (fds_initialized) return;
	
	for (int i = 0; i < MAX_N_FILES; ++i) {
		fds[i].part = NULL;
		fds[i].handle = 0;
		fds[i].mmap_ptr = NULL;
		fds[i].offset = 0;
		fds[i].is_open = false;
	}
	fds_initialized = true;
	lprintf(LO_INFO, "File descriptors initialized\n");
}

// Validate file descriptor
static bool is_valid_fd(int fd) {
	return fd >= 0 && fd < MAX_N_FILES && fds[fd].is_open && fds[fd].part != NULL;
}

// Clean up a file descriptor completely
static void cleanup_fd(int fd) {
	if (fd < 0 || fd >= MAX_N_FILES) return;
	
	if (fds[fd].is_open && fds[fd].handle != 0) {
		esp_partition_munmap(fds[fd].handle);
	}
	
	fds[fd].part = NULL;
	fds[fd].handle = 0;
	fds[fd].mmap_ptr = NULL;
	fds[fd].offset = 0;
	fds[fd].is_open = false;
}
 
 int I_Open(const char *wad, int flags) {
	 // Ensure file descriptors are initialized
	 init_fds();
	 
	 FileDesc *file = NULL;
	 int fd = -1;
	 lprintf(LO_INFO, "I_Open: trying to open %s\n", wad);
	 
	 // Find a free file descriptor
	 for (int i = 0; i < MAX_N_FILES; ++i) {
		 if (!fds[i].is_open) {
			 file = &fds[i];
			 fd = i;
			 lprintf(LO_INFO, "I_Open: found free fd %d\n", fd);
			 break;
		 }
	 }
 
	 if (file == NULL) {
		 lprintf(LO_ERROR, "I_Open: no free file descriptors available for %s\n", wad);
		 return -1;
	 }
 
	 // Check if the requested WAD is in our supported list
	 int wad_index = -1;
	 for (int i = 0; i < sizeof(flash_wads)/sizeof(flash_wads[0]); ++i) {
		 if (!strcasecmp(wad, flash_wads[i])) {
			 wad_index = i;
			 break;
		 }
	 }
	 
	 // Check if this is a .gwa file (graphics cache)
	 int gwa_index = -1;
	 for (int i = 0; i < sizeof(flash_gwa_files)/sizeof(flash_gwa_files[0]); ++i) {
		 if (!strcasecmp(wad, flash_gwa_files[i])) {
			 gwa_index = i;
			 break;
		 }
	 }
	 
	 if (wad_index >= 0) {
		 // Use the wad partition (type 66, subtype 6) for all WAD files
		 file->part = esp_partition_find_first(66, 6, NULL);
		 if (!file->part) {
			 lprintf(LO_ERROR, "I_Open: Failed to find WAD partition (type 66, subtype 6)\n");
			 return -1;
		 }
		 lprintf(LO_INFO, "I_Open: Found WAD partition at offset 0x%lx, size %lu\n", 
				 file->part->address, file->part->size);
		 file->offset = 0;
	 } else if (gwa_index >= 0) {
		 // .gwa files are not supported on ESP32, return "file not found"
		 lprintf(LO_INFO, "I_Open: .gwa files not supported on ESP32: %s\n", wad);
		 return -1;
	 } else {
		 lprintf(LO_INFO, "I_Open: unsupported file %s\n", wad);
		 return -1;
	 }
 
	 if (!file->part) {
		 lprintf(LO_INFO, "I_Open: open %s failed\n", wad);
		 return -1;
	 }
 
	 ESP_LOGD("i_system", "mmaping %s of size %lu", wad, file->part->size);
	 esp_err_t ret = esp_partition_mmap(file->part, 0, file->part->size, ESP_PARTITION_MMAP_DATA, &file->mmap_ptr, &file->handle);
	 if (ret != ESP_OK) {
		 lprintf(LO_ERROR, "I_Open: Failed to mmap partition: %s\n", esp_err_to_name(ret));
		 cleanup_fd(fd);
		 return -1;
	 }
	 
	 // Mark the file descriptor as open
	 file->is_open = true;
	 
	 ESP_LOGI("i_system", "%s @%p", wad, file->mmap_ptr);
	 lprintf(LO_INFO, "I_Open: successfully opened %s (size: %lu bytes)\n", wad, file->part->size);
	 
	 return fd;
 }
 
 int I_Lseek(int ifd, off_t offset, int whence) {
	 if (!is_valid_fd(ifd)) {
		 lprintf(LO_ERROR, "I_Lseek: invalid file descriptor %d\n", ifd);
		 return -1;
	 }
	 
	 if (whence == SEEK_SET) {
		 fds[ifd].offset = offset;
	 } else if (whence == SEEK_CUR) {
		 fds[ifd].offset += offset;
	 } else if (whence == SEEK_END) {
		 fds[ifd].offset = fds[ifd].part->size + offset;
	 }
	 
	 // Ensure offset doesn't go negative
	 if (fds[ifd].offset < 0) fds[ifd].offset = 0;
	 
	 return fds[ifd].offset;
 }
 
 int I_Filelength(int ifd)
 {
	 if (!is_valid_fd(ifd)) {
		 lprintf(LO_ERROR, "I_Filelength: invalid file descriptor %d\n", ifd);
		 return -1;
	 }
	 return fds[ifd].part->size;
 }
 
 void I_Close(int fd) {
	 lprintf(LO_INFO, "I_Close: closing fd %d\n", fd);
	 
	 if (!is_valid_fd(fd)) {
		 lprintf(LO_WARN, "I_Close: invalid file descriptor %d\n", fd);
		 return;
	 }
	 
	 cleanup_fd(fd);
}
 
 void *I_Mmap(void *addr, size_t length, int prot, int flags, int ifd, off_t offset) {
	 if (!is_valid_fd(ifd)) {
		 lprintf(LO_ERROR, "I_Mmap: invalid file descriptor %d\n", ifd);
		 return NULL;
	 }
	 
	 if (offset + length > fds[ifd].part->size) {
		 lprintf(LO_ERROR, "I_Mmap: mapping beyond end of file\n");
		 return NULL;
	 }
	 
	 return (byte*)fds[ifd].mmap_ptr + offset;
 }
 
 int I_Munmap(void *addr, size_t length) {
	 return 0;
 }
 
 void I_Read(int ifd, void* vbuf, size_t sz)
 {
	 if (!is_valid_fd(ifd)) {
		 lprintf(LO_ERROR, "I_Read: invalid file descriptor %d\n", ifd);
		 return;
	 }
	 
	 if (fds[ifd].offset + sz > fds[ifd].part->size) {
		 lprintf(LO_ERROR, "I_Read: read beyond end of file (offset %d + size %d > file size %lu)\n", 
				 fds[ifd].offset, sz, fds[ifd].part->size);
		 return;
	 }
	 
	 // Debug logging for first few reads to help diagnose issues
	 if (fds[ifd].offset < 100) {
		 lprintf(LO_DEBUG, "I_Read: fd=%d, offset=%d, size=%lu, reading %d bytes\n", 
				 ifd, fds[ifd].offset, fds[ifd].part->size, sz);
		 
		 // If this is the first read (offset 0), dump the first few bytes to help debug
		 if (fds[ifd].offset == 0 && sz >= 12) {
			 unsigned char *data = (unsigned char*)fds[ifd].mmap_ptr;
			 lprintf(LO_INFO, "I_Read: First 12 bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
					 data[0], data[1], data[2], data[3], data[4], data[5], 
					 data[6], data[7], data[8], data[9], data[10], data[11]);
		 }
	 }
	 
	 memcpy(vbuf, (byte*)fds[ifd].mmap_ptr + fds[ifd].offset, sz);
	 fds[ifd].offset += sz;
 }
 
 const char *I_DoomExeDir(void)
 {
   return "";
 }
 
 char* I_FindFile(const char* wfname, const char* ext)
 {
   char *findfile_name = malloc(strlen(wfname) + strlen(ext) + 1);
 
   sprintf(findfile_name, "%s%s", wfname, ext);
 
   // Check WAD files
   for (int i = 0; i < sizeof(flash_wads)/sizeof(flash_wads[0]); ++i) {
	   if (!strcasecmp(findfile_name, flash_wads[i])) {
		   return findfile_name;
	   }
   }
   
   // Check .gwa files (graphics cache)
   for (int i = 0; i < sizeof(flash_gwa_files)/sizeof(flash_gwa_files[0]); ++i) {
	   if (!strcasecmp(findfile_name, flash_gwa_files[i])) {
		   // .gwa files are not supported on ESP32, return NULL
		   lprintf(LO_INFO, "I_FindFile: .gwa files not supported on ESP32: %s\n", findfile_name);
		   free(findfile_name);
		   return NULL;
	   }
   }
 
   lprintf(LO_INFO, "I_FindFile: %s not found\n", findfile_name);
   free(findfile_name);
   return NULL;
 }
 
 void I_SetAffinityMask(void)
 {
 }