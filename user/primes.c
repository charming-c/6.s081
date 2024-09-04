#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void child(int now_p[]) {
    close(now_p[1]);

    int p;
    if(read(now_p[0], &p, 4) == 0) {
        close(now_p[0]);
        exit(0);
    }
    printf("prime %d\n", p);

    int n;
    int next_p[2];
    pipe(next_p);
    int pid;
    if((pid = fork()) == 0) {
        child(next_p);
    }
    else {
        close(next_p[0]);
        while(read(now_p[0], &n, 4) > 0) {
            if(n % p != 0) {
                write(next_p[1], &n, 4);
            }
        }
        close(next_p[1]);
        wait(0);
    }
    exit(0);
}

int main(int argc, char **argv) {
    
    int p[2];
    pipe(p);
    int pid;
    if((pid = fork()) == 0) {
        child(p);
    }
    else {
        close(p[0]);
        for(int num = 2; num <= 35; num++) {
            write(p[1], &num, 4);
        }
        close(p[1]);
        wait(0);
    }
    exit(0);
}
