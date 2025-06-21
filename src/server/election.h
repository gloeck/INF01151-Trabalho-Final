#ifndef ELECTION_H
#define ELECTION_H

int run_election(int my_id);

void set_election_answer_received(int status);
int get_election_answer_received();

#endif