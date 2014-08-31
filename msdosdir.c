/**
 * BootStrapSector is the first 512 bytes of the FAT.
 * Two byte fields are little-endian
 *
 * The format of this sector is:
 * byte(s) contents
 * ------- -------------------------------------------------------
 *  00-02	first instruction of bootstrap routine
 *  03-10	OEM name
 *  11-12	number of bytes per sector
 *     13	number of sectors per cluster
 *  14-15	number of reserved sectors
 *     16	number of copies of the file allocation table
 *  17-18	number of entries in root directory
 *  19-20	total number of sectors
 *     21	media descriptor byte
 *  22-23	number of sectors in each copy of file allocation table
 *  24-25	number of sectors per track
 *  26-27	number of sides
 *  28-29	number of hidden sectors
 *  30-509	bootstrap routine and partition information
 *     510	hexadecimal 55
 *     511	hexadecimal AA
 */

#include <stdio.h>
#include <stdlib.h>

typedef unsigned char BYTE;
typedef struct pair {
	BYTE bytes[2];
} BytePair;

typedef struct triplet {
	BYTE bytes[3];
} ByteTriplet;

typedef struct quad {
	BYTE bytes[4];
} ByteQuad;

typedef struct bootsector {
	ByteTriplet firstInstruction; // This is often a jump instruction to the boot sector code itself
	BYTE OEM[8];
	BytePair numBytesPerSector;
	BYTE numSectorsPerCluster;
	BytePair numReservedSectors;
	BYTE numCopiesFAT;
	BytePair numEntriesRootDir;
	BytePair numSectors;
	BYTE mediaDescriptor;
	BytePair numSectorsInFAT;
	BytePair numSectorsPerTrack;
	BytePair numSides;
	ByteQuad numHiddenSectors;
	ByteQuad largeSectors;
	BYTE physicalDiskNum;
	BYTE currentHead;
	BYTE signature;
	ByteQuad volumeSN;
	BYTE volumeLabel[11];
	BYTE formatType[8]; // FAT12 or FAT16 in this program
	BYTE bootstrap[448];
	BYTE hex55AA[2]; // the last bytes of the boot sector are, by definition, 55 AA.  This is a sanity check.
} BootSector;

typedef struct info {
	int fatType;
	int numClusters;
	int numFATSectors;
	int numCopiesFAT;
	int sizeofSector;
	int firstDataSector;
	int numRootEntries;
	int numRootClusters;
	int reservedSectors;
	int filesFound;
	long totalSize;
} FATInfo;

FATInfo* fatInfo;

typedef BYTE* Sector;

/*
 * 00-07	Filename
 * 08-10	Filename extension
 *    11	File attributes
 *    12	Reserved for use by WinNT
 *    13	Time created (tenth of a second)
 * 14-15	Time created
 * 		Hour	5 bits
 * 		Minutes	6 bits
 * 		Seconds	5 bits
 * 16-17	Date created
 * 		Year	7 bits
 * 		Month	4 bits
 * 		Day	5 bits
 * 18-19	Date last accessed
 * 20-21	Upper half of entry's first cluster, 0 for FAT12 and FAT16
 * 22-23	Time last modified
 * 24-25	Date last modified
 * 26-27	Lower half of entry's first cluster
 * 28-31	File size in bytes
 */
typedef struct direntry {
	BYTE filename[8];
	BYTE extension[3];
	BYTE attributes;
	BYTE reserved;
	BYTE timeCreatedTenthSec;
	BytePair timeCreated;
	BytePair dateCreated;
	BytePair dateAccessed;
	BytePair startingClusterUpper;
	BytePair timeModified;
	BytePair dateModified;
	BytePair startingCluster;
	ByteQuad fileSize;
} DirectoryEntry;
/*
 * Directory entry special values for first byte
 * 0x00	Filename never used.
 * 0xe5	The filename has been used, but the file has been deleted.
 * 0x05	The first character of the filename is actually 0xe5.
 * 0x2e	The entry is for a directory, not a normal file.
 * 	If the second byte is also 0x2e, the cluster field contains the cluster number of this directory's parent directory.
 *	If the parent directory is the root directory (which is statically allocated and doesn't have a cluster number), cluster number 0x0000 is specified here.
 */
const int NOT_USED = 0x00;
const int DELETED = 0xe5;
const int ACTUAL_E5 = 0x05;
const int DIRECTORY = 0x2e;

