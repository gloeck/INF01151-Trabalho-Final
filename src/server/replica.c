#include "./replica.h"
#include <stdio.h>
#include <netinet/in.h>
#include <stdio.h>

int current_manager = -1;

typedef struct ReplicaNode {
    int id;
    int socketfd;
    struct sockaddr_in device_address;
    struct ReplicaNode *next;
} ReplicaNode;

static ReplicaNode *head = NULL;
static pthread_mutex_t replica_list_mutex;

extern ReplicaNode *head; 
extern pthread_mutex_t replica_list_mutex;

static ReplicaNode *create_new_node(int socketfd, int id, struct sockaddr_in device_address) {
    ReplicaNode *new_node = (ReplicaNode *)malloc(sizeof(ReplicaNode));
    if (new_node == NULL) {
        perror("Failed to allocate memory for new ReplicaNode");
        return NULL;
    }
    new_node->id = id;
    new_node->socketfd = socketfd;
    new_node->device_address = device_address;
    new_node->next = NULL;
    return new_node;
}

static ReplicaNode *find_node(int socketfd) {
    ReplicaNode *current = head;
    while (current != NULL) {
        if (current->socketfd == socketfd) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}


void init_replica_list_mutex() {
    pthread_mutex_init(&replica_list_mutex, NULL);
}

void destroy_replica_list_mutex() {
    pthread_mutex_destroy(&replica_list_mutex);
}

int add_replica(int socketfd, int id, struct sockaddr_in device_address) {
    pthread_mutex_lock(&replica_list_mutex);
    if (find_node(socketfd) != NULL) {
        fprintf(stderr, "Warning: Replica with socketfd %d already exists in the list.\n", socketfd);
        pthread_mutex_unlock(&replica_list_mutex);
        return 0;
    }

    ReplicaNode *newNode = create_new_node(socketfd, id, device_address);
    if (newNode == NULL) {
        pthread_mutex_unlock(&replica_list_mutex); 
        return 0;
    }
    newNode->next = head;
    head = newNode;
    pthread_mutex_unlock(&replica_list_mutex);
    fprintf(stderr, "Replica %d successfully added to list\n", id);
    return 1;
}

int replica_list_contains(int socketfd) {
    pthread_mutex_lock(&replica_list_mutex);
    int contains = find_node(socketfd) != NULL;
    pthread_mutex_unlock(&replica_list_mutex);
    return contains;
}

ReplicaNode* replica_list_remove_node(ReplicaNode **head_ptr, ReplicaNode *prev, ReplicaNode *node_to_remove) {
    if (node_to_remove == NULL) {
        return NULL;
    }

    if (prev == NULL) {
        *head_ptr = node_to_remove->next;
    } else {
        prev->next = node_to_remove->next;
    }

    close(node_to_remove->socketfd); 
    free(node_to_remove);

    return (prev == NULL) ? *head_ptr : prev->next;
}

void replica_list_print_all() {
    pthread_mutex_lock(&replica_list_mutex); 
    printf("Replica List: [");
    ReplicaNode *current = head;
    while (current != NULL) {
        printf("%d - %d", current->id, current->socketfd);
        if (current->next != NULL) {
            printf(", ");
        }
        current = current->next;
    }
    printf("]\n");
    pthread_mutex_unlock(&replica_list_mutex); 
}

void replica_list_destroy() {
    pthread_mutex_lock(&replica_list_mutex); 
    ReplicaNode *current = head;
    ReplicaNode *next;
    while (current != NULL) {
        next = current->next;
        free(current);
        current = next;
    }
    head = NULL;
    printf("Replica list destroyed.\n");
    pthread_mutex_unlock(&replica_list_mutex); 
}

int send_event(ReplicaEvent* event, int socketfd){
    char *serialized_event = serialize_replica_event(event);

    Packet packet = create_control_packet(PACKET_REPLICA_MSG, strlen(serialized_event), serialized_event);
    int return_code = send_packet(socketfd, &packet);

    if (event->type == EVENT_FILE_UPLOADED)
    {
        send_file(socketfd, event->filepath);
    }
        
    free(serialized_event);
    return return_code;
}

int notify_replicas(ReplicaEvent* event){
    int notified_replicas = 0;
    pthread_mutex_lock(&replica_list_mutex);
    ReplicaNode *current = head;
    ReplicaNode *prev = NULL;
     while (current != NULL) {
        int return_code = send_event(event, current->socketfd);
        if (return_code == OK){
            notified_replicas++;
            prev = current;
            current = current->next;
        }else {
            fprintf(stderr, "Error sending packet to replica %d (socketfd: %d).\n", current->id, current->socketfd);
            if (return_code == SOCKET_CLOSED) {
                fprintf(stderr, "Replica %d closed the connection.\n", current->id);
                current = replica_list_remove_node(&head, prev, current);
            } else {
                prev = current;
                current = current->next;
            }
        }
        
    }
    pthread_mutex_unlock(&replica_list_mutex); 

    if (event->type != EVENT_HEARTBEAT){
        fprintf(stderr, "%d replicas notified\n", notified_replicas);
    }
    
    return notified_replicas;
}


void free_event(ReplicaEvent* event){
    if (event == NULL) return; 

    if (event->username != NULL) {
        free(event->username);
        event->username = NULL;
    }

    if (event->filepath != NULL) {
        free(event->filepath);
        event->filepath = NULL;
    }
}


ReplicaEvent *create_client_connected_event(ReplicaEvent *event, char *username, struct sockaddr_in device_address){
    event->type = EVENT_CLIENT_CONNECTED;
    event->username = strdup(username);
    event->device_address = device_address;
    event->filepath = NULL;

    return event;
}

ReplicaEvent *create_client_disconnected_event(ReplicaEvent *event, char *username, struct sockaddr_in device_address){
    event->type = EVENT_CLIENT_DISCONNECTED;
    event->username = strdup(username);
    event->device_address = device_address;
    event->filepath = NULL;

    return event;
}

ReplicaEvent *create_replica_added_event(ReplicaEvent *event, int id, struct sockaddr_in device_address){
    event->type = EVENT_REPLICA_ADDED;

    event->username = malloc(sizeof(int) + 1);
    event->username[0] = id;
    event->username[1] = '\0';

    event->device_address = device_address;
    event->filepath = NULL;

    return event;
}



ReplicaEvent *create_file_upload_event(ReplicaEvent *event, char *username, struct sockaddr_in device_address, char *filepath){
    event->type = EVENT_FILE_UPLOADED;
    event->username = strdup(username);
    event->device_address = device_address;
    event->filepath = strdup(filepath);

    return event;
}

ReplicaEvent *create_heartbeat_event(ReplicaEvent *event){
    event->type = EVENT_HEARTBEAT;
    event->username = NULL;
    event->filepath = NULL;
     
    struct sockaddr_in empty_struct = {0};
    event->device_address = empty_struct;
    

    return event;
}

char* serialize_replica_event(const ReplicaEvent *event) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(event->device_address.sin_addr), ip, INET_ADDRSTRLEN);
    int port = ntohs(event->device_address.sin_port);

    const char *username_str = (event->username != NULL) ? event->username : "";
    const char *filepath_str = (event->filepath != NULL) ? event->filepath : "";

    size_t needed = snprintf(NULL, 0, "%d|%s:%d|%s|%s",
                             event->type, ip, port, username_str, filepath_str);

    char *result = malloc(needed + 1);
    if (!result) return NULL;

    sprintf(result, "%d|%s:%d|%s|%s",
            event->type, ip, port, username_str, filepath_str);

    return result;
}


