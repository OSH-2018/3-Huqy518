#define FUSE_USE_VERSION 26
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fuse.h>
#include <sys/mman.h>
#include <stddef.h>

#define BLOCKNUM 65536	//一共64K个页 
#define BLOCKSIZE 32768	//每页32K 
#define SUCCESS	1
#define FAIL -1
#define Memory_Space_Insufficient -2
#define MORE_THAN_BIGGEST_FILE -3
#define MAX_BLOCK_NUM 10240	//单独文件最大块数 

static const size_t size = 2 * 1024 * 1024 * (size_t)1024;	//2G 
static const void* mem[BLOCKNUM];
int unused_num = BLOCKNUM;	//初始 
int last_block = 0;	//最后一次分配后的最后一块 

struct filenode{
    char filename[256];
    int32_t content[MAX_BLOCK_NUM];		//存储文件占用的块的位置 
    int32_t memlocation;	//节点的位置 
    int32_t filesize;	//文件大小，单位为块 
    struct stat st;
    struct filenode *next;
}; 


int find_unused(){
	int i = last_block;
	for(i = (last_block + 1) % BLOCKNUM; i != last_block; i = (i + 1) % BLOCKNUM){
		if(mem[i] == NULL){
			last_block = i;
			return i;
		}
	}	
	return FAIL;
}

int block_malloc(struct filenode* node, int num){
	int i = 0;
	if(num > unused_num)
		return Memory_Space_Insufficient;
	else if(num + node->filesize > MAX_BLOCK_NUM)
		return MORE_THAN_BIGGEST_FILE;
	else{
		while(num > 0){
			i = find_unused();
			node->filesize ++;
			node->content[node->filesize - 1] = i;
			mem[i] = mmap(NULL,BLOCKSIZE,PROT_READ | PROT_WRITE,MAP_PRIVATE | MAP_ANONYMOUS,-1,0);
			unused_num --;
			num --;
		}	//分配n块空间 
		return SUCCESS;
	}
	return FAIL;	//分配失败
}


void block_delete(struct filenode* node, int num){
	if(num > node->filesize)	//删除长度比文件本身大
		num = node->filesize;	//删除完
	int location;
	while(num > 0){
		location = node->content[node->filesize - 1];
		last_block = location > 0 ? location - 1 : BLOCKNUM;
		munmap(mem[location], BLOCKSIZE);
		node->filesize --;
		unused_num ++;
		num --; 
	} 
}

void block_realloc(off_t size, struct filenode* node){
	if(size >= node->filesize)
		block_malloc(node, size - node->filesize);
	else if(size < node->filesize)
		block_delete(node, node->filesize - size);
} 

static struct filenode *get_filenode(const char *name)
{
	struct filenode* root = (struct filenode*)mem[0];
    struct filenode* node = root;
    while(node) {
        if(strcmp(node->filename, name + 1) != 0)
            node = node->next;
        else
            return node;
    }
    return NULL;
}


static int create_filenode(const char* filename, const struct stat* st){
	int avail;
	if(unused_num == 0)
		return -ENOSPC;	//内存不足 
	avail = find_unused();
	mem[avail] = mmap(NULL,BLOCKSIZE,PROT_READ | PROT_WRITE,MAP_PRIVATE | MAP_ANONYMOUS,-1,0);
	unused_num --;
	struct filenode* newnode = (struct filenode*)mem[avail];
	memcpy(newnode->filename, filename, strlen(filename) + 1);
	memcpy(&(newnode->st), st, sizeof(struct stat));
	newnode->filesize = 0;
	newnode->memlocation = avail;
	struct filenode* root = (struct filenode*)mem[0];
	if(root->next == NULL){
		root->next = newnode;
	}
	else{
		newnode->next = root->next;
		root->next = newnode;	//头插法 
	}
	return 0;
}


static int oshfs_getattr(const char *path, struct stat *stbuf)
{
    int ret = 0;
    struct filenode *node = get_filenode(path);
    if(strcmp(path, "/") == 0) {
        memset(stbuf, 0, sizeof(struct stat));
        stbuf->st_mode = S_IFDIR | 0755;
    } else if(node) {
        memcpy(stbuf, &(node->st), sizeof(struct stat));
    } else {
        ret = -ENOENT;
    }
    return ret;
}

