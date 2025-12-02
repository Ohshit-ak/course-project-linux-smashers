#ifndef CONNECTION_MANAGER_H
#define CONNECTION_MANAGER_H

// Connection management functions
int connect_to_ns();
int connect_to_ss(const char *ip, int port);
int check_ns_alive();
void* monitor_ns_connection(void *arg);

// External globals (defined in client_modular.c)
extern int ns_socket;
extern char username[256];
extern char ns_ip[16];
extern int ns_port;
extern volatile int ns_alive;

#endif // CONNECTION_MANAGER_H
