#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include "tinyosc.h"

#define BUFFER_SIZE 1024
#define DEFAULT_PORT 8888
#define REPLY_PORT 8889
#define MAX_STATUS_LEN 1024

static volatile int keep_running = 1;
static char eth0_ip[INET_ADDRSTRLEN] = "0.0.0.0";

int get_ipv6_status() {
    FILE *fp = fopen("/proc/sys/net/ipv6/conf/all/disable_ipv6", "r");
    if (!fp) {
        return -1;
    }
    
    char status[2] = {0};
    if (fgets(status, sizeof(status), fp) != NULL) {
        fclose(fp);
        return (status[0] == '1') ? 1 : 0;
    }
    
    fclose(fp);
    return -1;
}

int switch_ipv6(int enable) {
    if (enable != 0 && enable != 1) {
        return -1;
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "/usr/bin/switch_ipv6.sh %d", enable);

    printf("Executing IPv6 switch command: %s\n", cmd);
    int result = system(cmd);
    printf("Command result: %d\n", result);
    
    return result;
}

char* get_fw_env_status() {
    static char status[MAX_STATUS_LEN];
    FILE *fp;
    char buffer[128];

    fp = popen("fw_printenv hostname udc_handle_suspend usb_ethernet_mode netmask_eth lnb_power attr_name attr_val audio_mode rf_input rf_output serial_force callsign locator ipaddr ipaddr_host", "r");
    if (fp == NULL) {
        return "error=Failed to execute fw_printenv";
    }

    status[0] = '\0';
    while (fgets(buffer, sizeof(buffer)-1, fp)) {
        char *newline = strchr(buffer, '\n');
        if (newline) *newline = ',';
        strcat(status, buffer);
    }

    int len = strlen(status);
    if (len > 0 && status[len-1] == ',') {
        status[len-1] = '\0';
    }

    pclose(fp);
    return status;
}

const char* get_eth0_ip() {
    struct ifaddrs *ifaddr, *ifa;
    
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return "0.0.0.0";
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) 
            continue;

        if (strcmp(ifa->ifa_name, "eth0") == 0) {
            struct sockaddr_in *addr = (struct sockaddr_in*)ifa->ifa_addr;
            inet_ntop(AF_INET, &addr->sin_addr, eth0_ip, INET_ADDRSTRLEN);
            break;
        }
    }

    freeifaddrs(ifaddr);
    return eth0_ip;
}

void signal_handler(int signum) {
    (void)signum;
    keep_running = 0;
}

