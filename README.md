# Simple TFTP server
Written in C, based on [RFC1350](https://tools.ietf.org/html/rfc1350)

## Author
Jón Steinn Elíasson
jonsteinn@gmail.com

## Video demo
[![IMAGE ALT TEXT HERE](https://img.youtube.com/vi/mXTAI9IAiY0/maxresdefault.jpg)](https://www.youtube.com/watch?v=mXTAI9IAiY0)

## Limitations
This TFTP server does not support WRQ. Any put request from a TFTP client will be responded to with an error package.

## Requirments
This server is only unix compatable and requires glib2.0 to run.

## Run
From the project root's directiory, this is an example for port 12345 and access only to a folder called data in the root.
```sh
$ make -C ./src
$ ./src/tftpd 12345 data
```
To run a client and get a file test.txt, use either 
```sh
$ tftp 127.0.0.1 12345 -c get test.txt
```
for netascii mode or
```sh
$ tftp 127.0.0.1 12345 -m octet -c get test.txt
```
for octed mode.

## Features
* Multiple clients at once
* Resends on block numbers mismatch
* Timeouts for inactive clients
* Removal of clients consistently sending incorrect ACK
* Both netascii and octet supported
* Convertion to netascii of data sent
* Avoidancce of parent directory access

## Data structures
### Error pack
All error packages are predefined globals which are ready to use given the address info and fd required to send.
```C
typedef struct
{
    uint16_t opcode;
    uint16_t error_code;
    char message[30];
    size_t size;
} error_pack;
```
### Client value
Client value is the value out of the (key, value) pair in the client pool dictionary. The key being the address. 
```C
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
```
### Server info
Server info holds various variables for receiving and sending and is mostly to avoid bloated parameter list.
```C
typedef struct
{
    int32_t fd;
    sockaddr_in address;
    sockaddr_in received_from;
    char input[516];
} server_info;
```

# Function list
```c
int32_t main(int32_t argc, char **argv);
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
```

# Implementation
## Server loop
The server loop runs until interupted from keyboard or an error occurs that results in terminating the process. The loop has three main parts.
1. Wait for something in the socket, done with select.
2. Read from socket, by using recvfrom and a buffer.
3. Process buffer that was read into. This is either RRQ, ACK or ERR, all other are not allowed.

Additionally, if nothing is on the socket for a specific amount of time, we go through the client pool and remove those that have been inactive for some time.

## Starting new transfer
Assuming client does not already exist, we check if his filename contains two dots for parent directory access and if request file exists. Also we validate the transfer mode and only allow netascii and octet. Failure in any of these will result in an error package sent and the client won't be added to our pool of clients. Otherwise, we read the next 512 bytes from the file and send the first package and also add him to the client pool (dictionary).

If he already exists he generally should not be sending more RRQ. If he does and clients block number is still at 1, we resend the first package up to some amount of resends. If they are reached we send an error and remove the client.

## Continuing existing transfer
First we check if client exists in our pool. If not, he has no business sending acks so we respond with a error pack. If he does exists we check if block numbers match and if not, we resend the last package up to a resend quota, which upon reaching we send an error pack and remove the client from the pool. 

If the block numbers do match, we check if the package size was not full which would mark the end of this transfer and the removal of this client from our pool. If we sent a full package we reset the ressend counter variable to 0, increment the block number (if at max value, set to 1), read the next 512 bytes from file and send them.

## Reading from file
If mode is octet we read each byte as is with `fread()`. If not, we must replace all `\n` and `\r` with `\r\n` and `\r\0` respectively. This is because unix TFTP clients will remove `\r` in netascii mode since they expect windows line feeds to be sent to them. If a binary file is sent, those characters have nothing to do with new lines so the client would be removing bytes essential to the file.