ReplicaEvent deserialize_replica_event(const char *str) {
    ReplicaEvent event;
    memset(&event, 0, sizeof(ReplicaEvent)); 

    char *copy = strdup(str);

    char *token;
    char *saveptr; 
    
    // 1. Event type
    token = strtok_r(copy, "|", &saveptr);
    if (token) {
        event.type = (enum EventTypes) atoi(token);
    } else {
        free(copy);
        return event; 
    }

    // 2. Device Address (IP:Port)
    token = strtok_r(NULL, "|", &saveptr);
    if (token) {
        char *colon = strchr(token, ':');
        if (colon) {
            *colon = '\0';
            char *ip = token;
            int port = atoi(colon + 1);

            event.device_address.sin_family = AF_INET;
            inet_pton(AF_INET, ip, &(event.device_address.sin_addr));
            event.device_address.sin_port = htons(port);
        } 
    } else {
        free(copy);
        return event;
    }

    // 3. Username
    token = strtok_r(NULL, "|", &saveptr);
    if (token) {
        if (strcmp(token, "") != 0) {
            event.username = strdup(token);
        } else {
            event.username = NULL; 
        }
    } else {
        event.username = NULL;
    }

    // 4. Filepath
    token = strtok_r(NULL, "|", &saveptr);
    if (token) {
        if (strcmp(token, "") != 0) {
            event.filepath = strdup(token);
        } else {
            event.filepath = NULL;
        }
    } else {
        event.filepath = NULL;
    }

    free(copy); 
    return event;
}

ReplicaEvent *create_election_event(ReplicaEvent *event, int sender_id) {
    event->type = EVENT_ELECTION;
    event->username = malloc(16);
    sprintf(event->username, "%d", sender_id);
    event->filepath = NULL;
    memset(&event->device_address, 0, sizeof(event->device_address));
    return event;
}

ReplicaEvent *create_election_answer_event(ReplicaEvent *event, int sender_id) {
    event->type = EVENT_ELECTION_ANSWER;
    event->username = malloc(16);
    sprintf(event->username, "%d", sender_id);
    event->filepath = NULL;
    memset(&event->device_address, 0, sizeof(event->device_address));
    return event;
}

ReplicaEvent *create_coordinator_event(ReplicaEvent *event, int leader_id, struct sockaddr_in leader_address) {
    event->type = EVENT_COORDINATOR;
    event->username = malloc(16);
    sprintf(event->username, "%d", leader_id);
    event->device_address = leader_address;
    event->filepath = NULL;
    return event;
}

ReplicaNode* get_replica_list_head() {
    return head;
}
