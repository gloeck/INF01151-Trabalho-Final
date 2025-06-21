#include "./serverRoles.h"
#include "./serverCommon.h"
#include "replica.h"
#include "election.h"
#include <bits/pthreadtypes.h>
#include <time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>


#define HEARTBEAT_TIMEOUT_SECONDS 5

atomic_int global_server_mode = UNKNOWN_MODE;
atomic_int global_shutdown_flag = 0;
time_t last_heartbeat;

pthread_mutex_t mode_change_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t heartbeat_mutex = PTHREAD_MUTEX_INITIALIZER;

void send_heartbeat_to_replicas() {
    ReplicaEvent event;
    create_heartbeat_event(&event);
    notify_replicas(&event);
    free_event(&event);
    //fprintf(stderr, "Heartbeat to %d replicas!\n", notified_replicas);
}

void heartbeat_received(){
    pthread_mutex_lock(&heartbeat_mutex);
    last_heartbeat = time(NULL);
    pthread_mutex_unlock(&heartbeat_mutex);
}

int is_manager_running() {
    pthread_mutex_lock(&heartbeat_mutex);
    int time_diff = difftime(time(NULL), last_heartbeat);
    pthread_mutex_unlock(&heartbeat_mutex);

    if (time_diff > HEARTBEAT_TIMEOUT_SECONDS) 
        return 0;
    return 1;
}


int has_data(int socketfd, int timeout_ms) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(socketfd, &read_fds);

    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    int result = select(socketfd + 1, &read_fds, NULL, NULL, &timeout);
    if (result < 0) {
        perror("select failed");
        return -1;
    }

    return result; 
}


