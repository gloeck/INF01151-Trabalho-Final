#include "./serverCommon.h"
#include "./serverRoles.h"
#include "replica.h"
#include <stdio.h>

const int ANSWER_OK = 1;

enum FileStatus { FILE_STATUS_NOT_FOUND, FILE_STATUS_UPDATED, FILE_STATUS_EXISTS };

HashTable contextTable;

char manager_ip[256] = "";
int manager_port = -1;
int id = -1;
char my_server_ip[256] = "127.0.0.1";

void perror_exit(const char *msg); // Escreve a mensagem de erro e termina o programa com falha
void *interface(void* arg); // Recebe e executa os comandos do usuário
void *send_f(void* arg); // Envia os arquivos para o cliente
void *receive(void* arg); // Recebe os arquivos do cliente
void *replication_management(void *args);
void termination(int sig);

// Variáveis definidas globalmente para poderem ser fechadas na função de terminação
// -1 sempre é um fd inválido, e pode ser fechado sem problemas
int sock_interface_listen = -1, sock_send_listen = -1, sock_receive_listen = -1;

int create_folder_if_not_exists(const char *path, const char *folder_name) {
    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s/%s", path, folder_name);

    struct stat st;
    if (stat(full_path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return 0;
        } else {
            fprintf(stderr, "Error: %s exists but is not a directory\n", full_path);
            return -1;
        }
    }

    if (mkdir(full_path, 0755) != 0) {
        perror("mkdir failed");
        return -1;
    }

    return 0;
}


void read_username(char *username, int sock_interface){
	int request_size = read(sock_interface, username, MAX_USERNAME_LENGTH);
	if (request_size < 0) 
		perror_exit("ERRO lendo o pedido de coneccao do usuário: ");
	username[request_size] = '\0';
	fflush(stdout);
}

void initialize_user_session_and_threads(struct sockaddr_in device_address, int sock_interface, int sock_receive, int sock_send, char *username) {
	UserContext *context = get_or_create_context(&contextTable, strdup(username));

	pthread_mutex_lock(&context->lock);
	int free_session_index = find_free_session_index(context);
	if (free_session_index == -1) {
		fprintf(stderr, "Maximo de sessoes atingidas para o user %s!\n", username);
		pthread_mutex_unlock(&context->lock);
		return;
	}

	printf("Usuario %s conectado, sessão %d\n", username,free_session_index);

	SessionSockets session_sockets = { sock_interface, sock_receive, sock_send };
	Session *user_session = create_session(free_session_index, context, session_sockets, device_address);

	if (atomic_load(&global_server_mode) == BACKUP_MANAGER)
	{

		ReplicaEvent event;
		create_client_connected_event(&event , username, device_address);
		notify_replicas(&event);
		free_event(&event);


		pthread_create(&user_session->threads.interface_thread, NULL, interface, user_session);
		pthread_create(&user_session->threads.send_thread, NULL, send_f, user_session);
		pthread_create(&user_session->threads.receive_thread, NULL, receive, user_session);
	}


	context->sessions[free_session_index] = user_session; 
	pthread_mutex_unlock(&context->lock);

	
	HashTable_print(&contextTable);
}


void handle_new_connection(int sock_interface){
	    // Verifica se a conecção é válida e responde para o cliente
	    char username[MAX_USERNAME_LENGTH + 1];
		read_username(username, sock_interface);

		// Comunica pra o cliente fazer a conecção
		if (write(sock_interface, &ANSWER_OK, sizeof(ANSWER_OK)) != sizeof(ANSWER_OK))
			perror_exit("ERRO respondendo para o cliente: ");

		// Cria os socket de transferência
	    int sock_send, sock_receive;
		struct sockaddr_in cli_send_addr, cli_receive_addr;
    	socklen_t cli_send_addr_len = sizeof(struct sockaddr_in);
		socklen_t cli_receive_addr_len = sizeof(struct sockaddr_in);
		
		// Aceita as conecções
		listen(sock_receive_listen,5);
		if ((sock_receive = accept(sock_receive_listen, (struct sockaddr *) &cli_receive_addr, &cli_receive_addr_len)) == -1) 
		    perror_exit("ERRO aceitando a coneccao de receive: ");

		listen(sock_send_listen,5);
		if ((sock_send = accept(sock_send_listen, (struct sockaddr *) &cli_send_addr, &cli_send_addr_len)) == -1) 
		    perror_exit("ERRO aceitando a coneccao de send: ");
		

		initialize_user_session_and_threads(cli_send_addr, sock_interface, sock_receive, sock_send, username);
}

