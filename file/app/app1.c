#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>

int fd;

// 线程 1：不断写入 AAA... 并读取
void* thread_func1(void* arg) {
    char *msg = "AAAAAAAAAA";
    char buf[20];
    while(1) {
        lseek(fd, 0, SEEK_SET);
        write(fd, msg, strlen(msg));
        
        lseek(fd, 0, SEEK_SET);
        read(fd, buf, sizeof(buf));
        printf("Thread 1 Read: %s\n", buf);
        usleep(1000); // 稍微停顿，方便观察
    }
}

// 线程 2：不断写入 BBB... 并读取
void* thread_func2(void* arg) {
    char *msg = "BBBBBBBBBB";
    char buf[20];
    while(1) {
        lseek(fd, 0, SEEK_SET);
        write(fd, msg, strlen(msg));
        
        lseek(fd, 0, SEEK_SET);
        read(fd, buf, sizeof(buf));
        printf("Thread 2 Read: %s\n", buf);
        usleep(1000);
    }
}

int main() {
    pthread_t t1, t2;
    fd = open("/dev/test_dev", O_RDWR);
    if (fd < 0) { perror("Open Failed"); return -1; }

    pthread_create(&t1, NULL, thread_func1, NULL);
    pthread_create(&t2, NULL, thread_func2, NULL);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    close(fd);
    return 0;
}