static int oshfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
    struct filenode* node = (struct filenode*)mem[0];
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    while(node) {
        filler(buf, node->filename, &(node->st), 0);
        node = node->next;
    }
    return 0;
}


static int oshfs_mknod(const char *path, mode_t mode, dev_t dev)
{
    struct stat st;
    st.st_mode = S_IFREG | 0644;
    st.st_uid = fuse_get_context()->uid;
    st.st_gid = fuse_get_context()->gid;
    st.st_nlink = 1;
    st.st_size = 0;
    create_filenode(path + 1, &st);
    return 0;
}

static int oshfs_open(const char *path, struct fuse_file_info *fi)
{
    return 0;
}

static void* oshfs_init(struct fuse_conn_info *conn){
	mem[0] = mmap(NULL, BLOCKSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	struct filenode* root = (struct filenode*)mem[0];
	strcpy(root->filename, ".");
	root->memlocation = 0;
	root->next = NULL;
	return NULL;
}

static int oshfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi){
	struct filenode* node = get_filenode(path);
	if(node == NULL)
		return -ENOENT;
	int offset_out = offset / BLOCKSIZE;	//块间偏移
	int offset_in = offset % BLOCKSIZE;		//块内偏移 
	int start_node, write = 0;
	if(offset + size > node->st.st_size){
        node->st.st_size = offset + size;
        int block_need = (node->st.st_size - 1) / BLOCKSIZE + 1;
        if(block_need > node->filesize){
        	int ret = block_malloc(node, block_need - node->filesize);	//分配空间 
        	if(ret == Memory_Space_Insufficient) 
        		return -ENOSPC;
        	else if(ret == MORE_THAN_BIGGEST_FILE)
        		return -EFBIG;
		}       
	}	
	while(size > write){
		start_node = node->content[offset_out];
		if(BLOCKSIZE - offset_in >= size - write){
			memcpy(mem[start_node] + offset_in, buf + write, size - write);
			write = size;
		}	//分最后一块 
		else{
			memcpy(mem[start_node] + offset_in, buf + write, BLOCKSIZE - offset_in);
			write = BLOCKSIZE - offset_in;
			offset_out ++;
			offset_in = 0;
		}//先把不足一块填满，然后逐渐填写完整的块 
	}	
	return size;
}

static int oshfs_truncate(const char *path, off_t size){
	struct filenode *node = get_filenode(path);
	if(node == NULL)
		return -ENOENT;
	node->st.st_size = size;
	int new_filesize = (size - 1) / BLOCKSIZE + 1;
	block_realloc(new_filesize, node);
	return 0;
} 

static int oshfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi){
	struct filenode *node = get_filenode(path);
	if(node == NULL)
		return -ENOENT;
	int need_read = size;
    if(offset + size > node->st.st_size)
        need_read = node->st.st_size - offset;
	int offset_out = offset / BLOCKSIZE;
	int offset_in = offset % BLOCKSIZE;
	int read = 0, start_node;
	while(read < need_read){
		start_node = node->content[offset_out];
		if((BLOCKSIZE - offset_in) < (need_read - read)){
			memcpy(buf + read, mem[start_node] + offset_in, BLOCKSIZE - offset_in);
			read = BLOCKSIZE - offset_in;
			offset_in = 0;
			offset_out ++;
		}
		else{
			memcpy(buf + read, mem[start_node] + offset_in, need_read - read);
			read = need_read;
		}
	}
	return need_read;
}
	
static int oshfs_unlink(const char *path){
	struct filenode* node = get_filenode(path);
	if(node == NULL)
		return -ENOENT;
	struct filenode* p = (struct filenode*)mem[0];
	for(p = (struct filenode*)mem[0]; p != NULL; p = p->next){
		if(p->next == node)	//找到要删除的节点 
				p->next = node->next;
	}
	block_delete(node, node->filesize);
	munmap(mem[node->memlocation], BLOCKSIZE);
	mem[node->memlocation] = NULL;
	return 0;
}
	
static const struct fuse_operations op = {
    .init = oshfs_init,
    .getattr = oshfs_getattr,
    .readdir = oshfs_readdir,
    .mknod = oshfs_mknod,
    .open = oshfs_open,
    .write = oshfs_write,
    .truncate = oshfs_truncate,
    .read = oshfs_read,
    .unlink = oshfs_unlink,
};

int main(int argc, char *argv[])
{
    return fuse_main(argc, argv, &op, NULL);
}	