// Directory entry attributes
const int ATTR_READ_ONLY = 0x01; // File is read only
const int ATTR_HIDDEN = 0x02; // Hidden file
const int ATTR_SYSTEM_FILE = 0x04; // Indicates a system file. These are hidden as well
const int ATTR_VOLUME_LABEL = 0x08; // Disk's volume label. Only found in the root directory
const int ATTR_SUB_DIR = 0x10; // The entry describes a subdirectory
const int ATTR_ARCHIVE = 0x20; // Archive flag. Set when the file is modified. Used by backup programs
const int ATTR_UNUSED1 = 0x40; // Not used; must be set to 0
const int ATTR_UNUSED2 = 0x80; // Not used; must be set to 0

// FAT entry special values
const int AVAILABLE_12 = 0x000;
const int AVAILABLE_16 = 0x0000;
const int RESERVED_12 = 0x001;
const int RESERVED_16 = 0x0001;
const int BAD_CLUSTER_12 = 0xff7;
const int BAD_CLUSTER_16 = 0xfff7;
const int END_MARKER_12 = 0xff8;
const int END_MARKER_16 = 0xfff8;

const int FIRST_ROOT_CLUSTER = 2;

int le2be2(BytePair bytes);
int le2be3(ByteTriplet bytes, int which);
int le2be4(ByteQuad bytes);
int getNumberClusters(BootSector* bs);
int getFATType(BootSector* bs);
int getAbsoluteCluster(int relativeCluster);
int clusterRelativeToRoot(int absoluteCluster);
int getNextCluster(Sector fatSector, int cluster);
Sector getCorrectFATSector(FILE* fs, Sector fatSector, int curFATSector, int nextCluster);
void displayBootStrapInfo(BootSector* bs);
void readBootStrapSector(FILE* fs, BootSector* bs);
void displayDirectoryEntry(DirectoryEntry* de);
void scanDirectorySector(FILE* fs, Sector directory);
void scanDirectory(FILE* fs, int cluster, int maxClusters);
void hexDump(char *desc, void *addr, int len);

int main (int argc, char *argv[]) {
	if (argc != 2) {
		printf("usage: %s filename\n", argv[0]);
		return 0;
	}
	// assume argv[1] is a filename to open
	FILE* file = fopen(argv[1], "r");
	if (file == 0) {
		printf("Could not open file %s\n", argv[1]);
		return 1;
	}
	BootSector* bs = malloc(sizeof(BootSector));
	readBootStrapSector(file, bs);
	scanDirectory(file, FIRST_ROOT_CLUSTER, fatInfo->numRootClusters);
	
	free(bs);
	fclose(file);
	
	return 0;
}

int le2be2(BytePair bytes) {
	return (bytes.bytes[0] + (bytes.bytes[1] << 8));
}

int le2be3(ByteTriplet bytes, int which) {
	if (which != 1 && which != 2) {
		which = 1;
	}
	
	int firstByte = (int)bytes.bytes[0];
	int secondByte = (int)bytes.bytes[1];
	int thirdByte = (int)bytes.bytes[2];
	int result = 0;
	//UV WX YZ --> XUV YZW
	
	if (which == 1) {
		result = firstByte; // UV
		result += ((secondByte & 0x0f) << 8); // X
	} else {
		result = (thirdByte << 4); // YZ
		result += ((secondByte & 0xf0) >> 4); // W
	}
	
	return result;
}

int le2be4(ByteQuad bytes) {
	int res = bytes.bytes[0];
	int i;
	for (i = 1; i < 4; i++) {
		res += bytes.bytes[i] << (8 * i);
	}
	return res;
}

int getNumberClusters(BootSector* bs) {
	unsigned int root_dir_sectors = ((le2be2(bs->numEntriesRootDir) * 32) + (le2be2(bs->numBytesPerSector) - 1)) / le2be2(bs->numBytesPerSector);
	unsigned int data_sectors;
	if (le2be2(bs->numSectors) != 0) {
		data_sectors = le2be2(bs->numSectors) - (le2be2(bs->numReservedSectors) + (bs->numCopiesFAT * le2be2(bs->numSectorsInFAT)) + root_dir_sectors);
	} else {
		data_sectors = le2be4(bs->largeSectors) - (le2be2(bs->numReservedSectors) + (bs->numCopiesFAT * le2be2(bs->numSectorsInFAT)) + root_dir_sectors);
	}
	return (int)(data_sectors / bs->numSectorsPerCluster);
}

int getFATType(BootSector* bs) {
	int total_clusters = getNumberClusters(bs);
	if (total_clusters < 4085) {
		return 12;
	} else {
		if (total_clusters < 65525) {
			return 16;
		} else {
			return 32;
		}
	}
}

int getAbsoluteCluster(int relativeCluster) {
	return relativeCluster - 2 + fatInfo->firstDataSector;
}

