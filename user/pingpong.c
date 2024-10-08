#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char **argv) {
    int p[2];
    int pid;
    pipe(p);
    if(fork() == 0) {
        pid = getpid();
        char b[1];
        read(p[0], b, 1);
        close(p[0]);
        printf("%d: received ping\n", pid);
        write(p[1], b, 1);
        close(p[1]);
        exit(0);
    } else {
        char b[1];
        pid = getpid();
        write(p[1], "a", 1);
        close(p[1]);
        wait(0);
        read(p[1], b, 1);
        printf("%d: received pong\n", pid);
        close(p[0]);
        exit(0);
    }
}
