#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

const char *SOCKET_A_PATH ; // "/tmp/A"
const char *SOCKET_B_PATH;  // "/tmp/B"
const char *LOG_FILE;       // "/tmp/dumpA_B.log"
#define MAX_CLIENTS 100
#define BUFFER_SIZE 1024*1024

int sock_a;
int server_b;
int client_b_sockets[MAX_CLIENTS];
int client_count = 0;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void log_data(const char *prefix, const char *data, ssize_t len) {
	pthread_mutex_lock(&log_mutex);
    FILE *fp = fopen(LOG_FILE, "a");
    if (fp) {
        time_t now = time(NULL);
        char *timestamp = ctime(&now);
        timestamp[strlen(timestamp)-1] = '\0'; // remove newline
        fprintf(fp, "\n[%s]/[%s]: %d\n",  timestamp,prefix, (int)len);
		char j='\0';
		ssize_t i = 0;
		while(i < len){
			/*if(j == '\0'){
				fprintf(fp,"%02x", data[i]& 0xFF );
				j= ' ';
			}else{
				fprintf(fp,"%02x%c", data[i]& 0xFF,j );
				j= '\0');
			}*/
			fprintf(fp,"0x%02x ", data[i]& 0xFF);
			if( (i %16) == 15){
				fprintf(fp,"\n");
			}
		  
		  i++;
		}
        fclose(fp);
		pthread_mutex_unlock(&log_mutex);
    }
}

void broadcast_to_clients(const char *data, ssize_t len) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; ++i) {
        if (client_b_sockets[i] != -1) {
            if (send(client_b_sockets[i], data, len, 0) == -1) {
                perror("send to B client");
                close(client_b_sockets[i]);
                client_b_sockets[i] = -1;
            }
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void *thread_read_from_a(void *arg) {
    char buffer[BUFFER_SIZE];
    while (1) {
        ssize_t n = read(sock_a, buffer, sizeof(buffer));
        if (n <= 0) {
            perror("read from ACS");
            break;
        }
        broadcast_to_clients(buffer, n);
		log_data("ACS", buffer, n);
    }
    return NULL;
}

void *handle_b_client(void *arg) {
    int client_sock = *(int *)arg;
    char buffer[BUFFER_SIZE];

    while (1) {
        ssize_t n = read(client_sock, buffer, sizeof(buffer));
        if (n <= 0) {
            perror("read from BCAL client");
            break;
        }
        log_data("BCAL", buffer, n);
        write(sock_a,buffer, n);
    }

    close(client_sock);
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; ++i) {
        if (client_b_sockets[i] == client_sock) {
            client_b_sockets[i] = -1;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return NULL;
}

void *thread_accept_b_clients(void *arg) {
    struct sockaddr_un client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (1) {
        int client_sock = accept(server_b, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock == -1) {
            perror("accept BCAL client");
            continue;
        }

        pthread_mutex_lock(&clients_mutex);
        if (client_count < MAX_CLIENTS) {
            client_b_sockets[client_count++] = client_sock;
            pthread_t tid;
            pthread_create(&tid, NULL, handle_b_client, &client_b_sockets[client_count - 1]);
            pthread_detach(tid);
        } else {
            close(client_sock);
            fprintf(stderr, "Max clients reached\n");
        }
        pthread_mutex_unlock(&clients_mutex);
    }
    return NULL;
}

int main(int argc ,char *argv[]) {
    struct sockaddr_un addr_a, addr_b;
	
	if(argc < 4){
		printf("\n invalid usage \n");
		return 0;
	}
	SOCKET_A_PATH = argv[1];
	SOCKET_B_PATH = argv[2];
	LOG_FILE = argv[3];
	

    // Connect to /tmp/A as client
    sock_a = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_a == -1) {
        perror("socket A");
        exit(EXIT_FAILURE);
    }

    memset(&addr_a, 0, sizeof(addr_a));
    addr_a.sun_family = AF_UNIX;
    strncpy(addr_a.sun_path, SOCKET_A_PATH, sizeof(addr_a.sun_path) - 1);

    if (connect(sock_a, (struct sockaddr *)&addr_a, sizeof(addr_a)) == -1) {
        perror("connect to A");
        exit(EXIT_FAILURE);
    }

    // Setup server on /tmp/B
    unlink(SOCKET_B_PATH);
    server_b = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_b == -1) {
        perror("socket B");
        exit(EXIT_FAILURE);
    }

    memset(&addr_b, 0, sizeof(addr_b));
    addr_b.sun_family = AF_UNIX;
    strncpy(addr_b.sun_path, SOCKET_B_PATH, sizeof(addr_b.sun_path) - 1);

    if (bind(server_b, (struct sockaddr *)&addr_b, sizeof(addr_b)) == -1) {
        perror("bind B");
        exit(EXIT_FAILURE);
    }

    if (listen(server_b, 10) == -1) {
        perror("listen B");
        exit(EXIT_FAILURE);
    }

    // Launch threads
    pthread_t thread_a, thread_b;
    pthread_create(&thread_a, NULL, thread_read_from_a, NULL);
    pthread_create(&thread_b, NULL, thread_accept_b_clients, NULL);

    pthread_join(thread_a, NULL);
    pthread_join(thread_b, NULL);

    close(sock_a);
    close(server_b);
    unlink(SOCKET_B_PATH);
    return 0;
}
