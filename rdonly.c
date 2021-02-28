//
// Read only DFS implementation
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

/* DFS internals */
struct dfs_header {
	uint8_t identifier[4];
	uint8_t version;
	uint64_t first_journaling_block;
	uint64_t root_block;
	uint16_t mirror_partition;
	uint16_t bmp_granularity;
	uint16_t size_of_part;
	uint8_t alloc_bmp[];
};

struct dfs_node_entry {
	uint64_t next_entry;
	uint64_t child_entry;
	uint64_t fragment;
	uint64_t symlink;
	uint64_t file_size;
	uint64_t utc_creation_time;
	uint64_t utc_edition_time;
	uint64_t utc_access_time;
	uint16_t group_perms;
	uint16_t user_perms;
	uint16_t gid;
	uint16_t uid;
	uint8_t os_id;
	uint64_t os_specific;
	uint32_t name[];
};

struct dfs_fragment {
	uint64_t size;
	uint64_t checksum;
	uint64_t next;
	uint8_t data_blob[];
};

struct dfs_journal_event {
	uint8_t type;
	uint64_t next;
	uint64_t size;
	uint8_t data_blob[];
};

enum {
	JOURNAL_FILE_MODIFICATION	= 0,
	JOURNAL_EXPAND_SHRINK		= 1,
	JOURNAL_UNLINK_CHAIN		= 2,
	JOURNAL_LINK_CHAIN			= 3,
	JOURNAL_COPY				= 4,
	JOURNAL_CHANGE_DESCRIPTOR	= 5,
	JOURNAL_RESIZE				= 6
};

static void * diskbuf;

#define DEFAULT_DISK_SIZE			32768000
#define bitsof(x)					(x*8)

static void * file2mem(const char * name) {
	FILE * fp;
	fp = fopen(name,"rb");
	if(fp == NULL) {
		perror("error:\n");
		exit(1);
	}
	
	long sz;
	fseek(fp,0L,SEEK_END);
	sz = ftell(fp);
	rewind(fp);
	
	void * p = malloc(sz);
	if(p == NULL) {
		return p;
	}
	
	fread(p,sizeof(char),sz,fp);
	
	fclose(fp);
	return p;
}

static void mem2file(const char * name, void * p, size_t size) {
	FILE * fp;
	fp = fopen(name,"wb");
	if(fp == NULL) {
		perror("error:\n");
		exit(1);
	}
	fwrite(p,1,size,fp);
	fclose(fp);
	return;
}

static uint64_t block_alloc(struct dfs_header * head) {
	uintmax_t bits = bitsof(sizeof(head->alloc_bmp[0]));
	uintmax_t rpz = (le16toh(head->size_of_part)*4096);
	uintmax_t gran = le16toh(head->bmp_granularity);
	
	for(size_t i = 0; i < (rpz/gran)/bits; i++) {
		uintmax_t bp = 1<<(i%bits);
		if(!(head->alloc_bmp[i/bits]&bp)) {
			head->alloc_bmp[i/bits] |= bp;
			return i;
		}
	}
	return 0;
}

static int dfs_format(void) {
	struct dfs_header * head = diskbuf;
	head->identifier[0] = '4';
	head->identifier[1] = 'D';
	head->identifier[2] = 'F';
	head->identifier[3] = 'S';
	
	head->version = 0x03;
	
	// Up to implementation how to allocate first root :)
	// For this implementation, the root block will be 48 sectors after
	// Master root block
	head->root_block = htole64(512*48);
	
	// Default - up to implementation the desired granularity and size
	head->bmp_granularity = htole16(512);
	head->size_of_part = htole16(DEFAULT_DISK_SIZE/4096); // in 4K units
	head->mirror_partition = htole64(0x00);
	
	// We do not require journaling as we are not a live filesystem ;)
	head->first_journaling_block = htole64(0x00);
	
	// Reset bitmap
	for(size_t i = 0; i < le64toh(head->bmp_granularity)/DEFAULT_DISK_SIZE; i++) {
		head->alloc_bmp[i] = 0x00;
	}
	
	size_t bmp_size = (DEFAULT_DISK_SIZE/le16toh(head->bmp_granularity));
	
	printf("A bitmap of size %zu was made\n",bmp_size/8);
	
	// Cover the first of bitmaps
	for(size_t i = 0; i < 48/bitsof(sizeof(head->alloc_bmp[0])); i++) {
		head->alloc_bmp[i] = 0x00;
	}
	
	// Create the root entry
	struct dfs_node_entry * root = diskbuf+(512*48);
	root->next_entry = 0;
	root->child_entry = 0;
	root->fragment = 0;
	root->symlink = 0;
	root->file_size = 0;
	root->utc_creation_time = 0;
	root->utc_edition_time = 0;
	root->utc_access_time = 0;
	root->group_perms = htole16(777);
	root->user_perms = htole16(777);
	root->uid = 0;
	root->gid = 0;
	root->os_id = 0;
	root->os_specific = 0;
	strcpy((char *)&root->name[0],"root");
	
	// Done
	return 0;
}

static int dfs_list(void) {
	struct dfs_header * head = diskbuf;
	
	if(head->identifier[0] != '4'
	|| head->identifier[1] != 'D'
	|| head->identifier[2] != 'F'
	|| head->identifier[3] != 'S') {
		printf("invalid signature\n");
		
		// after bootloader
		head = diskbuf+512;
		
		// check next bytes
		if(head->identifier[0] != '4'
		|| head->identifier[1] != 'D'
		|| head->identifier[2] != 'F'
		|| head->identifier[3] != 'S') {
			printf("invalid signature after boot\n");
			exit(1);
		}
	}
	
	printf("--------------[ DFS ]--------------\n");
	printf("DFS Version: v%u.%u\n",(unsigned)((head->version&0xF0)>>4),(unsigned)(head->version&0x0F));
	printf("Root block: +%zu\n",le64toh(head->root_block));
	printf("Bitmap granularity: +%u\n",le16toh(head->bmp_granularity));
	printf("Size of partition: %u MB\n",le16toh(head->size_of_part)*4096);
	printf("Mirror partition? %s\n",(head->mirror_partition)?"Yes":"No");
	printf("Journaling? %s\n",(head->first_journaling_block)?"Yes":"No");
	
	// recurse child
	struct dfs_node_entry * node = (void *)head+le64toh(head->root_block);
	
	printf("--- Start of file listing ---\n");
	while(1) {
		printf("------------------\n");
		printf("Node: %s\n",(const char *)&node->name[0]);
		printf("U/G perms: %u %u\n",le16toh(node->user_perms),le16toh(node->group_perms));
		printf("Access: %zu\n",le64toh(node->utc_access_time));
		printf("Edition: %zu\n",le64toh(node->utc_edition_time));
		printf("Creation: %zu\n",le64toh(node->utc_creation_time));
		
		if(le64toh(node->next_entry) == 0) {
			break;
		}
		
		node = (void *)node+le64toh(node->next_entry);
	}
	printf("--- End of file listing ---\n");
	return 0;
}

int main(int argc, char ** argv) {
	if(argc < 2) {
		printf("usage: ./dfs [disk] [args...]\n");
		printf("/f : formats a disk (default size of 32.78 MB)\n");
		printf("/l : lists information about a disk\n");
		printf("example: './dfs disk.img /f' will format a disk\n");
	}
	
	diskbuf = file2mem(argv[1]);
	
	if(!strcmp(argv[2],"/f")) {
		dfs_format();
		mem2file(argv[1],diskbuf,DEFAULT_DISK_SIZE);
	}
	else if(!strcmp(argv[2],"/l")) {
		dfs_list();
	}
}
