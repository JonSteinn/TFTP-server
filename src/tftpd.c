/////////////////////////
// Simple TFTP Server  //
// Jon Steinn Eliasson //
// jonsteinn@gmail.com //
/////////////////////////

//////////////
// Includes //
//////////////
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/time.h> 
#include <arpa/inet.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <glib.h>

/////////////
// Defines //
/////////////
#define ERROR(x) ((x) < 0)
#define INACTIVE_TIMER 5
#define CLIENT_TIMEOUT 5
#define MAX_RESENDS 5

//////////////
// Typedefs //
//////////////
typedef struct sockaddr_in sockaddr_in;
typedef struct sockaddr sockaddr;

///////////////////////
// Enums and structs //
///////////////////////
typedef enum 
{ 
    RRQ = 1,  // read request
    WRQ = 2,  // write request
    DATA = 3, // data 
    ACK = 4,  // acknowledgement
    ERR = 5,  // error
    NONE = 6  // none
} opcode;

typedef enum 
{ 
    UNDEFINED = 0, 
    NO_FILE, 
    ACCESS_VIOLATION, 
    DISK_FULL, 
    ILLEGAL_OP, 
    UNKNOWN_ID, 
    FILE_ALREADY_EXISTS, 
    NO_USER 
} error_code;

typedef enum
{
    netascii = 1,
    octet,
    mail,
    invalid
} mode;

typedef struct
{
    uint16_t opcode;
    uint16_t error_code;
    char message[30];
    size_t size;
} error_pack;

typedef struct
{
    FILE* file_fd;
    char buffer[516];
    size_t buffer_size;
    uint16_t block_number;
    uint16_t resends;
    mode md;
    char temp_char;
    time_t last_action;
} client_value;

typedef struct
{
    int32_t fd;
    sockaddr_in address;
    sockaddr_in received_from;
    char input[516];
} server_info;

/////////////
// Globals //
/////////////
static bool server_loop = true;
static const error_pack error_packs[] =
{
    {1280, 0,    "Undefined",              13},  // htons(0) = 0
    {1280, 256,  "No such file",           16},  // htons(1) = 256
    {1280, 512,  "Access violation",       20},  // htons(2) = 512
    {1280, 768,  "Disk full",              13},  // htons(3) = 768
    {1280, 1024, "Illegal TFTP operation", 26},  // htons(4) = 1024
    {1280, 1280, "Unknown transfer id",    23},  // htons(5) = 1280
    {1280, 1536, "File already exists",    23},  // htons(6) = 1536
    {1280, 1792, "No such user",           16}   // htons(7) = 1792
};

/////////////////////////
// Function predefines //
/////////////////////////
void int_handler(int32_t signal);
void exit_error(const char* str);
void start_server(server_info* server, char** argv);
void init_server(const char* port, server_info* server);
uint16_t convert_port(const char* port_string);
bool some_waiting(server_info* server);
gboolean timed_out(gpointer key, gpointer value, gpointer user_data);
guint client_hash(const void* key);
gboolean client_equals(const void* lhs, const void* rhs);
sockaddr_in* sockaddr_cpy(sockaddr_in* src);
void socket_listener(server_info* server);
void ip_message(sockaddr_in* client, bool greeting);
void send_error(server_info* server, error_code err);
void start_new_transfer(GHashTable* clients, server_info* server, char* root);
void continue_existing_transfer(GHashTable* clients, server_info* server);
void read_to_buffer(client_value* client);
size_t construct_full_path(char* dest, const char* root, const char* file_name);
int32_t get_mode(char* str);
void destroy_value(gpointer data);
client_value* init_client(mode m);

///////////////
// Functions //
///////////////

/*
 * Starting point.
 */
