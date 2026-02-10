int main() {
    int fd = open("/dev/test_dev", O_RDWR);
    char buf[20];

    printf("APP：准备调用 read，如果没中断我会卡住...\n");
    read(fd, buf, sizeof(buf)); // 这里会阻塞，程序停在这里不往下走
    printf("APP：终于读到了：%s\n", buf);

    close(fd);
    return 0;
}
