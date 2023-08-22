#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "ufs.h"
#include "message.h"
#include "mfs.h"
#include "udp.h"

int sd;
void *image;
int fd;
super_t *SUPERBLOCK;
inode_t *root_inode;
dir_ent_t *root_dir;
unsigned int *inodeMap;
unsigned int *dataMap;
inode_t *inode_table;
int *data_table;

int server_Lookup(int pinum, char *name);
int server_Stat(const int inum, MFS_Stat_t *m);
int server_Write(int inum, char *buffer, int offset, int nbytes);
int server_Create(int pinum, int type, char *name);
int server_Shutdown();
int server_Unlink(int pinum, char *name);
int server_Read(const int inum, char *buffer, int offset, int nbytes);

typedef struct
{
    dir_ent_t entries[128];
} dir_block_t;

typedef struct
{
    inode_t inodes[UFS_BLOCK_SIZE / sizeof(inode_t)];
} inode_block;

typedef struct
{
    unsigned int bits[UFS_BLOCK_SIZE / sizeof(unsigned int)];
} bitmap_t;

void intHandler(int dummy)
{
    UDP_Close(sd);
    exit(130);
}

unsigned int get_bit(unsigned int *bitmap, int position)
{
    int index = position / 32;
    int offset = 31 - (position % 32);
    return (bitmap[index] >> offset) & 0x1;
}

void set_bit(unsigned int *bitmap, int position)
{
    int index = position / 32;
    int offset = 31 - (position % 32);
    bitmap[index] |= 0x1 << offset;
}

void set_bit_zero(unsigned int *bitmap, int position)
{
    int index = position / 32;
    int offset = 31 - (position % 32);
    bitmap[index] &= ~ (0x1 << offset);
}

void set_bit_zero2(unsigned int *bitmap, int position)
{
    int index = position / 32;
    int offset = 31 - (position % 32);
    bitmap[index] |= 0x0 << offset;
}

