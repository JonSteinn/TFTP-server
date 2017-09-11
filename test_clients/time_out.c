#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    int sockfd;
    size_t msg_size;
    struct sockaddr_in server;
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);
    server.sin_port = htons(atoi(argv[1]));
    char buffer[516];
    memset(buffer, 0, 516);
    buffer[1] = 1;
    strcpy(buffer + 2, argv[2]);
    size_t padding = strlen(buffer+2);
    strcpy(buffer + 2 + padding + 1, argv[3]);
    fprintf(stdout, "Sending RRQ on port %s for file %s in mode %s\n", argv[1], argv[2], argv[3]);
    size_t padding2 = strlen(buffer + 3 + padding);
    sendto(sockfd, buffer, padding + padding2 + 4, 0, (struct sockaddr *) &server, sizeof(server));
    while (1)
    {
        socklen_t len = (socklen_t) sizeof(server);
        ssize_t n = recvfrom(sockfd, buffer, 516, 0, (struct sockaddr *) &server, &len);   
        if (buffer[1] == 3) 
        {
            fprintf(stdout, "RECEIVED DATA...hanging\n"); fflush(stdout);
        }
        else if (buffer[1] == 5)
        {
            fprintf(stdout, "ERROR!!!! Stop hanging\n"); fflush(stdout);
            break;
        }
        else
        {
            fprintf(stdout, "Something weird is happening %d\n", buffer_in[1]); fflush(stdout);
        }
    }
    fprintf(stdout, "Success!\n");
    close(sockfd);
    exit(EXIT_SUCCESS);
}