int clusterRelativeToRoot(int absoluteCluster) {
	return absoluteCluster + fatInfo->numRootClusters;
}

void displayBootStrapInfo(BootSector* bs) {
	printf("OEM:                 %.*s\n", 8, bs->OEM);
	printf("Bytes Per Sector:    %d\n", le2be2(bs->numBytesPerSector));
	printf("Sectors Per Cluster: %d\n", bs->numSectorsPerCluster);
	printf("Reserved Sectors:    %d\n", le2be2(bs->numReservedSectors));
	printf("FATs:                %d\n", bs->numCopiesFAT);
	printf("Entries in Root:     %d\n", le2be2(bs->numEntriesRootDir));
	printf("Sectors:             %d\n", le2be2(bs->numSectors));
	printf("Media:               0x%02x\n", bs->mediaDescriptor);
	printf("FAT Sectors:         %d\n", le2be2(bs->numSectorsInFAT));
	printf("Sectors Per Track:   %d\n", le2be2(bs->numSectorsPerTrack));
	printf("Sides:               %d\n", le2be2(bs->numSides));
	printf("Hidden Sectors:      %d\n", le2be4(bs->numHiddenSectors));
	printf("Large Sectors:       %d\n", le2be4(bs->largeSectors));
	printf("Disk Number:         %d\n", bs->physicalDiskNum);
	printf("Current Head:        %d\n", bs->currentHead);
	printf("Signature:           0x%02x\n", bs->signature);
	printf("Volume SN:           0x%08x\n", le2be4(bs->volumeSN));
	printf("Volume Label:        %.*s\n", 11, bs->volumeLabel);
	printf("Format Type:         %.*s\n", 8, bs->formatType);
	printf("FAT Type is FAT%d, disk has %d clusters\n", fatInfo->fatType, getNumberClusters(bs));
}

void readBootStrapSector(FILE* fs, BootSector* bs) {
	fread(bs, sizeof(BootSector), 1, fs);
	
	fatInfo = malloc(sizeof(FATInfo));
	fatInfo->fatType = getFATType(bs);
	fatInfo->numFATSectors = le2be2(bs->numSectorsInFAT);
	fatInfo->numCopiesFAT = bs->numCopiesFAT;
	fatInfo->sizeofSector = le2be2(bs->numBytesPerSector);
	fatInfo->firstDataSector = fatInfo->numCopiesFAT * le2be2(bs->numSectorsInFAT) + 1;
	fatInfo->numRootEntries = le2be2(bs->numEntriesRootDir);
	fatInfo->numRootClusters = fatInfo->numRootEntries * sizeof(DirectoryEntry) / fatInfo->sizeofSector;
	fatInfo->reservedSectors = le2be2(bs->numReservedSectors);
	fatInfo->filesFound = 0;
	fatInfo->totalSize = 0;
}

void displayDirectoryEntry(DirectoryEntry* de) {
	int timeCreated = le2be2(de->timeCreated);
	int hourCreated = (timeCreated & 0xf800) >> 11;
	int minCreated = (timeCreated & 0x7e0) >> 5;
	int secCreated = (timeCreated & 0x1f);
	secCreated = secCreated * 2; // time resolution of 2 seconds
	
	int dateCreated = le2be2(de->dateCreated);
	int yearCreated = ((dateCreated & 0xfe00) >> 9);
	int monthCreated = (dateCreated & 0x1e0) >> 5;
	int dayCreated = (dateCreated & 0x1f);
	yearCreated = yearCreated + 1980; // year is offset by 1980
	
	int dateAccessed = le2be2(de->dateAccessed);
	int yearAccessed = ((dateAccessed & 0xfe00) >> 9);
	int monthAccessed = (dateAccessed & 0x1e0) >> 5;
	int dayAccessed = (dateAccessed & 0x1f);
	yearAccessed = yearAccessed + 1980;
	
	int timeModified = le2be2(de->timeModified);
	int hourModified = (timeModified & 0xf800) >> 11;
	int minModified = (timeModified & 0x7e0) >> 5;
	int secModified = (timeModified & 0x1f);
	secModified = secModified * 2;
	
	int dateModified = le2be2(de->dateModified);
	int yearModified = ((dateModified & 0xfe00) >> 9);
	int monthModified = (dateModified & 0x1e0) >> 5;
	int dayModified = (dateModified & 0x1f);
	yearModified = yearModified + 1980;
	
	printf("%8.*s %3.*s %10d  %02d-%02d-%04d %02d:%02d:%02d  %02d-%02d-%04d  %02d-%02d-%04d %02d:%02d:%02d\n",
		8, de->filename, 3, de->extension, le2be4(de->fileSize),
		monthCreated, dayCreated, yearCreated,
		hourCreated, minCreated, secCreated,
		monthAccessed, dayAccessed, yearAccessed,
		monthModified, dayModified, yearModified,
		hourModified, minModified, secModified);
}