void parse_server_arguments(int argc, char *argv[]) {
    if (argc < 4) { 
        fprintf(stderr, "Uso: ./server ID MEU_IP -MODO [ip_manager porta_manager]\n");
		fprintf(stderr, "ID = numero natural\n");
        fprintf(stderr, "MEU_IP = O endereço IP desta máquina\n");
        fprintf(stderr, "MODO = M ou B (manager ou backup, respectivamente)\n");
        fprintf(stderr, "ip_manager e porta_manager desnecessario se MODO = M\n");
        exit(1);
    }

    id = atoi(argv[1]);
    if (id <= 0) { 
        fprintf(stderr, "ID do servidor inválido: %s (deve ser um número inteiro positivo)\n", argv[1]);
        exit(1);
    }

	strncpy(my_server_ip, argv[2], sizeof(my_server_ip) - 1);
    my_server_ip[sizeof(my_server_ip) - 1] = '\0';

    if (strcmp(argv[3], "-M") == 0) {
        global_server_mode = BACKUP_MANAGER;
    } else if (strcmp(argv[3], "-B") == 0) {
        global_server_mode = BACKUP;

        if (argc < 6) { 
            fprintf(stderr, "Necessário especificar ip e porta quando MODO = B\n");
            exit(1);
		}

        strncpy(manager_ip, argv[4], 255);
        manager_ip[255] = '\0'; 

        manager_port = atoi(argv[5]);
        if (manager_port <= 0) { 
            fprintf(stderr, "Porta inválida: %s\n", argv[4]);
            exit(1);
        }

    } else {
        fprintf(stderr, "Modo inválido: %s\n", argv[3]);
        fprintf(stderr, "Uso: ./server ID -MODO [ip porta]\n");
		fprintf(stderr, "ID = numero natural\n");
        fprintf(stderr, "MODO = M ou B (manager ou backup, respectivamente)\n");
        fprintf(stderr, "ip e porta desnecessario se MODO = M\n");
        exit(1);
    }
}