int32_t main(int32_t argc, char **argv)
{
    // Override ctrl+c
    signal(SIGINT, int_handler);
    
    // Excessive are ignored but 3 needed to run server
    if (argc < 3)
    {
        exit_error("Invalid arguments!\n");
    }

    // Start server
    server_info server;
    start_server(&server, argv);

    // Close socket
    close(server.fd);

    return 0;
}

/*
 * Server starting point, includes main loop.
 */
void start_server(server_info* server, char** argv)
{
    fprintf(stdout, "Setting up server...\n");

    // Set up socket
    init_server(argv[1], server);

    fprintf(stdout, "Server setup complete...\n");

    // Collection for clients
    GHashTable* clients = g_hash_table_new_full(client_hash, client_equals, free, destroy_value);

    fprintf(stdout, "Starting server loop...\n");
    fprintf(stdout, "Listening on port %s...\n", argv[1]);
    fflush(stdout);

    // Runs until interupted by SIGINT
    while(server_loop) 
    {
        // Check if any packets are in the socket, if not...
        if (!some_waiting(server))
        {
            //fprintf(stdout, "DEBUG: Inactive\n"); fflush(stdout);

            // Time out inactive clients
            g_hash_table_foreach_remove(clients, timed_out, server);
            continue;
        }

        // Retrieve what is on the socket
        socket_listener(server);

        // If first byte is not 0, then opcode is more than 1<<8 
        // and we set the second byte to send an error message
        if (server->input[0]) 
        {
            server->input[1] = NONE;
        }

        switch (server->input[1] /* Opcode */)
        {
            case RRQ:
                //fprintf(stdout, "DEBUG: PACK = RRQ\n"); fflush(stdout);
                start_new_transfer(clients, server, argv[2]);
                break;
            case ACK:
                //fprintf(stdout, "DEBUG: PACK = ACK\n"); fflush(stdout);
                continue_existing_transfer(clients, server);
                break;
            case ERR:
                //fprintf(stdout, "DEBUG: PACK = ERR\n"); fflush(stdout);
                g_hash_table_remove(clients, &server->received_from);
                break;
            default:
                //fprintf(stdout, "DEBUG: PACK = UNKNOWN\n"); fflush(stdout);
                send_error(server, ACCESS_VIOLATION);
        }
    }

    g_hash_table_destroy(clients);
}

/*
 * Signal listener for SIGINT.
 */
void int_handler(int signal)
{
    if (signal == SIGINT)
    {
        server_loop = false;

        fprintf(stdout, "Terminating server shortly...\n");
        fflush(stdout);
    }
}

/*
 * For abnormal terminations of program.
 */
void exit_error(const char* str) 
{
    perror(str);

    fprintf(stdout, "Exiting...\n");
    fflush(stdout);
    
    exit(EXIT_FAILURE);
}

/*
 * Create and bind socket.
 */
void init_server(const char* port, server_info* server) 
{
    // domain = v4, type = UDP, protocol = default
    server->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ERROR(server->fd))
    {
        exit_error("Failed to create socket!\n");
    }

    memset(&server->address, 0, sizeof(sockaddr_in));
    server->address.sin_family = AF_INET;                   // address familty = v4
    server->address.sin_port = htons(convert_port(port));   // port in network byte order
    server->address.sin_addr.s_addr = htonl(INADDR_ANY);    // all available interfaces
    
    if (ERROR(bind(server->fd, (sockaddr*)&server->address, (socklen_t)sizeof(sockaddr_in))))
    {
        exit_error("Failed to bind socket!\n");
    }
}

/*
 * Convert port as string to an unsigned short.
 */
uint16_t convert_port(const char* port_string)
{
    int32_t port = strtoul(port_string, NULL, 0);
    if (errno == ERANGE || port == 0 || port > 65535)
    {
        exit_error("Invalid port!\n");
    }
    return (uint16_t)port;
}

/*
 * Check if there is data in the socket.
 */
