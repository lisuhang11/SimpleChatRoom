#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

constexpr int BUF_SIZE = 4096;

int setNonBlock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void die(const char* msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        return 1;
    }

    /* 1. ���� TCP socket */
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) die("socket");

    /* 2. ���ӷ����� */
    sockaddr_in servaddr{};
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &servaddr.sin_addr) != 1)
        die("inet_pton");

    if (connect(sockfd, (sockaddr*)&servaddr, sizeof(servaddr)) == -1)
        die("connect");

    setNonBlock(sockfd);
    printf("Connected to %s:%s\nType messages, Ctrl-D to exit.\n",
        argv[1], argv[2]);

    /* 3. poll ��Ҫ������� fd��sockfd �� stdin */
    pollfd fds[2];
    fds[0].fd = sockfd;
    fds[0].events = POLLIN;
    fds[1].fd = STDIN_FILENO;
    fds[1].events = POLLIN;

    char buf[BUF_SIZE];

    while (true) {
        int nready = poll(fds, 2, -1);
        if (nready == -1) {
            if (errno == EINTR) continue;
            die("poll");
        }

        /* 3.1 �������������� -> ��ӡ��Ļ */
        if (fds[0].revents & POLLIN) {
            ssize_t n = recv(sockfd, buf, sizeof(buf), 0);
            if (n <= 0) {                 // 0: �Զ˹ر�  <0: ����
                printf("Server closed connection\n");
                break;
            }
            fwrite(buf, 1, n, stdout);
            fflush(stdout);
        }

        /* 3.2 �������� -> ���ͷ����� */
        if (fds[1].revents & POLLIN) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n == 0) {                 // Ctrl-D
                printf("Bye.\n");
                break;
            }
            if (send(sockfd, buf, n, 0) == -1)
                die("send");
        }

        /* 3.3 �������쳣 */
        if (fds[0].revents & (POLLHUP | POLLERR)) {
            printf("Connection error\n");
            break;
        }
    }

    close(sockfd);
    return 0;
}