int main(int argc, char *argv[])
{
    signal(SIGINT, intHandler);

    if (argc != 3)
    {
        printf("Incorrect format!");
        exit(1);
    }

    int port = atoi(argv[1]);
    char *file = argv[2];
    fd = open(file, O_RDWR);
    if (fd < 0)
    {
        printf("image does not exist\n");
        exit(1);
    }

    struct stat sbuf;
    int rc = fstat(fd, &sbuf);
    assert(rc > -1);

    int image_size = (int)sbuf.st_size;

    image = mmap(NULL, image_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(image != MAP_FAILED);

    SUPERBLOCK = (super_t *)image;
    inodeMap = image + SUPERBLOCK->inode_bitmap_addr * UFS_BLOCK_SIZE;
    inode_table = image + SUPERBLOCK->inode_region_addr * UFS_BLOCK_SIZE;
    dataMap = image + SUPERBLOCK->data_bitmap_addr * UFS_BLOCK_SIZE;
    data_table = image + SUPERBLOCK->data_region_addr * UFS_BLOCK_SIZE;

    root_inode = inode_table;
    root_dir = image + (root_inode->direct[0] * UFS_BLOCK_SIZE);

    sd = UDP_Open(port);
    assert(sd > -1);
    while (1)
    {
        struct sockaddr_in addr;
        client_message_t message;

        rc = UDP_Read(sd, &addr, (char *)&message, sizeof(message));
        if (rc > 0)
        {
            server_message_t response;
            switch (message.mtype)
            {
            case MFS_LOOKUP:
                response.rc = server_Lookup(message.method.lookup.pinum, message.method.lookup.name);
                UDP_Write(sd, &addr, (char *)&response, sizeof(response));
                break;
            case MFS_STAT:
                response.rc = server_Stat(message.method.stat.inum, &response.stat);
                UDP_Write(sd, &addr, (char *)&response, sizeof(response));
                break;
            case MFS_WRITE:
                response.rc = server_Write(message.method.write.inum, message.method.write.buffer, message.method.write.offset, message.method.write.nbytes);
                UDP_Write(sd, &addr, (char *)&response, sizeof(response));
                break;
            case MFS_READ:
                response.rc = server_Read(message.method.read.inum, response.buffer, message.method.read.offset, message.method.read.nbytes);
                UDP_Write(sd, &addr, (char *)&response, sizeof(response));
                break;
            case MFS_CRET:
                response.rc = server_Create(message.method.create.pinum, message.method.create.type, message.method.create.name);
                UDP_Write(sd, &addr, (char *)&response, sizeof(response));
                break;
            case MFS_UNLINK:
                response.rc = server_Unlink(message.method.unlink.pinum, message.method.unlink.name);
                UDP_Write(sd, &addr, (char *)&response, sizeof(response));
                break;
            case MFS_SHUTDOWN:
                response.rc = server_Shutdown();
                UDP_Write(sd, &addr, (char *)&response, sizeof(response));
                UDP_Close(sd);
                exit(0);
                break;
            default:
                response.rc = -1;
                UDP_Write(sd, &addr, (char *)&response, sizeof(response));
                break;
            }
        }
        else
        {
            printf("Do not get message");
            close(fd);
            return 0;
        }
    }

}
/**
 * lookup in directory of pinum for file with name, return -1 if failed,
 * return inode number otherwise
 */
int server_Lookup(int pinum, char *name)
{
    // todo: not sure if lseek and read is neccessary for finding inode, need to check on that
    if (pinum > SUPERBLOCK->num_inodes - 1 || pinum < 0)
        return -1;
    unsigned int bit = get_bit(inodeMap, pinum);
    if ((int)bit == 0)
        return -1;

    char pBuffer[UFS_BLOCK_SIZE];
    int pBlockOffset = (pinum * sizeof(inode_t)) / UFS_BLOCK_SIZE;
    lseek(fd, (SUPERBLOCK->inode_region_addr + pBlockOffset) * UFS_BLOCK_SIZE, SEEK_SET);
    read(fd, pBuffer, UFS_BLOCK_SIZE);
    inode_block *pinodeBlockPtr = (inode_block *)pBuffer;
    int pOffset = pinum - pBlockOffset * UFS_BLOCK_SIZE / sizeof(inode_t);
    inode_t *pinode = &pinodeBlockPtr->inodes[pOffset];

    if (pinode->type != UFS_DIRECTORY)
        return -1;

    for (int i = 0; i < DIRECT_PTRS; i++)
    {
        // let's say it's the blcok address in block, starting from super block
        int blockNum = pinode->direct[i];
        if (blockNum == -1)
            continue;
        char buffer[UFS_BLOCK_SIZE];
        lseek(fd, blockNum * UFS_BLOCK_SIZE, SEEK_SET);
        read(fd, buffer, UFS_BLOCK_SIZE);
        dir_block_t *entryblock = (dir_block_t *)buffer;
        for (int j = 0; j < 128; j++)
        {
            if (entryblock->entries[j].inum == -1)
                continue;
            if (strcmp(name, entryblock->entries[j].name) == 0)
            {
                return entryblock->entries[j].inum;
            }
        }
    }
    return -1;
}

int server_Stat(const int inum, MFS_Stat_t *m)
{
    if (inum > SUPERBLOCK->num_inodes - 1 || inum < 0)
        return -1;
    unsigned int bit = get_bit(inodeMap, inum);
    if ((int)bit == 0)
        return -1;

    char buffer[UFS_BLOCK_SIZE];
    int blockOffset = (inum * sizeof(inode_t)) / UFS_BLOCK_SIZE;
    lseek(fd, (SUPERBLOCK->inode_region_addr + blockOffset) * UFS_BLOCK_SIZE, SEEK_SET);
    read(fd, buffer, UFS_BLOCK_SIZE);
    inode_block *inodeBlockPtr = (inode_block *)buffer;
    int offset = inum - blockOffset * UFS_BLOCK_SIZE / sizeof(inode_t);
    inode_t *target = &inodeBlockPtr->inodes[offset];

    m->type = target->type;
    m->size = target->size;
    return 0;
}

int server_Write(int inum, char *buffer, int offset, int nbytes)
{
    if (inum > SUPERBLOCK->num_inodes - 1 || inum < 0 || nbytes > 4096 || nbytes < 0)
        return -1;
    unsigned int bit = get_bit(inodeMap, inum);
    if ((int)bit == 0)
        return -1;

    int directNum = offset / UFS_BLOCK_SIZE;
    int inBlockOffset = offset % UFS_BLOCK_SIZE;

    if (directNum > 29)
        return -1;
    if (directNum == 29 && (inBlockOffset + nbytes) > UFS_BLOCK_SIZE)
        return -1;

    char Buffer[UFS_BLOCK_SIZE];
    int BlockOffset = (inum * sizeof(inode_t)) / UFS_BLOCK_SIZE;
    lseek(fd, (SUPERBLOCK->inode_region_addr + BlockOffset) * UFS_BLOCK_SIZE, SEEK_SET);
    read(fd, Buffer, UFS_BLOCK_SIZE);
    inode_block *inodeBlockPtr = (inode_block *)Buffer;
    int Offset = inum - BlockOffset * UFS_BLOCK_SIZE / sizeof(inode_t);
    inode_t *target = &inodeBlockPtr->inodes[Offset];
    int targetBlock = target->direct[directNum];

    if (target->type != UFS_REGULAR_FILE)
        return -1;

    // No data block assigned to this offset
    if (targetBlock == -1)
    {   
        int assigned = 0;
        for (int i = 0; i < SUPERBLOCK->data_bitmap_len * UFS_BLOCK_SIZE; i++)
        {
            if (get_bit(dataMap, i) == 0)
            {
                set_bit(dataMap, i);
                target->direct[directNum] = SUPERBLOCK->data_region_addr + i;
                assigned = 1;
                break;
            }
        }
        if (assigned != 1)
            return -1;
    }
    if (inBlockOffset + nbytes > 4096)
    {
        int rc = lseek(fd, target->direct[directNum] * UFS_BLOCK_SIZE + inBlockOffset, SEEK_SET);
        if (rc == -1)
            return -1;

        rc = write(fd, buffer, UFS_BLOCK_SIZE - inBlockOffset);
        if (rc == -1)
            return -1;
        unsigned int targetBlock2 = target->direct[directNum + 1];
        if ((int)targetBlock2 == -1)
        {

            int assigned2 = 0;
            for (int i = 0; i < SUPERBLOCK->data_bitmap_len * UFS_BLOCK_SIZE; i++)
            {
                if (get_bit(dataMap, i) == 0)
                {
                    set_bit(dataMap, i);
                    target->direct[directNum] = SUPERBLOCK->data_region_addr + i;
                    assigned2 = 1;
                    break;
                }
            }
            if (assigned2 != 1)
                return -1;
        }
        rc = lseek(fd, target->direct[directNum + 1] * UFS_BLOCK_SIZE, SEEK_SET);
        if (rc == -1)
            return -1;
        rc = write(fd, buffer + UFS_BLOCK_SIZE - inBlockOffset, inBlockOffset + nbytes - UFS_BLOCK_SIZE);
        if (rc == -1)
            return -1;
    }
    else
    {
        int rc = lseek(fd, target->direct[directNum] * UFS_BLOCK_SIZE + inBlockOffset, SEEK_SET);
        if (rc == -1)
        {
            target->direct[directNum] = -1;
            return -1;
        }

        rc = write(fd, buffer, nbytes);
        if (rc == -1)
        {
            target->direct[directNum] = -1;
            return -1;
        }
    }
    target->size = offset + nbytes;
    int rc2 = pwrite(fd, target, sizeof(inode_t),
         (SUPERBLOCK->inode_region_addr + BlockOffset) * UFS_BLOCK_SIZE + Offset * sizeof(inode_t));
    assert(rc2 == sizeof(inode_t));
    fsync(fd);
    return 0;
}

int server_Read(const int inum, char *buffer, int offset, int nbytes)
{
    if (inum > SUPERBLOCK->num_inodes - 1 || inum < 0 || nbytes > 4096 || nbytes < 0)
        return -1;
    unsigned int bit = get_bit(inodeMap, inum);
    if ((int)bit == 0)
        return -1;

    int directNum = offset / UFS_BLOCK_SIZE;
    int inBlockOffset = offset % UFS_BLOCK_SIZE;
    
    if (directNum > 29)
    {
        return -1;
    }

    char Buffer[UFS_BLOCK_SIZE];
    int blockOffset = (inum * sizeof(inode_t)) / UFS_BLOCK_SIZE;
    lseek(fd, (SUPERBLOCK->inode_region_addr + blockOffset) * UFS_BLOCK_SIZE, SEEK_SET);
    read(fd, Buffer, UFS_BLOCK_SIZE);
    // inode_block* inodeBlockPtr = malloc(sizeof(inode_block));
    inode_block *inodeBlockPtr = (inode_block *)Buffer;
    int inumOffset = inum - blockOffset * UFS_BLOCK_SIZE / sizeof(inode_t);
    inode_t *target = &inodeBlockPtr->inodes[inumOffset];

    int targetBlock = target->direct[directNum];

    if (inBlockOffset + nbytes <= UFS_BLOCK_SIZE)
    {
        if (target->type == UFS_DIRECTORY)
        {
            if (inBlockOffset % sizeof(dir_ent_t) != 0 || nbytes != sizeof(dir_ent_t))
            {
                return -1;
            }
        }

        if (targetBlock == -1)
        {
            return -1;
        }

        int rc = lseek(fd, target->direct[directNum] * UFS_BLOCK_SIZE + inBlockOffset, SEEK_SET);
        if (rc == -1)
        {
            return -1;
        }

        rc = read(fd, buffer, nbytes);
        if (rc == -1)
        {
            return -1;
        }
    }
    else
    {
        if (directNum == 29)
        {
            return -1;
        }

        if (target->type == UFS_DIRECTORY)
        {
            if (inBlockOffset % sizeof(dir_ent_t) != 0 || nbytes != sizeof(dir_ent_t))
            {
                return -1;
            }
        }

        int nextBlock = target->direct[directNum + 1];

        if (targetBlock == -1 || nextBlock == -1)
        {
            return -1;
        }

        int rc = lseek(fd, targetBlock * UFS_BLOCK_SIZE + inBlockOffset, SEEK_SET);
        if (rc == -1)
        {
            return -1;
        }

        rc = read(fd, buffer, UFS_BLOCK_SIZE - inBlockOffset);
        if (rc == -1)
        {
            return -1;
        }

        rc = lseek(fd, nextBlock * UFS_BLOCK_SIZE, SEEK_SET);
        if (rc == -1)
        {
            return -1;
        }

        rc = read(fd, buffer + UFS_BLOCK_SIZE - inBlockOffset, inBlockOffset + nbytes - UFS_BLOCK_SIZE);
        if (rc == -1)
        {
            return -1;
        }
    }
    return 0;
}

int server_Create(int pinum, int type, char *name)
{

    if (pinum > SUPERBLOCK->num_inodes - 1 || pinum < 0)
    {
        return -1;
    }

    unsigned int bit = get_bit(inodeMap, pinum);

    if ((int)bit == 0)
    {
        return -1;
    }
    if (strlen(name) > 28 || strlen(name) < 1)
        return -1;

    char pBuffer[UFS_BLOCK_SIZE];
    int pBlockOffset = (pinum * sizeof(inode_t)) / UFS_BLOCK_SIZE;
    lseek(fd, (SUPERBLOCK->inode_region_addr + pBlockOffset) * UFS_BLOCK_SIZE, SEEK_SET);
    read(fd, pBuffer, UFS_BLOCK_SIZE);
    inode_block *pinodeBlockPtr = (inode_block *)pBuffer;
    int pOffset = pinum - pBlockOffset * UFS_BLOCK_SIZE / sizeof(inode_t);
    inode_t *pinode = &pinodeBlockPtr->inodes[pOffset];

    if (pinode->type != MFS_DIRECTORY)
    {
        return -1;
    }

    for (int i = 0; i < DIRECT_PTRS; i++)
    {
        // let's say it's the blcok address in block, starting from super block
        int blockNum = pinode->direct[i];

        if (blockNum == -1)
            continue;
        char buffer[UFS_BLOCK_SIZE];
        lseek(fd, blockNum * UFS_BLOCK_SIZE, SEEK_SET);
        read(fd, buffer, UFS_BLOCK_SIZE);
        dir_block_t *entryblock = (dir_block_t *)buffer;
        for (int j = 0; j < 128; j++)
        {
            if (entryblock->entries[j].inum == -1)
                continue;

            if (strcmp(name, entryblock->entries[j].name) == 0)
            {
                return 0;
            }
        }
    }

    int i;
    int assigned = 0;
    int blockOffset;
    inode_block *inodeBlockPtr;

    for (i = 0; i < SUPERBLOCK->num_inodes; i++)
    {
        if (get_bit(inodeMap, i) == 0)
        {
            set_bit(inodeMap, i);
            // write inodeMap to disk
            char buffer[UFS_BLOCK_SIZE];
            blockOffset = (i * sizeof(inode_t)) / UFS_BLOCK_SIZE;
            lseek(fd, (SUPERBLOCK->inode_region_addr + blockOffset) * UFS_BLOCK_SIZE, SEEK_SET);
            read(fd, buffer, UFS_BLOCK_SIZE);
            inodeBlockPtr = (inode_block *)buffer;
            int inBlockOffset = i - blockOffset * UFS_BLOCK_SIZE / sizeof(inode_t);
            // inode_t newInode = inodeBlockPtr->inodes[inBlockOffset];

            inodeBlockPtr->inodes[inBlockOffset].size = 0;
            inodeBlockPtr->inodes[inBlockOffset].type = type;
            for (int index = 0; index < DIRECT_PTRS; index++)
            {
                inodeBlockPtr->inodes[inBlockOffset].direct[index] = -1;
            }

            if (type == MFS_DIRECTORY)
            {
                inodeBlockPtr->inodes[inBlockOffset].size = 2 * sizeof(dir_ent_t);
                // find a new datablock for this newInode, set direct[0] to this block. In this block, set two dir_t, let the first one be self, the second one be the parent, left be -1
                // after finishing this, write this block to disk
                for (int j = 0; j < SUPERBLOCK->num_data; j++)
                {
                    if (get_bit(dataMap, j) == 0)
                    {
                        set_bit(dataMap, j);
                        char buffer[UFS_BLOCK_SIZE];
                        lseek(fd, (SUPERBLOCK->data_region_addr + j) * UFS_BLOCK_SIZE, SEEK_SET);
                        read(fd, buffer, UFS_BLOCK_SIZE);
                        dir_block_t *entryblock = (dir_block_t *)buffer;

                        entryblock->entries[0].inum = i;
                        strcpy(entryblock->entries[0].name, ".");
                        entryblock->entries[1].inum = pinum;
                        strcpy(entryblock->entries[1].name, "..");
                        for (int a = 2; a < 128; a++)
                        {
                            entryblock->entries[a].inum = -1;
                        }
                        int rc = pwrite(fd, entryblock, UFS_BLOCK_SIZE, (SUPERBLOCK->data_region_addr + j) * UFS_BLOCK_SIZE);
                        assert(rc == UFS_BLOCK_SIZE);
                        inodeBlockPtr->inodes[inBlockOffset].direct[0] = j + SUPERBLOCK->data_region_addr;
                        break;
                    }
                }
            }
            // write newInode to disk
            assigned = 1;
            break;
        }
    }

    if (assigned != 1)
    {
        return -1;
    }

    int rc = pwrite(fd, inodeBlockPtr, UFS_BLOCK_SIZE, (SUPERBLOCK->inode_region_addr + blockOffset) * UFS_BLOCK_SIZE);
    assert(rc == UFS_BLOCK_SIZE);

    int full = 0;
    for (int j = 0; j < DIRECT_PTRS; j++)
    {
        unsigned int blockNum = pinode->direct[j];

        if (blockNum == -1 && full == 0)
        {
            for (int k = 0; k < SUPERBLOCK->num_data; k++)
            {
                if (get_bit(dataMap, k) == 0)
                {

                    set_bit(dataMap, k);
                    char buffer[UFS_BLOCK_SIZE];
                    // void* blockAddr = (void*)(SUPERBLOCK->data_region_addr + k * UFS_BLOCK_SIZE);
                    lseek(fd, (SUPERBLOCK->data_region_addr + k) * UFS_BLOCK_SIZE, SEEK_SET);
                    read(fd, buffer, UFS_BLOCK_SIZE);
                    dir_block_t *entryblock = (dir_block_t *)buffer;
                    entryblock->entries[0].inum = i;
                    strcpy(entryblock->entries[0].name, name);
                    for (int m = 1; m < 128; m++)
                    {
                        entryblock->entries[m].inum = -1;
                    }
                    int rc = pwrite(fd, entryblock, UFS_BLOCK_SIZE, (SUPERBLOCK->data_region_addr + k) * UFS_BLOCK_SIZE);
                    assert(rc == UFS_BLOCK_SIZE);

                    pinode->size += sizeof(dir_ent_t);
                    pinode->direct[j] = SUPERBLOCK->data_region_addr + k;

                    // write pinode to disk
                    rc = pwrite(fd, pinode, sizeof(inode_t), (SUPERBLOCK->inode_region_addr + pBlockOffset) * UFS_BLOCK_SIZE + pOffset * sizeof(inode_t));
                    assert(rc == sizeof(inode_t));
                    fsync(fd);
                    return 0;
                }
            }
            full = 1;
        }
        else
        {
            char buffer[UFS_BLOCK_SIZE];
            lseek(fd, blockNum * UFS_BLOCK_SIZE, SEEK_SET);
            read(fd, buffer, UFS_BLOCK_SIZE);
            dir_block_t *entryblock = (dir_block_t *)buffer;

            for (int k = 0; k < 128; k++)
            {
                if (entryblock->entries[k].inum == -1)
                {
                    entryblock->entries[k].inum = i;
                    strcpy(entryblock->entries[k].name, name);
                    pinode->size += sizeof(dir_ent_t);
                    int rc = pwrite(fd, entryblock, UFS_BLOCK_SIZE, blockNum * UFS_BLOCK_SIZE);
                    assert(rc == UFS_BLOCK_SIZE);

                    // write pinode to disk
                    rc = pwrite(fd, pinode, sizeof(inode_t), (SUPERBLOCK->inode_region_addr + pBlockOffset) * UFS_BLOCK_SIZE + pOffset * sizeof(inode_t));
                    assert(rc == sizeof(inode_t));
                    fsync(fd);

                    return 0;
                }
            }
        }
    }
    return -1;
} 

int server_Unlink(int pinum, char *name)
{
    for(int m = 0; m < SUPERBLOCK->num_inodes;m++)
    if (pinum > SUPERBLOCK->num_inodes - 1 || pinum < 0)
        return -1;
    unsigned int bit = get_bit(inodeMap, pinum);
    if ((int)bit == 0)
        return -1;
    if (strlen(name) > 28 || strlen(name) < 1)
        return 0;
    //find parent inode
    char pBuffer[UFS_BLOCK_SIZE];
    int pBlockOffset = (pinum * sizeof(inode_t)) / UFS_BLOCK_SIZE;
    lseek(fd, (SUPERBLOCK->inode_region_addr + pBlockOffset) * UFS_BLOCK_SIZE, SEEK_SET);
    read(fd, pBuffer, UFS_BLOCK_SIZE);
    inode_block *pinodeBlockPtr = (inode_block *)pBuffer;
    int pOffset = pinum - pBlockOffset * UFS_BLOCK_SIZE / sizeof(inode_t);
    inode_t *pinode = &pinodeBlockPtr->inodes[pOffset];

    if (pinode->type != UFS_DIRECTORY)
        return -1;

    dir_ent_t *targetEntry;
    targetEntry->inum = -1;
    dir_block_t *entryblock;
    int i;
    int j;
    int found = 0;

    for (i = 0; i < DIRECT_PTRS; i++)
    {
        int blockNum = pinode->direct[i];
        if (blockNum == -1)
            continue;
        char buffer[UFS_BLOCK_SIZE];
        lseek(fd, blockNum * UFS_BLOCK_SIZE, SEEK_SET);
        read(fd, buffer, UFS_BLOCK_SIZE);
        entryblock = (dir_block_t *)buffer;
        for (j = 0; j < 128; j++)
        {
            if (entryblock->entries[j].inum == -1)
                continue;
            if (strcmp(name, entryblock->entries[j].name) == 0)
            {
                found = 1;
                targetEntry = &entryblock->entries[j];
                break;
            }
        }
        if(found == 1)
            break;
    }

    if (targetEntry->inum == -1)
        return 0;

    int inum = targetEntry->inum;

    char Buffer[UFS_BLOCK_SIZE];
    int BlockOffset = (inum * sizeof(inode_t)) / UFS_BLOCK_SIZE;
    lseek(fd, (SUPERBLOCK->inode_region_addr + BlockOffset) * UFS_BLOCK_SIZE, SEEK_SET);
    read(fd, Buffer, UFS_BLOCK_SIZE);
    inode_block *inodeBlockPtr = (inode_block *)Buffer;
    int Offset = inum - BlockOffset * UFS_BLOCK_SIZE / sizeof(inode_t);
    inode_t *target = &inodeBlockPtr->inodes[Offset];

    if (target->type == UFS_DIRECTORY)
    {
        int blockNum = target->direct[0];
        char buffer[UFS_BLOCK_SIZE];
        lseek(fd, blockNum * UFS_BLOCK_SIZE, SEEK_SET);
        read(fd, buffer, UFS_BLOCK_SIZE);
        dir_block_t *entryblock2 = (dir_block_t *)buffer;
        for(int j = 2; j < 128; j++)
        {
            if (entryblock2->entries[j].inum != -1)
                return -1;
        }

        for (int i = 1; i < DIRECT_PTRS; i++)
        {
            if (target->direct[i] != -1)
                return -1;
        }
        targetEntry->inum = -1;
        set_bit_zero(inodeMap, inum);
    }
    else
    {
        targetEntry->inum = -1;
        set_bit_zero(inodeMap, inum);
        for (int i = 0; i < DIRECT_PTRS; i++)
        {
            if (target->direct[i] != -1)
            {
                target->direct[i] == -1;
                set_bit_zero(dataMap, target->direct[i] - SUPERBLOCK->data_region_addr);
            }
        }
    }
    pinode->size -= sizeof(dir_ent_t);
    int rc = pwrite(fd, targetEntry,sizeof(dir_ent_t) , (SUPERBLOCK->data_region_addr + i) * UFS_BLOCK_SIZE + j*sizeof(dir_ent_t));
    assert(rc == sizeof(dir_ent_t));
    rc = pwrite(fd, pinode, sizeof(inode_t), (SUPERBLOCK->inode_region_addr + pBlockOffset) * UFS_BLOCK_SIZE + pOffset * sizeof(inode_t));
    assert(rc == sizeof(inode_t));
    fsync(fd);
    return 0;
}

int server_Shutdown()
{
    bitmap_t *inodeBitMap = (bitmap_t *)(image + SUPERBLOCK->inode_bitmap_addr * UFS_BLOCK_SIZE);
    int rc = pwrite(fd, inodeBitMap, SUPERBLOCK->inode_bitmap_len * UFS_BLOCK_SIZE, SUPERBLOCK->inode_bitmap_addr * UFS_BLOCK_SIZE);
    assert(rc == SUPERBLOCK->inode_bitmap_len * UFS_BLOCK_SIZE);

    bitmap_t *dataBitMap = (bitmap_t *)(image + SUPERBLOCK->data_bitmap_addr * UFS_BLOCK_SIZE);
    rc = pwrite(fd, dataBitMap, SUPERBLOCK->data_bitmap_len * UFS_BLOCK_SIZE, SUPERBLOCK->data_bitmap_addr * UFS_BLOCK_SIZE);
    assert(rc == SUPERBLOCK->data_bitmap_len * UFS_BLOCK_SIZE);
    int ret = fsync(fd);
    close(fd);

    if (ret < 0)
    {
        return -1;
    }
    else
    {
        return 0;
    }
}