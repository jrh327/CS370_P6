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
} FATInfo;

FATInfo* fatInfo;

typedef BYTE* Sector;

typedef struct folder {
	BYTE name[11];
	BYTE attribute;
	ByteTriplet createTime;
	BytePair createDate;
	BytePair lastAccessDate;
	BytePair lastModifiedTime;
	BytePair lastModifiedDate;
	BytePair startingClusterNumberInFAT;
	ByteQuad size;
} Folder;

/*
 * 00-07	Filename
 * 08-10	Filename extension
 *    11	File attributes
 * 12-21	Reserved
 * 22-23	Time created or last updated
 * 24-25	Date created or last updated
 * 26-27	Starting cluster number for file
 * 28-31	File size in bytes
 */
typedef struct direntry {
	BYTE filename[8];
	BYTE extension[3];
	BYTE attributes;
	BYTE reserved[10];
	BytePair timeCreated;
	BytePair dateCreated;
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

// FAT entry special values
const int AVAILABLE_12 = 0x000;
const int AVAILABLE_16 = 0x0000;
const int RESERVED_12 = 0x001;
const int RESERVED_16 = 0x0001;
const int BAD_CLUSTER_12 = 0xff7;
const int BAD_CLUSTER_16 = 0xfff7;
const int END_MARKER_12 = 0xff8;
const int END_MARKER_16 = 0xfff8;

int getFATType(BootSector* bs);
BYTE* getVolumeLabel(BootSector* bs);
BYTE* getVolumeSerialNumber(BootSector* bs);
BYTE* getFormatType(BootSector* bs);
void readBootStrapSector(FILE* fs, BootSector* bs);
void readFilesInFAT(FILE* fs, BootSector* bs);

int le2be2(BytePair bytes);
int le2be3(ByteTriplet bytes, int which);
int le2be4(ByteQuad bytes);

void hexDump (char *desc, void *addr, int len);

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
	BootSector* bs = (BootSector*)malloc(sizeof(BootSector));
	readBootStrapSector(file, bs);
	readFilesInFAT(file, bs);
	
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

void readBootStrapSector(FILE* file, BootSector* bs) {
	fread(bs, sizeof(BootSector), 1, file);
	//hexDump("BootSector", &bs, sizeof(BootSector));
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
	
	fatInfo = (FATInfo*)malloc(sizeof(FATInfo));
	fatInfo->fatType = getFATType(bs);
	fatInfo->numFATSectors = le2be2(bs->numSectorsInFAT);
	fatInfo->numCopiesFAT = bs->numCopiesFAT;
	fatInfo->sizeofSector = le2be2(bs->numBytesPerSector);
	fatInfo->firstDataSector = fatInfo->numCopiesFAT * le2be2(bs->numSectorsInFAT) + 1;
	fatInfo->numRootEntries = le2be2(bs->numEntriesRootDir);
	
	printf("FAT Type is FAT%d, disk has %d clusters\n", fatInfo->fatType, getNumberClusters(bs));
}

void displayDirectoryEntry(DirectoryEntry* de) {
	printf("%8.*s %3.*s %13d\n", 8, de->filename, 3, de->extension, le2be4(de->fileSize));
}

void scanDirectory(Sector directory) {
	int sizeofDirEntry = sizeof(DirectoryEntry);
	int entriesPerSector = fatInfo->sizeofSector / sizeofDirEntry;
	int done = 0;
	
	while (!done) {
		int e;
		//      ADDNAME  EX_        14,032 08-31-94  12:00a
		printf("FILENAME EXT          SIZE     DATE    TIME\n");
		for (e = 0; e < entriesPerSector; e++) {
			int b;
			int offset = e * sizeof(DirectoryEntry);
			
			if (directory[offset] != DELETED && directory[offset] != NOT_USED) {
				DirectoryEntry* de = (DirectoryEntry*)malloc(sizeof(DirectoryEntry));
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
				for (b = 0; b < 10; b++) {
					de->reserved[b] = directory[offset + 12 + b];
				}
				de->timeCreated.bytes[0] = directory[offset + 22];
				de->timeCreated.bytes[1] = directory[offset + 23];
				de->dateCreated.bytes[0] = directory[offset + 24];
				de->dateCreated.bytes[1] = directory[offset + 25];
				de->startingCluster.bytes[0] = directory[offset + 26];
				de->startingCluster.bytes[1] = directory[offset + 27];
				for (b = 0; b < 4; b++) {
					de->fileSize.bytes[b] = directory[offset + 28 + b];
				}
				
				displayDirectoryEntry(de);
				
				// if entry is itself a directory
				// scanDirectory(new sector)
				free(de);
			}
		}
		// if not the last sector in the directory
		// get the next sector and keep going
		// else
		done = 1;
	}
}

void readFilesInFAT(FILE* fs, BootSector* bs) {
	int numFATSectors = fatInfo->numFATSectors;
	int sizeofSector = fatInfo->sizeofSector;
	int startFAT = sizeofSector * le2be2(bs->numReservedSectors);
	int dirEntriesPerSector = fatInfo->numRootEntries * sizeof(DirectoryEntry) / sizeofSector;
	int numRootSectors = fatInfo->numRootEntries / dirEntriesPerSector;
	
	int i;
	for (i = 0; i < 1; i++) {
		Sector fatSector = (BYTE*)malloc(sizeofSector);
		fseek(fs, sizeofSector * i + startFAT, SEEK_SET);
		fread(fatSector, sizeofSector, 1, fs);
		
		int j;
		// first two entries in FAT are reserved
		// start scanning at 2
		for (j = 2; j < numRootSectors + 2; j++) {
			int fileCluster;
			int offset;
			
			if (fatInfo->fatType == 12) {
				ByteTriplet *bt = (ByteTriplet*)malloc(sizeof(ByteTriplet));
				if (j % 2) {
					offset = (j - 1) * 3 / 2 + 1;
					bt->bytes[1] = fatSector[offset];
					bt->bytes[2] = fatSector[offset + 1];
					fileCluster = le2be3(*bt, 2);
				} else {
					offset = j * 3 / 2;
					bt->bytes[0] = fatSector[offset];
					bt->bytes[1] = fatSector[offset + 1];
					fileCluster = le2be3(*bt, 1);
				}
			} else if (fatInfo->fatType == 16) {
				BytePair *bp = (BytePair*)malloc(sizeof(BytePair));
				offset = j * 2;
				bp->bytes[0] = fatSector[offset];
				bp->bytes[1] = fatSector[offset + 1];
				fileCluster = le2be2(*bp);
			} else {
				return;
			}
			
			Sector fileSector;
			if (fileCluster > RESERVED_12 && fileCluster < BAD_CLUSTER_12) {
				// sectors listed in FAT are relative to the first data sector + 2
				// value returned from getAbsoluteCluster is not 0-based index
				fileCluster = getAbsoluteCluster(fileCluster) - 1;
				
				fileSector = (BYTE*)malloc(sizeofSector);
				fseek(fs, sizeofSector * fileCluster, SEEK_SET);
				fread(fileSector, sizeofSector, 1, fs);
				
				scanDirectory(fileSector);
				
				free(fileSector);
			}
		}
	}
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