bool some_waiting(server_info* server)
{
    // Set inactive timer
    struct timeval tv;
    tv.tv_sec = INACTIVE_TIMER;
    tv.tv_usec = 0;

    // Create, restart and add server fd to set
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(server->fd, &rfds);
    
    int32_t s = select(server->fd + 1, &rfds, NULL, NULL, &tv);
    if (ERROR(s))
    {
    	if (!server_loop) return false;
        exit_error("Select failed\n");
    }
    
    return s > 0 && FD_ISSET(server->fd, &rfds);
}

/*
 * Timeout checker for hash table iteration. Returns true iff 
 * timed out which leads to it being removed from the hash table.
 */
gboolean timed_out(gpointer key, gpointer value, gpointer user_data)
{
    // Pointer casting
    sockaddr_in* client_key = (sockaddr_in*)key;
    client_value* client_val = (client_value*)value;
    server_info* server = (server_info*)user_data;
    
    time_t now = time(NULL);
    if (difftime(now, client_val->last_action) >= CLIENT_TIMEOUT)
    {
        // Send error to timed out client
        memcpy(&server->received_from, client_key, sizeof(sockaddr_in));
        send_error(server, UNDEFINED);

        ip_message(client_key, false);
        
        return TRUE;
    }
    return FALSE;
}

/*
 * Hashing for clients.
 */
guint client_hash(const void* key)
{
    sockaddr_in* k = (sockaddr_in*)key;
    return 41 * k->sin_port + 47 * k->sin_addr.s_addr;
}

/*
 * Equal comparison for clients.
 */
gboolean client_equals(const void* lhs, const void* rhs)
{
    sockaddr_in* a = (sockaddr_in*)lhs;
    sockaddr_in* b = (sockaddr_in*)rhs;
    return a->sin_port == b->sin_port && a->sin_addr.s_addr == b->sin_addr.s_addr;
}

/*
 * Memory deallocator for hash map's value which 
 * also handles closing file descriptor. 
 */
void destroy_value(gpointer data) 
{
    client_value* cv = (client_value*)data;
    if (cv->file_fd != NULL)
    {
        fclose(cv->file_fd);
    }
    free(cv);
}

/*
 * Read packet from socket.
 */
void socket_listener(server_info* server)
{
    socklen_t len = (socklen_t)sizeof(sockaddr_in);
    ssize_t n = recvfrom(server->fd, server->input, sizeof(server->input)-1, 
        0, (sockaddr*)&server->received_from, &len);
    
    if (ERROR(n))
    {
        exit_error("Failure in receiving a message!\n");
    }
    
    server->input[n] = 0;
}

/*
 * Send error package to address in received_from in server. Uses
 * predefined error packages with predefined error messages.
 */
void send_error(server_info* server, error_code err)
{
    fprintf(stdout, "Sending error: %s\n", error_packs[err].message);
    fflush(stdout);

    if (ERROR(sendto(server->fd, (void*)&error_packs[err], error_packs[err].size, 
        0, (sockaddr*)&server->received_from, sizeof(sockaddr_in))))
    {
        exit_error("Send failed\n");
    }
}

/*
 * Handling of RRQ requests. If valid, client is added to pool.
 */