int main(int argc, char* argv[]) {

	parse_server_arguments(argc, argv);

	

	// Define a função de terminação do programa
	signal(SIGINT, termination);

	contextTable = HashTable_create(30); 
            

	if(create_folder_if_not_exists("./", USER_FILES_FOLDER) != 0){
		fprintf(stderr, "Failed to create \"user files\" folder");
	}

    // Cria os sockets de espera de conecção
	if ((sock_interface_listen = socket(AF_INET, SOCK_STREAM, 0)) == -1) 
        perror_exit("ERRO abrindo o socket de espera de coneccoes de interface: ");

	if ((sock_send_listen = socket(AF_INET, SOCK_STREAM, 0)) == -1) 
        perror_exit("ERRO abrindo o socket de espera por coneccao de send: ");
		
	if ((sock_receive_listen = socket(AF_INET, SOCK_STREAM, 0)) == -1) 
        perror_exit("ERRO abrindo o socket de espera por coneccao de receive: ");


    // Faz bind nos ports
	struct sockaddr_in interface_serv_addr, send_serv_addr, receive_serv_addr;

	uint16_t interface_socket_port = 4000;
	

    interface_serv_addr.sin_family = AF_INET;
	interface_serv_addr.sin_port = htons(interface_socket_port);
	interface_serv_addr.sin_addr.s_addr = INADDR_ANY;
	bzero(&(interface_serv_addr.sin_zero), 8);

   
	srand((unsigned) time(NULL));
	while (bind(sock_interface_listen, (struct sockaddr *) &interface_serv_addr, sizeof(interface_serv_addr)) < 0){
        interface_socket_port = (rand() % 30000) + 2000;
        interface_serv_addr.sin_port = htons(interface_socket_port);
    } ;

	fprintf(stderr, "Server running on port %d\n", interface_socket_port);

	
	uint16_t receive_socket_port = interface_socket_port + 1;
	uint16_t send_socket_port = interface_socket_port + 2;
	uint16_t replica_socket_port = interface_socket_port + 3;


	pthread_t replication_thread;
	if (global_server_mode == BACKUP_MANAGER)
	{
		ManagerArgs *args = (ManagerArgs *)malloc(sizeof(ManagerArgs));
		args->id = id;
		args->port = replica_socket_port;

		pthread_create(&replication_thread, NULL, manage_replicas, (void*) args);
		pthread_detach(replication_thread);
	}else if (global_server_mode == BACKUP)
	{
		BackupArgs *args = (BackupArgs *)malloc(sizeof(BackupArgs)); 
		args->id = id;
		strncpy(args->hostname, manager_ip, sizeof(args->hostname) - 1);
		args->hostname[sizeof(args->hostname) - 1] = '\0';
		args->port = manager_port;
		args->my_base_port = interface_socket_port;

		pthread_create(&replication_thread, NULL, run_as_backup, (void*) args);
		pthread_detach(replication_thread);
	}else
	{
		fprintf(stderr, "Unknown server mode.\n");
		exit(1);
	}

	receive_serv_addr.sin_family = AF_INET;
    receive_serv_addr.sin_port = htons(receive_socket_port);
    receive_serv_addr.sin_addr.s_addr = INADDR_ANY;
    bzero(&(receive_serv_addr.sin_zero), 8);

	send_serv_addr.sin_family = AF_INET;
    send_serv_addr.sin_port = htons(send_socket_port);
    send_serv_addr.sin_addr.s_addr = INADDR_ANY;
    bzero(&(send_serv_addr.sin_zero), 8);

	if (bind(sock_receive_listen, (struct sockaddr *) &receive_serv_addr, sizeof(receive_serv_addr)) < 0){ 
			perror_exit("ERRO vinulando o socket de espera da coneccao de receive: ");
	}

	if (bind(sock_send_listen, (struct sockaddr *) &send_serv_addr, sizeof(send_serv_addr)) < 0){ 
		perror_exit("ERRO vinulando o socket de espera da coneccao de send: ");
	}
	
	// Começa a esperar pedidos de conecção
	listen(sock_interface_listen, 5);
	listen(sock_receive_listen, 1);
	listen(sock_send_listen, 1);
	

    while (1) {
        // Espera uma requisição de conecção
        int sock_interface;
        struct sockaddr_in cli_addr;
	    socklen_t clilen;

        clilen = sizeof(struct sockaddr_in);
	    if ((sock_interface = accept(sock_interface_listen, (struct sockaddr *) &cli_addr, &clilen)) == -1) 
		    perror_exit("ERRO ao aceitar coneccoes da interface: ");

		handle_new_connection(sock_interface);
	}
			

    return 0;
}


void perror_exit(const char *msg) {
	perror(msg);
	exit(EXIT_FAILURE);
}

int receive_command(int socketfd){
	Packet packet = read_packet(socketfd);
	return packet.type;
}

char *get_user_folder(const char *username){
    size_t len = strlen(USER_FILES_FOLDER) + strlen(username) + 2;

    char *path = malloc(len);
    if (path == NULL) {
        perror("malloc");
        return NULL;
    }

    snprintf(path, len, "%s/%s", USER_FILES_FOLDER, username);

	return path;
}


