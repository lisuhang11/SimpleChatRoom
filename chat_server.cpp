#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <assert.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

constexpr int LISTEN_PORT = 5555;
constexpr int MAX_EVENTS = 1024;     // poll 最大同时监控 fd
constexpr int BUF_SIZE = 4096;     // 读写缓冲区大小
constexpr int BACKLOG = 128;

/* 每个连接的上下文 ---------------------------------------------------- */
struct Conn {
    sockaddr_in addr{};
    char        rbuf[BUF_SIZE]{};
    ssize_t     rbytes = 0;           // 已读字节数
    std::string wbuf;                 // 待发送数据
};

/* 工具函数 ------------------------------------------------------------ */
int setNonBlock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void die(const char* msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

/* 主函数 -------------------------------------------------------------- */
int main() {
    /* 1. 监听 socket -------------------------------------------------- */
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) die("socket");

    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in servaddr{};
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(LISTEN_PORT);

    if (bind(listenfd, (sockaddr*)&servaddr, sizeof(servaddr)) == -1)
        die("bind");

    if (listen(listenfd, BACKLOG) == -1) die("listen");

    setNonBlock(listenfd);

    /* 2. 预分配 Conn 数组（空间换时间） ------------------------------- */
    Conn* conns = new Conn[FD_SETSIZE]();   // FD_SETSIZE 通常是 1024/65536
    std::vector<pollfd> pollfds;
    pollfds.reserve(MAX_EVENTS);
    pollfds.push_back({ listenfd, POLLIN, 0 });

    printf("Chat server listening on port %d …\n", LISTEN_PORT);

    /* 3. 事件循环 ----------------------------------------------------- */
    for (;;) {
        int nready = poll(pollfds.data(), pollfds.size(), -1);
        if (nready == -1) {
            if (errno == EINTR) continue;
            die("poll");
        }

        for (auto it = pollfds.begin(); it != pollfds.end();) {
            if (it->revents == 0) { ++it; continue; }

            int fd = it->fd;

            /* 3.1 新连接 ------------------------------------------------ */
            if (fd == listenfd && (it->revents & POLLIN)) {
                sockaddr_in cli{};
                socklen_t   len = sizeof(cli);
                int connfd = accept4(listenfd, (sockaddr*)&cli, &len,
                    SOCK_NONBLOCK);
                if (connfd == -1) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK)
                        perror("accept4");
                    ++it; continue;
                }
                if (connfd >= FD_SETSIZE) {
                    printf("fd %d >= FD_SETSIZE, drop\n", connfd);
                    close(connfd);
                    ++it; continue;
                }

                conns[connfd].addr = cli;
                pollfds.push_back({ connfd, POLLIN | POLLRDHUP, 0 });
                printf("new client %d from %s:%d\n",
                    connfd,
                    inet_ntoa(cli.sin_addr),
                    ntohs(cli.sin_port));
                ++it;
                continue;
            }

            /* 3.2 对端关闭 ------------------------------------------------ */
            if (it->revents & (POLLRDHUP | POLLHUP | POLLERR)) {
                printf("client %d disconnected\n", fd);
                close(fd);
                conns[fd].wbuf.clear();
                it = pollfds.erase(it);
                continue;
            }

            /* 3.3 读数据 -------------------------------------------------- */
            if (it->revents & POLLIN) {
                Conn& c = conns[fd];
                ssize_t n = recv(fd, c.rbuf, sizeof(c.rbuf), 0);
                if (n <= 0) {            // 0 或错误
                    if (n == 0 || errno != EAGAIN)
                        it->revents |= POLLRDHUP; // 下一循环清理
                    ++it; continue;
                }
                c.rbytes = n;

                /* 广播：把数据塞进所有其他连接的 wbuf */
                std::string msg(c.rbuf, n);
                for (const auto& pfd : pollfds) {
                    if (pfd.fd == listenfd || pfd.fd == fd) continue;
                    Conn& dst = conns[pfd.fd];
                    dst.wbuf += msg;
                }
            }

            /* 3.4 写数据 -------------------------------------------------- */
            if (it->revents & POLLOUT) {
                Conn& c = conns[fd];
                if (!c.wbuf.empty()) {
                    ssize_t n = send(fd, c.wbuf.data(), c.wbuf.size(), 0);
                    if (n == -1) {           // 错误
                        if (errno != EAGAIN)
                            it->revents |= POLLERR;
                    }
                    else {
                        c.wbuf.erase(0, n);  // 移除已发部分
                    }
                }
                /* 如果全部写完，取消 POLLOUT */
                if (c.wbuf.empty())
                    it->events &= ~POLLOUT;
            }

            /* 3.5 如有数据待写，确保注册 POLLOUT */
            if (!conns[fd].wbuf.empty())
                it->events |= POLLOUT;

            ++it;
        }
    }

    delete[] conns;
    close(listenfd);
    return 0;
}