void start_new_transfer(GHashTable* clients, server_info* server, char* root)
{
    // If a client resends a read request in a middle of a transfer
    if (g_hash_table_contains(clients, &server->received_from))
    {
        //fprintf(stdout, "DEBUG: Double RRQ from client\n"); fflush(stdout);

        client_value* client = (client_value*)g_hash_table_lookup(clients, &server->received_from);

        // If client is not on first data package, we terminate his transfer since
        // he should not be sending RRQ at this point. If at first package, we allow
        // resends of first package.
        if (client->block_number != 1)
        {
            //fprintf(stdout, "DEBUG: RRQ in mid transfer\n"); fflush(stdout);

            send_error(server, ILLEGAL_OP);
            g_hash_table_remove(clients, &server->received_from);
        }
        else
        {
            // On too many resends, we stop resending and send one error before
            // terminating. Otherwise we resend the first package.
            if (client->resends++ == MAX_RESENDS)
            {
                //fprintf(stdout, "DEBUG: Removing after constant RRQ\n"); fflush(stdout);

                send_error(server, UNDEFINED);
                g_hash_table_remove(clients, &server->received_from);
            }
            else
            {
                //fprintf(stdout, "DEBUG: Resending after double RRQ\n"); fflush(stdout);

                if (ERROR(sendto(server->fd, client->buffer, client->buffer_size, 
                    0, (sockaddr*)&server->received_from, sizeof(sockaddr_in))))
                {
                    exit_error("Send failed\n");
                }
            }
        }
        return;
    }

    // Show client in server's stdout
    ip_message(&server->received_from, true);

    // Construct full path. 0 is returned if path contains parent directory access.
    char full_path[512];
    size_t size = construct_full_path(full_path, root, server->input + 2);
    if (!size)
    {
        //fprintf(stdout, "DEBUG: Client wanted to access parent directory\n"); fflush(stdout);
        send_error(server, ACCESS_VIOLATION);
        return;
    }

    fprintf(stdout, "PATH: %s\n", full_path);

    //fprintf(stdout, "DEBUG: File asked for is %s\n", full_path); fflush(stdout);

    // Allocate memory for a new client
    client_value* new_client = init_client(get_mode(server->input + size + 3));
    switch(new_client->md)
    {
        case netascii:
            fprintf(stdout, "Mode: NETASCII\n");
            new_client->file_fd = fopen(full_path, "r");
            break;
        case octet:
            fprintf(stdout, "Mode: OCTET\n");
            new_client->file_fd = fopen(full_path, "rb");
            break;
        default:
            fprintf(stdout, "Mode: Not Supported\n");
            send_error(server, ILLEGAL_OP);
            return;
    }

    // If file was not found, we tell the client
    if (new_client->file_fd == NULL)
    {
        //fprintf(stdout, "DEBUG: File does not exists\n"); fflush(stdout);
        send_error(server, NO_FILE);
        free(new_client);
        return;
    }

    //fprintf(stdout, "DEBUG: File does exists\n"); fflush(stdout);
    fprintf(stdout, "Beginning transfer...\n");

    // Add next 512 bytes of client's file descriptor to a buffer
    read_to_buffer(new_client);

    //fprintf(stdout, "DEBUG: Ready to send %zu bytes\n", new_client->buffer_size); fflush(stdout);

    // Send first pack
    if (ERROR(sendto(server->fd, new_client->buffer, new_client->buffer_size, 
        0, (sockaddr*)&server->received_from, sizeof(sockaddr_in))))
    {
        exit_error("Send failed\n");
    }

    // Add client to client pool
    g_hash_table_insert(clients, sockaddr_cpy(&server->received_from), new_client);

    fflush(stdout);
}

/*
 * Create path from root directory and file. File can incldude path as
 * long as it does not contain "..", upon which 0 is returned. Otherwise
 * a positive number is returned. New path is "<root>/<file>\0".
 */
size_t construct_full_path(char* dest, const char* root, const char* file_name)
{
    if (strstr(file_name, "..") != NULL) 
    {
        return 0;
    }

    // Lengths
    size_t file_name_size = strlen(file_name);
    size_t root_size = strlen(root);

    // Constructing new string
    strncpy(dest, root, root_size);
    dest[root_size] = '/';
    strncpy(dest + root_size + 1, file_name, file_name_size);
    dest[root_size + file_name_size + 1] = '\0';

    //fprintf(stdout, "DEBUG: path = %s\n", dest); fflush(stdout);
    
    return file_name_size;
}

/*
 * Convert string to mode enum.
 */
