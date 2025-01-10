#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <errno.h>

#define BUFFER_SIZE 1024
#define DEFAULT_PORT 8888
#define REPLY_PORT 8889

static volatile int keep_running = 1;

void signal_handler(int signum) {
    (void)signum;
    keep_running = 0;
}

// parse osc message
int parse_osc_message(const char *buffer, int len, char *command, int *num) {
    if (len < 8) return -1;

    if (strncmp(buffer, "/", 1) != 0) return -1;
    
    if (strncmp(buffer, "/rx", 3) == 0) {
        strcpy(command, "rx");
    } else if (strncmp(buffer, "/tx", 3) == 0) {
        strcpy(command, "tx");
    } else {
        return -1;
    }

    const char *type_tag = buffer + 4;
    if (type_tag[0] != ',' || type_tag[1] != 'i') return -1;

    const char *data = type_tag + 4;
    *num = ntohl(*(int *)data);

    return 0;
}

int switch_port(const char *type, int num) {
    char cmd[256];
    if (strcmp(type, "rx") == 0) {
        snprintf(cmd, sizeof(cmd), "/usr/bin/switch_rfinput.sh rx%d", num);
    } else if (strcmp(type, "tx") == 0) {
        snprintf(cmd, sizeof(cmd), "/usr/bin/switch_rfoutput.sh tx%d", num);
    } else {
        return -1;
    }
    return system(cmd);
}

// response
void send_reply(const char *target_ip, const char *type, int num, int result) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    struct sockaddr_in reply_addr;
    memset(&reply_addr, 0, sizeof(reply_addr));
    reply_addr.sin_family = AF_INET;
    reply_addr.sin_port = htons(REPLY_PORT);
    inet_pton(AF_INET, target_ip, &reply_addr.sin_addr);

    char reply[128];
    snprintf(reply, sizeof(reply), "/%s/status,%s,%d", type, 
             result == 0 ? "success" : "error", num);
    
    sendto(sock, reply, strlen(reply), 0,
           (struct sockaddr *)&reply_addr, sizeof(reply_addr));
    
    close(sock);
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    int sock;

    if (argc > 1) {
        port = atoi(argv[1]);
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket creation failed");
        return 1;
    }

    // ソケットオプションの追加
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt failed");
        close(sock);
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(sock);
        return 1;
    }

    printf("OSC server listening on port %d...\n", port);
    printf("Commands: /rx [1|2] or /tx [1|2]\n");

    while (keep_running) {
        char buffer[BUFFER_SIZE];
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int recv_len = recvfrom(sock, buffer, BUFFER_SIZE, 0,
                               (struct sockaddr *)&client_addr, &client_len);
        
        if (recv_len < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("recvfrom failed");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

        char command[3];
        int num;
        if (parse_osc_message(buffer, recv_len, command, &num) == 0) {
            // error = rx1
            if (num != 1 && num != 2) {
                printf("Invalid %s number %d, defaulting to 1 (request from %s)\n", 
                       command, num, client_ip);
                num = 1;
            }
            printf("Switching %s to %d (request from %s)\n", command, num, client_ip);
            int result = switch_port(command, num);
            send_reply(client_ip, command, num, result);
        }
    }

    close(sock);
    return 0;
}