/**
 * Scans a sector of a directory and prints out its entries
 * 
 * @param fs - The file handle for the filesystem
 * @param directory - The sector to scan
 */
void scanDirectorySector(FILE* fs, Sector directory) {
	int sizeofDirEntry = sizeof(DirectoryEntry);
	int entriesPerSector = fatInfo->sizeofSector / sizeofDirEntry;
	
	int e;
	for (e = 0; e < entriesPerSector; e++) {
		int b;
		int offset = e * sizeofDirEntry;
		
		if (directory[offset] != DELETED && directory[offset] != NOT_USED) {
			DirectoryEntry* de = malloc(sizeofDirEntry);
			for (b = 0; b < 8; b++) {
				de->filename[b] = directory[offset + b];
			}
			if (de->filename[0] == ACTUAL_E5) {
				de->filename[0] = 0xe5;
			}
			for (b = 0; b < 3; b++) {
				de->extension[b] = directory[offset + 8 + b];
			}
			de->attributes = directory[offset + 11];
			de->reserved = directory[offset + 12];
			de->timeCreatedTenthSec = directory[offset + 13];
			de->timeCreated.bytes[0] = directory[offset + 14];
			de->timeCreated.bytes[0] = directory[offset + 15];
			de->dateCreated.bytes[0] = directory[offset + 16];
			de->dateCreated.bytes[0] = directory[offset + 17];
			de->dateAccessed.bytes[0] = directory[offset + 18];
			de->dateAccessed.bytes[0] = directory[offset + 19];
			de->startingClusterUpper.bytes[0] = directory[offset + 20];
			de->startingClusterUpper.bytes[0] = directory[offset + 21];
			de->timeModified.bytes[0] = directory[offset + 22];
			de->timeModified.bytes[1] = directory[offset + 23];
			de->dateModified.bytes[0] = directory[offset + 24];
			de->dateModified.bytes[1] = directory[offset + 25];
			de->startingCluster.bytes[0] = directory[offset + 26];
			de->startingCluster.bytes[1] = directory[offset + 27];
			for (b = 0; b < 4; b++) {
				de->fileSize.bytes[b] = directory[offset + 28 + b];
			}
			
			if (!(de->attributes & ATTR_HIDDEN) && !(de->attributes & ATTR_SYSTEM_FILE)
				&& !(de->attributes & ATTR_VOLUME_LABEL)
			) {
				fatInfo->filesFound++;
				fatInfo->totalSize += le2be4(de->fileSize);
				
				displayDirectoryEntry(de);
				
				if (directory[offset] == DIRECTORY || de->attributes & ATTR_SUB_DIR) {
					if (directory[offset + 1] != DIRECTORY) {
						// don't scan if second byte in entry is also DIRECTORY
						// this indicates that the cluster points to the parent
						// which will result in infinite recursion
						scanDirectory(fs, le2be2(de->startingCluster), 0);
					}
				}
			}
			
			free(de);
		}
	}
}

int getNextCluster(Sector fatSector, int cluster) {
	int nextCluster;
	int offset;
	// even: i / 2 * 3
	// odd: (i - 1) / 2 * 3 + 1
	if (fatInfo->fatType == 12) {
		ByteTriplet *bt = malloc(sizeof(ByteTriplet));
		if (cluster % 2) {
			offset = (cluster - 1) / 2 * 3 + 1;
			bt->bytes[1] = fatSector[offset];
			bt->bytes[2] = fatSector[offset + 1];
			nextCluster = le2be3(*bt, 2);
		} else {
			offset = cluster / 2 * 3;
			bt->bytes[0] = fatSector[offset];
			bt->bytes[1] = fatSector[offset + 1];
			nextCluster = le2be3(*bt, 1);
		}
		free(bt);
	} else if (fatInfo->fatType == 16) {
		BytePair *bp = malloc(sizeof(BytePair));
		offset = cluster * 2;
		bp->bytes[0] = fatSector[offset];
		bp->bytes[1] = fatSector[offset + 1];
		nextCluster = le2be2(*bp);
		free(bp);
	} else {
		nextCluster = -1;
	}
	return nextCluster;
}