int32_t get_mode(char* str)
{
    size_t len = strlen(str);

    // Convert mode to upper since all combinations of 
    // upper and lower case digits should be supported
    char upper[len + 1];
    for (size_t i = 0; i < len; i++)
    {
        upper[i] = toupper(str[i]);
    }
    upper[len] = '\0';
    //fprintf(stdout, "DEBUG: model = %s\n", upper); fflush(stdout);

    // We only allow 'NETASCII' and 'OCTET'.
    if (len == 8 && !strncmp("NETASCII", upper, 8))
    {
        return netascii;
    }
    else if (len == 5 && !strncmp("OCTET", upper, 5))
    {
        return octet;
    }
    else if (len == 4 && !strncmp("MAIL", upper, 4))
    {
        return mail;
    }
    else
    {
        return invalid;
    }
}

/*
 * Print ip and port of client. Either when first contact 
 * is made or when removing from client pool.
 */
void ip_message(sockaddr_in* client, bool greeting)
{
    char ip_buffer[16];
    memset(ip_buffer, 0, sizeof(ip_buffer));
    if (inet_ntop(AF_INET, &(client->sin_addr), ip_buffer, 16) != NULL) 
    {
        fprintf(stdout, "%s %s on port %hu...\n", 
            greeting ? "Request received from" : "Terminating", 
            ip_buffer, client->sin_port);
    }
}

/*
 * Deal with ACK packates for already existing clients.
 */
void continue_existing_transfer(GHashTable* clients, server_info* server)
{
    // If client does not exist, he should not be sending ACKs
    if (!g_hash_table_contains(clients, &server->received_from))
    {
        //fprintf(stdout, "DEBUG: ACK from unknown source\n"); fflush(stdout);
        send_error(server, UNKNOWN_ID);
        return;
    }

    // Byte 2: aaaa-bbbb
    // Byte 3: cccc-dddd
    // Block number: aaaa-bbbb-cccc-dddd
    uint16_t block_number = (unsigned char)server->input[3] + ((unsigned char)(server->input[2]) << 8);

    // Reset last action to current time
    client_value* client = (client_value*)g_hash_table_lookup(clients, &server->received_from);
    client->last_action = time(NULL);

    //fprintf(stdout, "DEBUG: BN = (%hu,%hu)%s\n", block_number, client->block_number); fflush(stdout);

    // If block number does not match
    if (client->block_number != block_number)
    {
        //fprintf(stdout, "DEBUG: Block number mismatch\n"); fflush(stdout);

        // Check if too many resends already. If so, send error and remove
        // client from client pool. Otherwise resend.
        if (client->resends++ == MAX_RESENDS)
        {
            //fprintf(stdout, "DEBUG: Resends depleted\n"); fflush(stdout);
            send_error(server, UNDEFINED);
            g_hash_table_remove(clients, &server->received_from);
        }
        else
        {
            //fprintf(stdout, "DEBUG: Sending last package\n"); fflush(stdout);
            if (ERROR(sendto(server->fd, client->buffer, client->buffer_size, 
                0, (sockaddr*)&server->received_from, sizeof(sockaddr_in))))
            {
                exit_error("Send failed\n");
            }
        }
        return;
    }

    // Check if transfer is done and if so, remove client from pool
    if (client->buffer_size < 516)
    {
        fprintf(stdout, "Transfer done, client removed from pool...\n");
        fflush(stdout);

        //fprintf(stdout, "DEBUG: Last package confirmed\n"); fflush(stdout);
        g_hash_table_remove(clients, &server->received_from);
        return;
    }
    
    // If block number match, we reset resends
    client->resends = 0;

    //fprintf(stdout, "DEBUG: BN before = %hu\n", client->block_number); fflush(stdout);

    // Increment block number, circular ignoring 0.
    client->block_number = (client->block_number == 65535 ? 1 : client->block_number + 1);

    //fprintf(stdout, "DEBUG: BN after = %hu\n", client->block_number); fflush(stdout);

    // Read next 512 byte from client's file descriptor
    read_to_buffer(client);

    //fprintf(stdout, "DEBUG: Metadata = (%d,%d,%d,%d)\n", client->buffer[0], client->buffer[1], 
    //             client->buffer[2], client->buffer[3]); fflush(stdout);
    //fprintf(stdout, "DEBUG: Ready to send %zu bytes\n", client->buffer_size); fflush(stdout);

    // Send next package
    if (ERROR(sendto(server->fd, client->buffer, client->buffer_size, 
        0, (sockaddr*)&server->received_from, sizeof(sockaddr_in))))
    {
        exit_error("Send failed\n");
    }

}

