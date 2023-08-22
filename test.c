/**
 * @brief Wrapper for the MFS write function in the server side
 *
 * @param nbytes
 * @param offset
 * @param inum
 * @param inode_table
 * @param data_bitmap
 * @param inode_bitmap
 * @param buffer
 * @param superBlock
 * @param image
 * @return int
 */
int MFS_write(int nbytes, int offset, int inum, inode_t *inode_table, char *data_bitmap, char *inode_bitmap, char *buffer, super_t *superBlock, void *image)
{
    // precheck
    if (nbytes <= 0 || nbytes > BLOCK_SIZE || offset < 0 || offset + nbytes > BLOCK_SIZE * DIRECT_PTRS)
    {
        return -1;
    }
    // Check if it is a regular file
    inode_t metadata = inode_table[inum];
    if (metadata.type == 0)
    { // directory {}
        return -1;
    }

    int numBlockToWrite = findNoBlockAlloc(offset, nbytes); // Number of Data Block To Write: 1 or 2
    unsigned int comparison = -1;

    int numByteToWriteFirstBlock = numBlockToWrite == 2 ? BLOCK_SIZE - offset : nbytes;
    int startAddrFirstBlockOffset = offset % BLOCK_SIZE;
    int numByteToWriteSecondBlock = numBlockToWrite == 1 ? 0 : nbytes - numByteToWriteFirstBlock;

    int locationFirstBlock = offset / BLOCK_SIZE;
    int locationFirstBlockNum = metadata.direct[locationFirstBlock];
    int locationSecondBlock = locationFirstBlock + 1; // prove this
    int locationSecondBlockNum = metadata.direct[locationSecondBlock];

    // Operation on First Block
    int firstBlockAllocated = locationFirstBlockNum == comparison ? 0 : 1;
    int emptySlot;
    if (firstBlockAllocated == 0)
    {
        firstBlockAllocated = find_empty_set_bitmap((unsigned int *)data_bitmap, superBlock->num_data, &emptySlot);
        metadata.direct[locationFirstBlock] = emptySlot;
    }
    
    locationFirstBlockNum = metadata.direct[locationFirstBlock];

    if (firstBlockAllocated == 0)
    {
        return -1;
    }

    char *startAddr = image + superBlock->data_region_addr * BLOCK_SIZE + BLOCK_SIZE * locationFirstBlockNum + startAddrFirstBlockOffset;
    // Write to persistency file
    memcpy(startAddr, buffer, numByteToWriteFirstBlock);
    msync(startAddr, numByteToWriteFirstBlock, MS_SYNC);

    if (numBlockToWrite == 2)
    {
        // Operation on Second Block
        int secondBlockAllocated = locationSecondBlockNum == comparison ? 0 : 1;
        if (secondBlockAllocated == 0)
        {
            secondBlockAllocated = find_empty_set_bitmap((unsigned int *)data_bitmap, superBlock->num_data, &emptySlot);
            metadata.direct[locationSecondBlock] = emptySlot;
        }

        
        locationSecondBlockNum = metadata.direct[locationSecondBlock];

        if (secondBlockAllocated == 0)
        {
            return -1;
        }

        char *startAddr2 = image + superBlock->data_region_addr * BLOCK_SIZE + BLOCK_SIZE * locationSecondBlockNum;
        // Write to persistency file
        memcpy(startAddr2, buffer, numByteToWriteSecondBlock);
        msync(startAddr2, numByteToWriteSecondBlock, MS_SYNC);
    }

    // update the size accordingly
    metadata.size = (nbytes + offset) > metadata.size ? nbytes + offset : metadata.size;

    msync(inode_bitmap, superBlock->inode_bitmap_len * BLOCK_SIZE, MS_SYNC);
    msync(data_bitmap, superBlock->data_bitmap_len * BLOCK_SIZE, MS_SYNC);
    memcpy(inode_table + inum, &metadata, sizeof(inode_t));
    msync(inode_table, superBlock->num_inodes * BLOCK_SIZE, MS_SYNC);
    return 0;
}