void *replica_listener_thread(void *arg) {
    ManagerArgs *manager_args = (ManagerArgs *)arg;
    int port = manager_args->port;

    int listen_sockfd, newsockfd;
    struct sockaddr_in sockaddr, cli_addr;
    socklen_t clilen;

    printf("[Manager Listener Thread] Starting on port %d.\n", port);

    if ((listen_sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("[Manager Listener Thread] ERROR opening socket");
        free(manager_args);
        pthread_exit(NULL);
    }

    sockaddr.sin_family = AF_INET;
    sockaddr.sin_port = htons(port);
    sockaddr.sin_addr.s_addr = INADDR_ANY;
    bzero(&(sockaddr.sin_zero), 8); 


    if (bind(listen_sockfd, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) < 0) {
        perror("[Manager Listener Thread] ERROR on binding");
        close(listen_sockfd);
        free(manager_args);
        pthread_exit(NULL);
    }

    if (listen(listen_sockfd, 5) == -1) { 
        perror("[Manager Listener Thread] ERROR on listen");
        close(listen_sockfd);
        free(manager_args);
        pthread_exit(NULL); 
    }

    printf("[Manager Listener Thread] Listening for replica connections on port %d...\n", port);

    while (1) {
        pthread_mutex_lock(&mode_change_mutex);
        int should_exit = atomic_load(&global_shutdown_flag) || (atomic_load(&global_server_mode) != BACKUP_MANAGER);
        pthread_mutex_unlock(&mode_change_mutex);

        if (should_exit) {
            printf("[Manager Listener Thread] Exiting loop due to mode change or global shutdown.\n");
            break; 
        }
        int replica_connected = has_data(listen_sockfd, 1000);
        if ( replica_connected > 0){
             clilen = sizeof(cli_addr);
            newsockfd = accept(listen_sockfd, (struct sockaddr *)&cli_addr, &clilen);

            if (newsockfd == -1) {
                if (errno == EINTR) {
                    // Interrupted by a signal, retry accept
                    continue;
                }
                perror("[Manager Listener Thread] ERROR on accept");
                break;
            }

            Packet packet = read_packet(newsockfd);
            int replica_id = atoi(packet._payload);

            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(cli_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
            fprintf(stderr, "[Manager Listener Thread] Accepted new replica connection from %s:%hu (fd %d) - Id %d\n",
                   ip_str, ntohs(cli_addr.sin_port), newsockfd, replica_id);



            if(!add_replica(newsockfd, replica_id, cli_addr)){
                fprintf(stderr, "[Manager Listener Thread] Error adding replica");
                continue;
            }

            ReplicaEvent event;
            create_replica_added_event(&event , replica_id, cli_addr);
            notify_replicas(&event);
            free_event(&event);

        } else if (replica_connected < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("[Manager Listener Thread] Select error");
            break; 
        } else if (replica_connected == 0) {
            // Timeout occurred, no new connections, loop back to check flags again
            continue;
        }
    }

    pthread_exit(NULL);
}

void *heartbeat_monitor_thread_main(void *arg) {
    int id = *(int*)arg;
    printf("[Backup Thread] Heartbeat monitor started for Backup ID %d.\n", id);

    while (1) {
        pthread_mutex_lock(&mode_change_mutex);
        int should_exit = atomic_load(&global_shutdown_flag) || (atomic_load(&global_server_mode) != BACKUP);
        pthread_mutex_unlock(&mode_change_mutex);

        if (should_exit) {
            printf("[Backup Thread] Heartbeat monitor exiting due to mode change or shutdown.\n");
            break;
        }

        if (!is_manager_running()) {
            fprintf(stderr, "[Backup %d Connection Thread] No heartbeat received in the last %d seconds. Connection might be dead.\n", 
                id, HEARTBEAT_TIMEOUT_SECONDS);
            pthread_mutex_lock(&mode_change_mutex);
            if (atomic_load(&global_server_mode) == BACKUP) { 
                printf("[Backup %d] Manager heartbeats stopped. Initiating failover.\n", id);
                if (run_election(id)) { 
                    atomic_store(&global_server_mode,BACKUP_MANAGER);
                    printf("[Backup %d] Role changed to MANAGER.\n", id);
                } else {
                    printf("[Backup %d] Failed to win election. Remaining BACKUP.\n", id);
                }
            }
            pthread_mutex_unlock(&mode_change_mutex);
        }

        sleep(1); 
    }
    pthread_exit(NULL);
}


void *connect_to_server_thread(void *arg) {
    BackupArgs *backup_args = (BackupArgs *)arg;
    int id = backup_args->id;
    const char *hostname = backup_args->hostname;
    int port = backup_args->port;

    

    int socketfd = -1;
    printf("[Backup %d Connection Thread] Attempting to connect to %s:%d.\n", id, hostname, port);

    while (1) {
        pthread_mutex_lock(&mode_change_mutex);
        int should_exit = atomic_load(&global_shutdown_flag) || (atomic_load(&global_server_mode) != BACKUP);
        pthread_mutex_unlock(&mode_change_mutex);

        if (should_exit) {
            printf("[Backup %d Connection Thread] Exiting due to mode change or global shutdown.\n", id);
            break; // Exit the connection/communication loop
        }

        struct hostent *server;
        if ((server = gethostbyname(hostname)) == NULL) {
            fprintf(stderr,"ERRO servidor nao encontrado\n");
            continue;
        }

        if ((socketfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
            fprintf(stderr, "ERRO abrindo o socket da interface\n");
            continue;
        }

        struct sockaddr_in sockaddr;
        memset(&sockaddr, 0, sizeof(sockaddr));
        sockaddr.sin_family = AF_INET;
        sockaddr.sin_port = htons(port);
        memcpy(&sockaddr.sin_addr, server->h_addr, server->h_length);

        if (connect(socketfd, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) < 0) {
            fprintf(stderr,"ERRO conectando ao servidor\n");
            close(socketfd);
            socketfd = -1;
            sleep(1);
            continue;
        }

        printf("[Backup %d Connection Thread] Successfully connected to %s:%d (socket fd: %d).\n", id, hostname, port, socketfd);

        char *id_string = malloc(2);
        sprintf(id_string, "%d", id);
        Packet packet = create_control_packet(PACKET_REPLICA_MSG, strlen(id_string), id_string);
        send_packet(socketfd, &packet);

        while (1) {
            pthread_mutex_lock(&mode_change_mutex);
            int comm_should_exit = atomic_load(&global_shutdown_flag) || (atomic_load(&global_server_mode) != BACKUP);
            pthread_mutex_unlock(&mode_change_mutex);

            if (comm_should_exit) {
                printf("[Backup %d Connection Thread] Communication loop exiting due to mode change or shutdown.\n", id);
                break; // Exit inner communication loop
            }

            if (has_data(socketfd, 1000) > 0) {
                Packet packet = read_packet(socketfd);
                switch (packet.type) {
                    case PACKET_REPLICA_MSG:{
                        ReplicaEvent event = deserialize_replica_event(packet._payload);
                        switch (event.type) {
                            case EVENT_CLIENT_CONNECTED:
                                initialize_user_session_and_threads(event.device_address, -1, -1, -1, event.username);
                                break;
                            case EVENT_CLIENT_DISCONNECTED:{
                                UserContext *context = get_or_create_context(&contextTable, event.username);
                                pthread_mutex_lock(&context->lock);
                                Session *session = get_user_session_by_address(context, &event.device_address);
                                session->user_context->sessions[session->session_index] = NULL;
                                pthread_mutex_unlock(&context->lock);
                                fprintf(stderr, "Usuário %s desconectou.\n", event.username);
                            }
                            
                                break;
                            case EVENT_FILE_UPLOADED:{
                                UserContext *context = get_or_create_context(&contextTable, event.username);
                                pthread_mutex_lock(&context->lock);
                                Session *session = get_user_session_by_address(context, &event.device_address);
                                pthread_mutex_unlock(&context->lock);
                                char *user_folder_path = get_user_folder(event.username);
                                handle_incoming_file(session, socketfd, user_folder_path);
                                free(user_folder_path);
                                break;
                            }
                                
                            case EVENT_REPLICA_ADDED:{
                                int replica_id = event.username[0];
                                if (replica_id != id)
                                    add_replica(-1, replica_id, event.device_address);
                                break;
                            }
                            case EVENT_HEARTBEAT:
                                heartbeat_received();
                                break;
                            
                            case EVENT_ELECTION: {
                                int sender_id = atoi(event.username);
                                fprintf(stderr, "[Servidor %d] Recebeu ELECTION de %d.\n", id, sender_id);
                                if (sender_id < id) {
                                    ReplicaEvent answer_event;
                                    create_election_answer_event(&answer_event, id);
                                    send_event(&answer_event, socketfd);
                                    free_event(&answer_event);
                                    
                                    // Inicia sua própria eleição em uma nova thread
                                    pthread_t election_thread;
                                    int* my_id_ptr = malloc(sizeof(int));
                                    *my_id_ptr = id;
                                    pthread_create(&election_thread, NULL, (void* (*)(void*))run_election, my_id_ptr);
                                    pthread_detach(election_thread);
                                }
                                break;
                            }

                            case EVENT_ELECTION_ANSWER: {
                                int sender_id = atoi(event.username);
                                fprintf(stderr, "[Servidor %d] Recebeu ANSWER de %d.\n", id, sender_id);
                                set_election_answer_received(1);
                                break;
                            }

                            case EVENT_COORDINATOR: {
                                int leader_id = atoi(event.username);
                                char leader_ip[INET_ADDRSTRLEN];
                                inet_ntop(AF_INET, &(event.device_address.sin_addr), leader_ip, INET_ADDRSTRLEN);
                                int leader_port = ntohs(event.device_address.sin_port);

                                fprintf(stderr, "[Servidor %d] Anúncio recebido: Novo líder é %d em %s:%d.\n", id, leader_id, leader_ip, leader_port);

                                pthread_mutex_lock(&mode_change_mutex);
                                // Atualiza os dados de conexão para o novo líder
                                strncpy(backup_args->hostname, leader_ip, sizeof(backup_args->hostname) - 1);
                                backup_args->hostname[sizeof(backup_args->hostname) - 1] = '\0';
                                backup_args->port = leader_port;
                                pthread_mutex_unlock(&mode_change_mutex);

                                close(socketfd);
                                break;
                            }
                        }
                        break;
                    }
                    case PACKET_CONNECTION_CLOSED:{
                        fprintf(stderr, "[Backup %d Connection Thread] Connection closed.\n",id);
                        break;
                    }
                        
                    default :{
                        fprintf(stderr, "[Backup %d Connection Thread] Unsupported packet type: %d\n",id, packet.type);
                        break;
                    }
                }
                
            }
            sleep(1);
        }
        printf("[Backup %d Connection Thread] Disconnected from manager or communication loop exited. Closing socket %d.\n", id, socketfd);
        
    }
    close(socketfd);

    printf("[Backup %d Connection Thread] Thread fully exiting.\n", id);
    pthread_exit(NULL);
}

void *manage_replicas(void *args) {
    ManagerArgs *manager_args = (ManagerArgs *) args;
    printf("Server ID %d: Running as MANAGER.\n", manager_args->id);
    atomic_store(&global_server_mode,BACKUP_MANAGER);
    last_heartbeat = time(NULL);
    
    pthread_t listener_tid;

    int rc = pthread_create(&listener_tid, NULL, replica_listener_thread, manager_args);
    if (rc != 0) {
        fprintf(stderr, "Error launching replica listener thread: %s\n", strerror(rc));
        free(manager_args);
        exit(EXIT_FAILURE);
    }
    pthread_detach(listener_tid);

    printf("Manager server (ID %d) is performing its main duties...\n", manager_args->id);
    while (1) {
        pthread_mutex_lock(&mode_change_mutex);
        int should_exit_role = atomic_load(&global_shutdown_flag) || (atomic_load(&global_server_mode) != BACKUP_MANAGER);
        pthread_mutex_unlock(&mode_change_mutex);

        if (should_exit_role) {
            printf("[Manager %d] Exiting MANAGER role loop.\n", manager_args->id);
            break;
        }
        send_heartbeat_to_replicas();
        sleep(2);
    }
    printf("Manager server (ID %d) is cleaning up resources.\n", manager_args->id);
    pthread_exit(NULL);
}

void *run_as_backup(void* arg) {
    BackupArgs *backup_args = (BackupArgs *) arg;

    printf("Server ID %d: Running as BACKUP.\n", backup_args->id);
    printf("Manager to connect to: %s:%d\n", backup_args->hostname, backup_args->port);
    atomic_store(&global_server_mode,BACKUP);

    last_heartbeat = time(NULL);

    ManagerArgs *listener_args = malloc(sizeof(ManagerArgs));
    listener_args->id = backup_args->id;
    listener_args->port = backup_args->my_base_port + 3;
    
    pthread_t listener_tid;
    if (pthread_create(&listener_tid, NULL, replica_listener_thread, listener_args) != 0) {
        perror("Backup failed to create replica listener thread");
        exit(EXIT_FAILURE);
    }
    pthread_detach(listener_tid);
    printf("[Servidor %d] Backup agora está escutando por conexões de réplicas na porta %d.\n", listener_args->id, listener_args->port);

    pthread_t manager_conn_tid;

    int rc = pthread_create(&manager_conn_tid, NULL, connect_to_server_thread, backup_args);
    if (rc != 0) {
        fprintf(stderr, "Error launching manager connection thread: %s\n", strerror(rc));
        exit(EXIT_FAILURE);
    }
    pthread_detach(manager_conn_tid); // Detach the connection thread

    // Launch heartbeat monitor thread (if not already running)
    pthread_t heartbeat_tid;
    int *backup_id_ptr = malloc(sizeof(int));
    if (backup_id_ptr == NULL) {
        perror("Failed to allocate ID for heartbeat thread");
        exit(EXIT_FAILURE);
    }
    *backup_id_ptr = backup_args->id;

    rc = pthread_create(&heartbeat_tid, NULL, heartbeat_monitor_thread_main, backup_id_ptr);
    if (rc != 0) {
        fprintf(stderr, "Error launching heartbeat monitor thread: %s\n", strerror(rc));
        free(backup_id_ptr);
        exit(EXIT_FAILURE);
    }
    pthread_detach(heartbeat_tid);

    printf("Backup server (ID %d) is performing its main duties...\n", backup_args->id);
    while (1) {
        pthread_mutex_lock(&mode_change_mutex);
        int should_exit_role = atomic_load(&global_shutdown_flag) || (atomic_load(&global_server_mode) != BACKUP);
        pthread_mutex_unlock(&mode_change_mutex);

        if (should_exit_role) {
            printf("[Backup %d] Exiting BACKUP role loop.\n", backup_args->id);
            break;
        }
        sleep(1);
    }
    printf("Backup server (ID %d) is cleaning up resources.\n", backup_args->id);
    

    if (atomic_load(&global_server_mode) == BACKUP_MANAGER) {
        ManagerArgs *args = (ManagerArgs *)malloc(sizeof(ManagerArgs));
        args->id = backup_args->id;
        args->port = backup_args->my_base_port + 10;

        struct sockaddr_in my_new_manager_address;
        memset(&my_new_manager_address, 0, sizeof(my_new_manager_address));
        my_new_manager_address.sin_family = AF_INET;
        my_new_manager_address.sin_port = htons(args->port);

        if (inet_pton(AF_INET, my_server_ip, &my_new_manager_address.sin_addr) <= 0) {
            perror("inet_pton falhou ao construir o endereço do novo coordenador");
        }

        ReplicaEvent coord_event;
        create_coordinator_event(&coord_event, args->id, my_new_manager_address);
        notify_replicas(&coord_event);
        free_event(&coord_event);

        // precisa notificar o front-end

        pthread_t manager_thread;
        pthread_create(&manager_thread, NULL, manage_replicas, (void *) args);
        pthread_detach(manager_thread);
    }

    free(backup_args);
    pthread_exit(NULL);
}