Sector getCorrectFATSector(FILE* fs, Sector fatSector, int curFATSector, int nextCluster) {
	int sizeofSector = fatInfo->sizeofSector;
	int startFAT = sizeofSector * fatInfo->reservedSectors;
	int entriesPerFATSector;
	if (fatInfo->fatType == 12) {
		entriesPerFATSector = sizeofSector * 2 / 3;
	} else if (fatInfo->fatType == 16) {
		entriesPerFATSector = sizeofSector / 2;
	} else {
		return fatSector;
	}
	
	// check if the cluster being searched for is within the
	// currently loaded FAT sector. If not, get the right one
	
	// the smallest cluster available in this FAT sector
	int fatSectorMin = curFATSector * entriesPerFATSector;
	
	// if cluster being searched for is less than the lowest
	// available or higher than the highest available
	if (nextCluster < fatSectorMin || nextCluster > (fatSectorMin + entriesPerFATSector)) {
		
		// get the sector num required to contain nextCluster
		curFATSector = sizeofSector * (nextCluster / entriesPerFATSector);
		
		// free the existing FAT sector
		free(fatSector);
		
		// malloc and read the new sector
		fatSector = malloc(sizeofSector);
		fseek(fs, startFAT + sizeofSector * curFATSector, SEEK_SET);
		fread(fatSector, sizeofSector, 1, fs);
	}
	
	return fatSector;
}

/**
 * Scans through a directory and lists its contents
 * 
 * @param fs - The file handle for the filesystem
 * @param cluster - The cluster to start at
 * @param maxClusters - Only used for root directories.
 *                      Indicates how many contiguous clusters to check
 */
void scanDirectory(FILE* fs, int cluster, int maxClusters) {
	int numFATSectors = fatInfo->numFATSectors;
	int sizeofSector = fatInfo->sizeofSector;
	int startFAT = sizeofSector * fatInfo->reservedSectors;
	int endOfDir = 0;
	int clusterCount = 0;
	int nextCluster = cluster;
	int curFATSector = -1;
	
	printf("FILENAME EXT       SIZE              CREATED    ACCESSED             MODIFIED\n");
	
	// malloc here just to be sure all frees have something to free
	Sector fatSector = malloc(sizeofSector);
	
	while (!endOfDir) {
		
		// if we've got a good cluster
		if ((fatInfo->fatType == 12 && nextCluster > RESERVED_12 && nextCluster < BAD_CLUSTER_12) ||
			(fatInfo->fatType == 16 && nextCluster > RESERVED_16 && nextCluster < BAD_CLUSTER_16)
		) {
			
			// get the correct address for this cluster
			int absoluteCluster = getAbsoluteCluster(nextCluster);
			
			Sector fileSector = malloc(sizeofSector);
			fseek(fs, sizeofSector * absoluteCluster, SEEK_SET);
			fread(fileSector, sizeofSector, 1, fs);
			
			scanDirectorySector(fs, fileSector);
			
			free(fileSector);
		} else {
			
			// otherwise stop searching
			endOfDir = 1;
			break;
		}
		
		fatSector = getCorrectFATSector(fs, fatSector, curFATSector, nextCluster);
		
		// if doing root directory, increment the cluster count
		if (maxClusters > 0) {
			// next cluster = start cluster + how many done so far
			clusterCount++;
			nextCluster = cluster + clusterCount;
			if (clusterCount >= maxClusters) {
				endOfDir = 1;
				break;
			}
		} else {
			// get the next cluster based on the current cluster
			nextCluster = getNextCluster(fatSector, nextCluster);
		}
	}
	
	free(fatSector);
	
	printf("%5d file(s) %9ld bytes\n", fatInfo->filesFound, fatInfo->totalSize);
}

void hexDump(char *desc, void *addr, int len) {
    int i;
    unsigned char buff[17];
    unsigned char *pc = (unsigned char*)addr;

    // Output description if given.
    if (desc != NULL)
        printf ("%s:\n", desc);

    // Process every byte in the data.
    for (i = 0; i < len; i++) {
        // Multiple of 16 means new line (with line offset).

        if ((i % 16) == 0) {
            // Just don't print ASCII for the zeroth line.
            if (i != 0)
                printf ("  %s\n", buff);

            // Output the offset.
            printf ("  %04x ", i);
        }

        // Now the hex code for the specific character.
        printf (" %02x", pc[i]);

        // And store a printable ASCII character for later.
        if ((pc[i] < 0x20) || (pc[i] > 0x7e))
            buff[i % 16] = '.';
        else
            buff[i % 16] = pc[i];
        buff[(i % 16) + 1] = '\0';
    }

    // Pad out last line if not exactly 16 characters.
    while ((i % 16) != 0) {
        printf ("   ");
        i++;
    }

    // And print the final ASCII bit.
    printf ("  %s\n", buff);
}
