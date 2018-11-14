#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <syslog.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/errno.h>

#include <netinet/in.h>
#include <arpa/inet.h>



/* Settings ---------------------------------------------------------- */
#define DEFAULT_PORT 8080
#define DEFAULT_ROOT "/var/www"
#define BUF_SIZE 1024
#define USE_SYSLOG 0
/* ------------------------------------------------------------------- */



#define BACKLOG 5



extern char *__progname;

static uint16_t port = DEFAULT_PORT;
static const char* document_root = DEFAULT_ROOT;

static const char* response =
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: text/html\r\n"
    "\r\n"
    "<!DOCTYPE html>\r\n"
    "<html>"
    "<head>\r\n"
    "<title>it works</title>\r\n"
    "</head>\r\n"
    "<body>\r\n"
    "<p>it works</p>\r\n"
    "</body>\r\n"
    "</html>\r\n";



#if USE_SYSLOG
    #define SYSLOG(...) syslog(__VA_ARGS__)
#else
    #define SYSLOG(...)
#endif



void sigchld (int n);



int main(int argc, char *argv[])
{
    int c;
    int sockfd;
    int optval = 1;
    struct sockaddr_in sa_in;
    int connection_fd;
    struct sockaddr client_addr;
    pid_t pid;

    /* オプション解析 */
    while ((c = getopt(argc, argv, "p:d:")) != -1) {
        switch (c) {
        case 'p':
            port = (uint16_t)atoi(optarg);
            break;
        case 'd':
            document_root = optarg;
            break;
        default:
            exit(EXIT_FAILURE);
        }
    }
    if ((argc - optind) != 0) {
        /* オプション解析エラー */
        exit(EXIT_FAILURE);
    }

    /* Syslog */
#if USE_SYSLOG
    openlog(__progname, LOG_PID, LOG_DAEMON);
#endif

    /* ソケットを開く */
    sockfd = socket(PF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        SYSLOG(LOG_ERR, "socket: %m");
        perror("socket");
        exit(EXIT_FAILURE);
    }

    /*-
     * https://www.jpcert.or.jp/sc-rules/c-fio22-c.html
     * FD_CLOEXEC
     */
    if (fcntl(sockfd, F_SETFD, FD_CLOEXEC) < 0) {
        SYSLOG(LOG_ERR, "fcntl: %m");
        perror("fcntl");
        exit(EXIT_FAILURE);
    }

    /* TIME_WAITでも使えるように */
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (void *)&optval, sizeof(optval)) < 0) {
        SYSLOG(LOG_ERR, "setsockopt: %m");
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    /* バインド（IPv4） */
    memset(&sa_in, 0, sizeof(struct sockaddr_in));
    sa_in.sin_family = AF_INET;
    sa_in.sin_addr.s_addr = htonl(INADDR_ANY);
    sa_in.sin_port = htons(port);
    if (bind(sockfd, (struct sockaddr*)&sa_in, sizeof(struct sockaddr)) < 0) {
        SYSLOG(LOG_ERR, "bind: %m");
        perror("bind");
        exit(EXIT_FAILURE);
    }

    /* リッスンを始める */
    if (listen(sockfd, BACKLOG) < 0) {
        SYSLOG(LOG_ERR, "listen: %m");
        perror("listen");
        exit(EXIT_FAILURE);
    }

    /* Accept Filter */
// #ifdef SO_ACCEPTFILTER
//     {
//         struct accept_filter_arg afa;
//         bzero(&afa, sizeof(afa)); 
//         strcpy(afa.af_name, "httpready"); 
//         setsockopt(sockfd, SOL_SOCKET, SO_ACCEPTFILTER, &afa, sizeof(afa));
//     }
// #elif TCP_DEFER_ACCEPT
//     {
//         int optval = 1;
//         setsockopt(sockfd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &optval, sizeof(optval));
//     }
// #endif

    /* TODO: Daemonize */
    setsid();

    /*-
     * シグナルの設定
     * https://www.jpcert.or.jp/sc-rules/c-sig01-c.html 
     */
    {
        struct sigaction act;
        memset(&act, 0, sizeof(act));
        act.sa_handler = sigchld;
        if (sigemptyset(&act.sa_mask) < 0) {
            SYSLOG(LOG_ERR, "sigemptyset: %m");
            perror("sigemptyset");
            exit(EXIT_FAILURE);
        }
        act.sa_flags = SA_NOCLDSTOP | SA_RESTART;
        if (sigaction(SIGCHLD, &act, NULL) < 0) {
            SYSLOG(LOG_ERR, "sigaction: %m");
            perror("sigaction");
            exit(EXIT_FAILURE);
        }
    }

    for (;;) {
        socklen_t client_addr_size = sizeof(client_addr);
        connection_fd = accept(sockfd, &client_addr, &client_addr_size);
        if (connection_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED)
				continue;
                // EPROTO continue
            SYSLOG(LOG_ERR, "accept: %m");
			perror("accept");
			exit(1);
        }

        pid = fork();
        if (pid < 0) {
            SYSLOG(LOG_ERR, "fork: %m");
            perror("fork");
            exit(EXIT_FAILURE);
        }

        /* 子プロセスの処理 */
        if (pid == 0) {
            close(sockfd);

            /* Request */
            {
                char inet_addrstr[INET_ADDRSTRLEN];
                char buf[BUF_SIZE];

                if (inet_ntop(AF_INET, &((struct sockaddr_in *)&client_addr)->sin_addr, inet_addrstr, sizeof(inet_addrstr)) < 0) {
                    SYSLOG(LOG_WARNING, "inet_ntop: %m");
                    perror("inet_ntop");
                    puts("accessed from UNKNOWN HOST");
                } else {
                    printf("accessed from %s\n", inet_addrstr);
                }
                read(connection_fd, buf, sizeof(buf));
                write(connection_fd, response, strlen(response));
            }
            if (close(connection_fd) < 0) {
                SYSLOG(LOG_ERR, "close: %m");
                perror("close");
                exit(EXIT_FAILURE);
            }
            /* 子プロセスの終了 */
            exit(EXIT_SUCCESS);
        }
        close(connection_fd);
    }
    
    return 0;
}



/* SIGCHLDシグナルハンドラ */
void sigchld (int n)
{
    pid_t pid;
    int status;

    do {
        pid = waitpid((pid_t)-1, &status, WNOHANG);

        if (pid < 0) {
            if (errno == EINTR || errno == EAGAIN)
				continue;
            if (errno != ECHILD) {
                SYSLOG(LOG_ERR, "waitpid: %m");
			    perror("waitpid");
            }
            break;
        }
    } while (pid > 0);
}
