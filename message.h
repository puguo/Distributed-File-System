#ifndef __message_h__
#define __message_h__

#define MAX_NAME_LEN 28

#define MFS_INIT (1)
#define MFS_LOOKUP (2)
#define MFS_STAT (3)
#define MFS_WRITE (4)
#define MFS_READ (5)
#define MFS_CRET (6)
#define MFS_UNLINK (7)
#define MFS_SHUTDOWN (8)

#include "mfs.h"

typedef struct _client_message
{
    int mtype; // message type from above

    union
    {
        struct
        {
            int pinum;
            char name[MAX_NAME_LEN];
        } lookup;
        struct 
        {
            int pinum;
            int type;
            char name[MAX_NAME_LEN];
        } create;
        struct 
        {
            int inum;
            int offset;
            int nbytes;
            char buffer[4096];
        } write;
        struct 
        {
            int inum;
            int offset;
            int nbytes;
        } read;
        struct message
        {
            int pinum;
            char name[MAX_NAME_LEN];
        } unlink;
        struct
        {
            int inum;
        } stat;
    } method;
} client_message_t;

typedef struct _server_message
{
    int rc;    // return code
    char buffer[4096];
    MFS_Stat_t stat;
    MFS_DirEnt_t dir;
    // union 
    // {
    //     MFS_STAT_t stat;
    // };
    
} server_message_t;

#endif // __message_h__