#ifndef SERVER_ROLES_H
#define SERVER_ROLES_H

#include <pthread.h> 
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <arpa/inet.h> 
#include <sys/socket.h>  
#include <netinet/in.h>
#include <stdatomic.h>


#include "../util/communication.h"
#include "./replica.h"

enum ServerMode{ UNKNOWN_MODE, BACKUP, BACKUP_MANAGER };


typedef struct {
    int id;
    char hostname[256];
    int port;
    int my_base_port;           
} BackupArgs;

typedef struct { 
    int id;
    int port;
} ManagerArgs;

extern atomic_int global_server_mode;
extern atomic_int global_shutdown_flag;
extern pthread_mutex_t mode_change_mutex; 

void *replica_listener_thread(void *arg);

#endif 
