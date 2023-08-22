#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mfs.h"
#include "udp.h"
#include "message.h"

int fd;
struct sockaddr_in server_addr;

int retry()
{
    struct timeval time;
    fd_set set;
    time.tv_sec = 5;
    time.tv_usec = 0;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    return select(fd + 1, &set, NULL, NULL, &time);
}

int MFS_Init(char *hostname, int port)
{
    int rc = UDP_FillSockAddr(&server_addr, hostname, port);
    if (rc != 0)
    {
        printf("Failed to set up server address");
        return rc;
    }

    fd = UDP_Open(3473);
    return 0;
}

int MFS_Lookup(int pinum, char *name)
{
    client_message_t message;
    message.mtype = MFS_LOOKUP;
    message.method.lookup.pinum = pinum;
    strcpy(message.method.lookup.name, name);

    int rc = UDP_Write(fd, &server_addr, (char*)&message, sizeof(message));
    rc = retry();

    if(rc >= 0)
    {
        server_message_t response;
        struct sockaddr_in ret_addr;

        rc = UDP_Read(fd, &ret_addr, (char*)&response, sizeof(response));

        return response.rc;
    }
    else
    {
        return -1;
    }
}

int MFS_Stat(int inum, MFS_Stat_t *m)
{
    client_message_t message;
    message.mtype = MFS_STAT;
    message.method.stat.inum = inum;

    int rc = UDP_Write(fd, &server_addr, (char*)&message, sizeof(message));
    rc = retry();

    if(rc >= 0)
    {
        server_message_t response;
        struct sockaddr_in ret_addr;
    
        rc = UDP_Read(fd, &ret_addr, (char*)&response, sizeof(response));

        if(response.rc >= 0)
        {
            m->size = response.stat.size;
            m->type = response.stat.type;
            printf("%d,%d\n",m->size,m->type);
        }

        return response.rc;
    }
    else
    {
        return -1;
    }
}

int MFS_Write(int inum, char *buffer, int offset, int nbytes)
{
    client_message_t message;
    message.mtype = MFS_WRITE;
    message.method.write.inum = inum;
    message.method.write.offset = offset;
    message.method.write.nbytes = nbytes;
    memcpy(message.method.write.buffer, buffer,4096);

    int rc = UDP_Write(fd, &server_addr, (char*)&message, sizeof(message));
    rc = retry();

    if(rc >= 0)
    {
        server_message_t response;
        struct sockaddr_in ret_addr;
    
        rc = UDP_Read(fd, &ret_addr, (char*)&response, sizeof(response));

        return response.rc;
    }
    else
    {
        return -1;
    }
}

int MFS_Read(int inum, char *buffer, int offset, int nbytes)
{
    client_message_t message;
    message.mtype = MFS_READ;
    message.method.read.inum = inum;
    message.method.read.offset = offset;
    message.method.read.nbytes = nbytes;

    int rc = UDP_Write(fd, &server_addr, (char*)&message, sizeof(message));
    rc = retry();

    if(rc >= 0)
    {
        server_message_t response;
        struct sockaddr_in ret_addr;
    
        rc = UDP_Read(fd, &ret_addr, (char*)&response, sizeof(response));

        if(response.rc >= 0)
        {
            memcpy(buffer, response.buffer,nbytes);
        }
        return response.rc;
    }
    else
    {
        return -1;
    }
}

int MFS_Creat(int pinum, int type, char *name)
{
    client_message_t message;
    message.mtype = MFS_CRET;
    message.method.create.pinum = pinum;
    message.method.create.type = type;
    strcpy(message.method.create.name, name);

    int rc = UDP_Write(fd, &server_addr, (char*)&message, sizeof(message));
    rc = retry();

    if(rc >= 0)
    {
        server_message_t response;
        struct sockaddr_in ret_addr;
    
        rc = UDP_Read(fd, &ret_addr, (char*)&response, sizeof(response));

        return response.rc;
    }
    else
    {
        return -1;
    }
}

int MFS_Unlink(int pinum, char *name)
{
    client_message_t message;
    message.mtype = MFS_UNLINK;
    message.method.unlink.pinum = pinum;
    strcpy(message.method.unlink.name, name);

    int rc = UDP_Write(fd, &server_addr, (char*)&message, sizeof(message));
    rc = retry();

    if(rc >= 0)
    {
        server_message_t response;
        struct sockaddr_in ret_addr;
    
        rc = UDP_Read(fd, &ret_addr, (char*)&response, sizeof(response));

        return response.rc;
    } 
    else
    {
        return -1;
    }
}

int MFS_Shutdown()
{
    client_message_t message;
    message.mtype = MFS_SHUTDOWN;

    int rc = UDP_Write(fd, &server_addr, (char*)&message, sizeof(message));
    // rc = retry();

    if(rc >= 0)
    {
        server_message_t response;
        struct sockaddr_in ret_addr;
    
        rc = UDP_Read(fd, &ret_addr, (char*)&response, sizeof(response));
        UDP_Close(fd);
        return response.rc;
    }
    else
    {
        return -1;
    }
}
