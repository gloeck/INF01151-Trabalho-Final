#include "election.h"
#include "replica.h"
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

static int election_answer_received = 0;
static pthread_mutex_t answer_mutex = PTHREAD_MUTEX_INITIALIZER;

void set_election_answer_received(int status) {
    pthread_mutex_lock(&answer_mutex);
    election_answer_received = status;
    pthread_mutex_unlock(&answer_mutex);
}

int get_election_answer_received() {
    int status;
    pthread_mutex_lock(&answer_mutex);
    status = election_answer_received;
    pthread_mutex_unlock(&answer_mutex);
    return status;
}

int run_election(int my_id) {
    fprintf(stderr, "[Servidor %d] Iniciando eleição...\n", my_id);

    // Reseta o estado de resposta
    set_election_answer_received(0);

    ReplicaNode* current_replicas = get_replica_list_head();
    int sent_to_higher_id = 0;

    pthread_mutex_lock(&replica_list_mutex); // Trava a lista pra uso
    for (ReplicaNode *node = current_replicas; node != NULL; node = node->next) {
        if (node->id > my_id) {
            fprintf(stderr, "[Servidor %d] Enviando ELECTION para %d\n", my_id, node->id);
            ReplicaEvent event;
            create_election_event(&event, my_id);
            send_event(&event, node->socketfd);
            free_event(&event);
            sent_to_higher_id = 1;
        }
    }
    pthread_mutex_unlock(&replica_list_mutex);

    if (!sent_to_higher_id) {
        fprintf(stderr, "[Servidor %d] Nenhum ID maior. Venci a eleição!\n", my_id);
        return 1;
    }

    sleep(2); // Timeout de 2 segundos

    if (get_election_answer_received()) {
        fprintf(stderr, "[Servidor %d] Recebi ANSWER. Perdi a eleição.\n", my_id);
        return 0;
    } else {
        fprintf(stderr, "[Servidor %d] Timeout, ninguém respondeu. Venci a eleição!\n", my_id);
        return 1;
    }
}