/*
 * Read file to packet. If mode is netascii we replace '\n' and '\r'
 * with '\r\n' and '\r\n' respectively. Octet is an agreement to send
 * files as is. Netascii however expects windows line endings. Tftp
 * clients will remove '\r' when running on Unix so not replacing '\n'
 * with '\r\n' does not matter but for a windows client, it would miss
 * the '\r'. When it comes to binary files however, we do not have any
 * line ending meaning with bytes like '\n' and '\r' so sending such 
 * files without replacement to an unix client will have the client
 * remove the '\r' so the binary file will be broken. If we replace
 * '\r' with '\r\0', it will only remove the '\0'.
 */
void read_to_buffer(client_value* client)
{
    // add block number to buffer
    client->buffer[2] = (client->block_number >> 8);
    client->buffer[3] = client->block_number;

    if (client->md == octet)
    {
        // If mode is octed, no need to do anything special
        client->buffer_size = 4 + fread(client->buffer + 4, 1, 512, client->file_fd);
    }
    else
    {
        // It would be possible to have to replace 1 char with 2 when
        // there is only one char left on the buffer which would require
        // us to distribute the two replacement characters over two packets.
        // That's why we store a temp char variable for all clients. When
        // -1, there is not temp char stored.

        // Acts as an index & counter
        size_t counter = 0;

        // If we owe the buffer a character from last package
        if (client->temp_char != -1)
        {
            client->buffer[4 + counter++] = client->temp_char;
            client->temp_char = -1;
        }

        // Max chars read is 512
        while (counter < 512)
        {
            // Check if fail is already read to end
            int32_t next = fgetc(client->file_fd);
            if (next == EOF)
            {
                break;
            }

            char next_char = (char)next;

            if (next_char == '\n')
            {
                // '\n' is replaced with '\r'
                client->buffer[4 + counter++] = '\r';

                if (counter ==  512)
                {
                    // Special case: no more space on buffer, add in next pack
                    client->temp_char = '\n';
                }
                else
                {
                    client->buffer[4 + counter++] = '\n';
                }
            }
            else if (next_char == '\r')
            {
                client->buffer[4 + counter++] = '\r';
                if (counter == 512)
                {
                    client->temp_char = '\0';
                }
                else
                {
                    // Special case: no more space, '\0' added next pack
                    client->buffer[4 + counter++] = '\0';
                }
            }
            else
            {
                client->buffer[4 + counter++] = next_char;
            }   
        }

        // Return size of buffer, including metadata
        client->buffer_size = 4 + counter;
    }
}

/*
 * Copy sockaddr_in for hash table.
 */
sockaddr_in* sockaddr_cpy(sockaddr_in* src)
{
    sockaddr_in* copy = (sockaddr_in*)malloc(sizeof(sockaddr_in));
    memcpy(copy, src, sizeof(sockaddr_in));
    return copy;
}

/*
 * Allocate and init client value for hash table.
 */
client_value* init_client(mode m)
{
    client_value* c = (client_value*)malloc(sizeof(client_value));
    c->file_fd = NULL;
    c->buffer_size = 516;
    c->block_number = 1;
    c->resends = 0;
    c->md = m;
    c->temp_char = -1;
    c->last_action = time(NULL);
    c->buffer[0] = 0;
    c->buffer[1] = DATA;
    return c;
}