int parse_osc_message(const char *buffer, int len, char *command, int *num) {
    printf("Parsing message with length %d\n", len);
    if (len < 8) {
        printf("Message too short\n");
        return -1;
    }

    if (strncmp(buffer, "/", 1) != 0) {
        printf("Invalid message format: doesn't start with /\n");
        return -1;
    }
    
    printf("Command prefix: %.*s\n", 7, buffer);
    
    if (strncmp(buffer, "/status", 7) == 0) {
        strcpy(command, "status");
        *num = 0;
        return 0;
    } else if (strncmp(buffer, "/ipv6", 5) == 0) {
        strcpy(command, "ipv6");
        const char *type_tag = buffer + 8;
        if (type_tag[0] != ',' || type_tag[1] != 'i') {
            printf("Invalid type tag for ipv6 command\n");
            return -1;
        }
        const char *data = type_tag + 4;
        *num = ntohl(*(int *)data);
        printf("Parsed IPv6 command with value: %d\n", *num);
        return 0;
    } else if (strncmp(buffer, "/rx", 3) == 0) {
        strcpy(command, "rx");
    } else if (strncmp(buffer, "/tx", 3) == 0) {
        strcpy(command, "tx");
    } else {
        printf("Unknown command\n");
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

void send_reply(const char *target_ip, const char *type, int num, int result) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("Reply socket creation failed");
        return;
    }

    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = 0;
    src_addr.sin_addr.s_addr = inet_addr(eth0_ip);

    if (bind(sock, (struct sockaddr *)&src_addr, sizeof(src_addr)) < 0) {
        perror("bind for source address failed");
        close(sock);
        return;
    }

    struct sockaddr_in reply_addr;
    memset(&reply_addr, 0, sizeof(reply_addr));
    reply_addr.sin_family = AF_INET;
    reply_addr.sin_port = htons(REPLY_PORT);
    
    if (inet_pton(AF_INET, target_ip, &reply_addr.sin_addr) <= 0) {
        printf("Invalid IP address: %s\n", target_ip);
        close(sock);
        return;
    }

    char buffer[256];
    int len = tosc_writeMessage(buffer, sizeof(buffer), type, "i", num);

    printf("Debug: Preparing to send reply:\n");
    printf("  Source IP: %s\n", eth0_ip);
    printf("  Target IP: %s\n", target_ip);
    printf("  Port: %d\n", REPLY_PORT);
    printf("  Message size: %d bytes\n", len);
    
    printf("  Message content (hex):");
    for (int i = 0; i < len; i++) {
        if (i % 16 == 0) printf("\n    ");
        printf("%02x ", (unsigned char)buffer[i]);
    }
    printf("\n");

    ssize_t sent = sendto(sock, buffer, len, 0,
                         (struct sockaddr *)&reply_addr, sizeof(reply_addr));
    if (sent < 0) {
        perror("Reply send failed");
        printf("errno: %d\n", errno);
    } else {
        printf("Reply sent successfully: %zd bytes\n", sent);
    }
    
    close(sock);
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    int sock;

    // Wait for network interface to be up
    sleep(2);

    // Get eth0 IP (retry a few times if needed)
    int retry = 0;
    while (retry < 5) {
        get_eth0_ip();
        if (strcmp(eth0_ip, "0.0.0.0") != 0) {
            break;
        }
        sleep(1);
        retry++;
    }

    if (strcmp(eth0_ip, "0.0.0.0") == 0) {
        fprintf(stderr, "Failed to get eth0 IP address\n");
        return 1;
    }

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

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(eth0_ip);
    server_addr.sin_port = htons(port);

    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt failed");
        close(sock);
        return 1;
    }

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(sock);
        return 1;
    }

    printf("OSC server listening on %s:%d...\n", eth0_ip, port);
    printf("Commands: /rx [1|2] or /tx [1|2] or /status or /ipv6 [0|1]\n");

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

        printf("Received %d bytes from %s\n", recv_len, client_ip);
        printf("Raw message content (hex):");
        for (int i = 0; i < recv_len; i++) {
            if (i % 16 == 0) printf("\n    ");
            printf("%02x ", (unsigned char)buffer[i]);
        }
        printf("\n");

        char command[10];
        int num;
        if (parse_osc_message(buffer, recv_len, command, &num) == 0) {
            if (strcmp(command, "status") == 0) {
                char *status = get_fw_env_status();
                
                char reply_buffer[MAX_STATUS_LEN + 32];
                int len = tosc_writeMessage(reply_buffer, sizeof(reply_buffer),
                                          "/status", "s", status);
                
                struct sockaddr_in reply_addr = client_addr;
                reply_addr.sin_port = htons(REPLY_PORT);
                
                sendto(sock, reply_buffer, len, 0,
                      (struct sockaddr *)&reply_addr, sizeof(reply_addr));
                
                printf("Status sent to %s\n", client_ip);
            } else if (strcmp(command, "ipv6") == 0) {
                printf("Processing IPv6 command with value: %d\n", num);
                if (num == 0 || num == 1) {
                    switch_ipv6(num);
                }
                
                int status = get_ipv6_status();
                char reply_buffer[256];
                int len = tosc_writeMessage(reply_buffer, sizeof(reply_buffer),
                                          "/ipv6", "i", status);
                
                struct sockaddr_in reply_addr = client_addr;
                reply_addr.sin_port = htons(REPLY_PORT);
                
                sendto(sock, reply_buffer, len, 0,
                      (struct sockaddr *)&reply_addr, sizeof(reply_addr));
                
                printf("IPv6 status=%d (requested by %s)\n", status, client_ip);
            } else {
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
    }

    close(sock);
    return 0;
}