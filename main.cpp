#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量

// 添加文件描述符到epoll对象中
extern void addfd( int epollfd, int fd, bool one_shot );
// 从epoll删除文件描述符
extern void removefd( int epollfd, int fd );

// 添加信号捕捉，用于信号处理
void addsig(int sig, void( handler )(int)){
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    sigfillset( &sa.sa_mask ); //设置临时阻塞的信号集
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

int main( int argc, char* argv[] ) {
    
    if( argc <= 1 ) {
        printf( "usage: %s port_number\n", basename(argv[0]));
        return 1;
    }

    // 获取端口号
    int port = atoi( argv[1] );

    // 对sig信号进行处理->忽略
    addsig( SIGPIPE, SIG_IGN );

    // 初始化线程池
    threadpool< http_conn >* pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch( ... ) {
        return 1;
    }

    // 创建一个数组用于保存所有的客户端信息
    http_conn* users = new http_conn[ MAX_FD ];

    // 创建监听的socket
    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );


    int ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons( port );

    // 端口复用
    int reuse = 1;
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    // 绑定
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    // listen
    ret = listen( listenfd, 5 );

    // 创建epoll对象，和事件数组，添加
    epoll_event events[ MAX_EVENT_NUMBER ]; //有数据改变的events集合
    int epollfd = epoll_create( 5 ); //5没有意义，只要大于0就可以

    // 将监听的文件描述符添加到epoll对象中
    addfd( epollfd, listenfd, false ); //在httpconn中设置了边沿触发
    http_conn::m_epollfd = epollfd; //  所有socket上的事件都被注册到同一个epoll内核事件中

    while(true) { //主线程不断循环检测是否有事件发生
        
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 ); //检测到了几个事件
        
        if ( ( number < 0 ) && ( errno != EINTR ) ) { //调用epoll失败
            printf( "epoll failure\n" );
            break;
        }

        // 循环遍历epoll数组
        for ( int i = 0; i < number; i++ ) {
            
            int sockfd = events[i].data.fd;
            
            if( sockfd == listenfd ) { //是监听的文件描述符，有客户端连接进来
                
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength ); // 连接
                
                if ( connfd < 0 ) {
                    printf( "errno is: %d\n", errno );
                    continue;
                } 

                if( http_conn::m_user_count >= MAX_FD ) { // 目前的连接数满了
                    close(connfd);
                    continue;
                }
                users[connfd].init( connfd, client_address); //将新的客户数据初始化，放到数组中,把connfd添加到epoll中

            } else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) { //对方异常断开或者错误事件等

                users[sockfd].close_conn();

            } else if(events[i].events & EPOLLIN) { //不是监听的文件描述符，有读的事件发生

                if(users[sockfd].read()) { //一次性把数据读完
                    pool->append(users + sockfd); // 交给工作线程，线程池，线程池执行run
                } else { // 读失败
                    users[sockfd].close_conn();
                }

            }  else if( events[i].events & EPOLLOUT ) { //写事件

                if( !users[sockfd].write() ) { // 一次性写完所有数据
                    users[sockfd].close_conn();
                }

            }
        }
    }
    
    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}