char* list_files(const char *folder_path) {
    struct dirent *entry;
    DIR *dir = opendir(folder_path);
    if (dir == NULL) {
        perror("opendir");
        return NULL;
    }

    size_t buffer_size = 4096;
    char *result = malloc(buffer_size);
    if (result == NULL) {
        perror("malloc");
        closedir(dir);
        return NULL;
    }
    result[0] = '\0';

    char path[PATH_MAX];
    struct stat info;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' &&
           (entry->d_name[1] == '\0' ||
           (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", folder_path, entry->d_name);
        if (stat(path, &info) != 0) {
            continue;
        }

        char mod_time[20], acc_time[20], chg_time[20];
        strftime(mod_time, sizeof(mod_time), "%d/%m/%Y %H:%M:%S", localtime(&info.st_mtime));
        strftime(acc_time, sizeof(acc_time), "%d/%m/%Y %H:%M:%S", localtime(&info.st_atime));
        strftime(chg_time, sizeof(chg_time), "%d/%m/%Y %H:%M:%S", localtime(&info.st_ctime));

        size_t needed = strlen(result) + strlen(entry->d_name) + strlen(mod_time)
                      + strlen(acc_time) + strlen(chg_time) + 200;
        if (needed > buffer_size) {
            while (needed > buffer_size) {
                buffer_size *= 2;
            }
            char *new_result = realloc(result, buffer_size);
            if (new_result == NULL) {
                perror("realloc");
                free(result);
                closedir(dir);
                return NULL;
            }
            result = new_result;
        }

        strcat(result, "\nFile: ");
        strcat(result, entry->d_name);
        strcat(result, "\n    Modification time: ");
        strcat(result, mod_time);
        strcat(result, "\n          Access time: ");
        strcat(result, acc_time);
        strcat(result, "\n Change/creation time: ");
        strcat(result, chg_time);
        strcat(result, "\n\n");
    }

    closedir(dir);
    return result;
}

void signal_shutdown(Session *session) {
    pthread_mutex_lock(&session->sync_buffer.lock);
    pthread_cond_broadcast(&session->sync_buffer.not_empty);
    pthread_cond_broadcast(&session->sync_buffer.not_full);
    pthread_mutex_unlock(&session->sync_buffer.lock);
}


// Recebe e executa os comandos do usuário
void *interface(void* arg) {
	Session *session = (Session *) arg;
	int interface_socket = session->sockets.interface_socketfd;
	int send_socketfd = session->sockets.send_socketfd;
	int receive_socketfd = session->sockets.receive_socketfd;

	char *folder_path = get_user_folder(session->user_context->username);

	while (session->active)
	{
		int command = receive_command(interface_socket);

		//fprintf(stderr, "Command: %d\n", command);

		switch (command)
		{
		case PACKET_LIST:{
			char *files = list_files(folder_path);

			int seqn = 0;
			int total_packets = 1;
			Packet packet = create_data_packet(seqn,total_packets,strlen(files),files);

			if(send_packet(interface_socket,&packet) != OK){
				fprintf(stderr, "ERROR responding to list_server");
				free(folder_path);
			}

			
			free(files);
		}
			
			
			break;

		case PACKET_DOWNLOAD: {
			//printf("DOWNLOAD requisitado\n");

    		Packet packet = read_packet(interface_socket);
    		if (packet.length == 0 || !packet._payload) {
        		fprintf(stderr, "ERROR invalid file name.\n");
        		break;
    		}

    		printf("Requested file: '%.*s'\n", packet.length, packet._payload);


    		char filepath[512];
    		snprintf(filepath, sizeof(filepath), "%s/%.*s", folder_path, packet.length, packet._payload);

    		if (access(filepath, F_OK) != 0) {
        		fprintf(stderr, "ERROR file not found: %s\n", filepath);
        		break;
    		}

    		printf("Sending file: %s\n", filepath);
    		send_file(interface_socket, filepath);

    		break;
		}

		case PACKET_DELETE: {
    		//printf("DELETE requisitado\n");
			
    		Packet packet = read_packet(interface_socket);
    		if (packet.length == 0 || !packet._payload) {
        		fprintf(stderr, "ERROR invalid file name\n");
        		break;
    		}

    		char filepath[512];
    		snprintf(filepath, sizeof(filepath), "%s/%.*s", folder_path, packet.length, packet._payload);

    		if (access(filepath, F_OK) != 0) {
        		fprintf(stderr, "ERROR file not found: %s\n", filepath);
    		} else if (remove(filepath) == 0) {
        		printf("Arquivo excluído: %s\n", filepath);
    		} else {
        		perror("[Servidor] ERROR deleting file");
    		}

    		break;
		}

		case PACKET_EXIT:{
			//fprintf(stderr, "iniciando exit para a sessao %d\n",session->session_index);
			session->active = 0;

			signal_shutdown(session);
			
			close(receive_socketfd);
			//fprintf(stderr, "fechou a socket the receive\n");
			close(interface_socket);
			//fprintf(stderr, "fechou a socket the interface\n");
			close(send_socketfd);
			//fprintf(stderr, "fechou a socket the send\n");

			
			//fprintf(stderr, "esperando thread: %lu\n", (unsigned long) session->threads.receive_thread);
			pthread_join(session->threads.receive_thread, NULL); 
			//fprintf(stderr, "receive_thread exited\n");
			pthread_join(session->threads.send_thread, NULL); 
			//fprintf(stderr, "send_thread exited\n");

			pthread_mutex_lock(&session->user_context->lock);
			session->user_context->sessions[session->session_index] = NULL;

			ReplicaEvent event;
			create_client_disconnected_event(&event, session->user_context->username, session->device_address);

    		pthread_mutex_unlock(&session->user_context->lock);

			notify_replicas(&event);
			free_event(&event);
			break;
		}
		
		default:
			break;
		}
	}
	free(folder_path);

	fprintf(stderr, "Sessao %d desconectada\n", session->session_index);
	free(arg);
	pthread_exit(NULL);
}

// Envia os arquivos para o cliente
void *send_f(void* arg) {
	Session *session = (Session *) arg;
	int send_socket = session->sockets.send_socketfd;

	while (session->active)
	{
		FileEntry file_entry = get_next_file_to_sync(session);
		if(!file_entry.valid){
			continue;
		}
		send_file(send_socket, file_entry.filename);
    	free_file_entry(file_entry); 
	}
		
	pthread_exit(NULL);
}

int get_file_status(FileNode *list, const char *filepath) {
	if (!list){
		return FILE_STATUS_NOT_FOUND;
	}

	FileNode *file_node = FileLinkedList_get(list, filepath);

	if (!file_node){
		return FILE_STATUS_NOT_FOUND;
	}

	uint32_t old_crc = file_node->crc;
    uint32_t new_crc = crc32(filepath);

	if (new_crc != old_crc)
	{
		return FILE_STATUS_UPDATED;
	}

    return FILE_STATUS_EXISTS;
}

void handle_incoming_file(Session *session, int receive_socket, const char *folder_path) {
    create_folder_if_not_exists(USER_FILES_FOLDER,session->user_context->username);
	char *filepath = read_file_from_socket(receive_socket, folder_path);
	
	if(session->active ){
		fprintf(stderr, "File received.\n");
		switch (get_file_status(session->user_context->file_list, filepath)) {
			case FILE_STATUS_NOT_FOUND:
				if (add_file_to_context(&contextTable,filepath,session->user_context->username) != 0){
					fprintf(stderr, "ERROR adicionando arquivo ao contexto");
				}
				break;
			case FILE_STATUS_UPDATED:{
				FileNode *file_node = FileLinkedList_get(session->user_context->file_list, filepath);
				file_node->crc = crc32(filepath);
			}
		}	

		if (atomic_load(&global_server_mode) == BACKUP_MANAGER)
		{
			int send_to_index = !session->session_index; //session_index poder ser só 1 ou 0
			send_file_to_session(send_to_index, session->user_context, filepath);
		}
	}

	if(atomic_load(&global_server_mode) == BACKUP_MANAGER && filepath != NULL){
		ReplicaEvent event;
		create_file_upload_event(&event, session->user_context->username, session->device_address, filepath);
		notify_replicas(&event);
		free_event(&event);
		
	}

	free(filepath);
}

// Recebe os arquivos do cliente
void *receive(void* arg) {
	Session *session = (Session *) arg;
	int receive_socket = session->sockets.receive_socketfd;

	char *folder_path = get_user_folder(session->user_context->username);
	while (session->active)
	{
		handle_incoming_file(session,receive_socket,folder_path);
	}
	
	free(folder_path);
	pthread_exit(NULL);
}

void termination(int sig) {
	close(sock_interface_listen);
	close(sock_send_listen);
	close(sock_receive_listen);

	// TODO: fechar todas as sessões abertas

	exit(EXIT_SUCCESS);
}

