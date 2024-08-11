#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"


int main(int argc, char **argv) {
    // 将要传递的命令参数
    char *params[MAXARG];
    // char* cmd = argv[1];
    // 参数列表最后会有一个 null，表示结束
    int n = argc;    

    for (int i = 1; i < n; i++) {
        params[i - 1] = argv[i];
    }

    char p;
    char s[512];
    char *str;
    str = s;
    // 读取所有行
    while(read(0, &p, 1) > 0) {
        // 读取到换行
        if(p == '\n') {
            *str = '\0';
            char *buf[MAXARG];
            for(int i = 0; i<n; i++) {
                buf[i] = params[i];
            }
            buf[n - 1] = s;
            buf[n] = '\0';
            
            int pid;
            if((pid = fork()) == 0) {
                exec(buf[0], buf);
                exit(0);
            }
            wait(0);
            str = s;
        } else {
            *str++ = p;
        }
    }
    exit(0);
}