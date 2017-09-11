#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

void foo(const char* folder)
{
    sleep(1);
    if (fork() == 0)
    {
        char cmd0[] = "mkdir";  char *args0[] = {"mkdir", folder, NULL};
        execvp(cmd0, args0);
    }
    else
    {
        int status;
        wait(&status);

        chdir(folder);

        char cmd1[] = "tftp";  char *args1[] = {"tftp", "127.0.0.1", "12321", "-c", "get", "example_data3", NULL};
        execvp(cmd1, args1);    
    }
}

int main(int argc, char** argv)
{
    if (fork() == 0)
    {
        foo("test1");
    }

    if (fork() == 0)
    {
        foo("test2");
    }

    if (fork() == 0)
    {
        foo("test3");
    }

    if (fork() == 0)
    {
        foo("test4");
    }

    while (waitpid(-1, NULL, 0)) 
    {
        if (errno == ECHILD) {
            break;
        }
    }

    return 0;
}
