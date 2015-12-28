#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#define BUFFER_SIZE 1024
#define MAX_EVENT_NUMBER 1024
#define PROCESS_COUNT 5
#define USER_PER_PROCESS 65535

struct process_in_pool
{
    pid_t pid;
    int pipefd[2];
};

struct client_data
{
    sockaddr_in address;
    char buf[ BUFFER_SIZE ];
    int read_idx;
};

int sig_pipefd[2];
int epollfd;
int listenfd;
process_in_pool sub_process[ PROCESS_COUNT ];
bool stop_child = false;

int setnonblocking( int fd )
{
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}

void addfd( int epollfd, int fd )
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setnonblocking( fd );
}

void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send( sig_pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

void addsig( int sig, void(*handler)(int), bool restart = true )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    if( restart )
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

void del_resource()
{
    close( sig_pipefd[0] );
    close( sig_pipefd[1] );
    close( listenfd );
    close( epollfd );
}

void child_term_handler( int sig )
{
    stop_child = true;
}

void child_child_handler( int sig )
{
    pid_t pid;
    int stat;
    while ( ( pid = waitpid( -1, &stat, WNOHANG ) ) > 0 )
    {
        continue;
    }
}

int run_child( int idx )
{
    epoll_event events[ MAX_EVENT_NUMBER ];
    int child_epollfd = epoll_create( 5 );
    assert( child_epollfd != -1 );
    int pipefd = sub_process[idx].pipefd[1];
    addfd( child_epollfd, pipefd );
    int ret;
    addsig( SIGTERM, child_term_handler, false );
    addsig( SIGCHLD, child_child_handler );
    client_data* users = new client_data[ USER_PER_PROCESS ];

    while( !stop_child )
    {
        int number = epoll_wait( child_epollfd, events, MAX_EVENT_NUMBER, -1 );
        if ( ( number < 0 ) && ( errno != EINTR ) )
        {
            printf( "epoll failure\n" );
            break;
        }

        for ( int i = 0; i < number; i++ )
        {
            int sockfd = events[i].data.fd;
            if( ( sockfd == pipefd ) && ( events[i].events & EPOLLIN ) )
            {
                int client = 0;
                ret = recv( sockfd, ( char* )&client, sizeof( client ), 0 );
                if( ret < 0 )
                {
                    if( errno != EAGAIN )
                    {
                        stop_child = true;
                    }
                }
                else if( ret == 0 )
                {
                    stop_child = true;
                }
                else
                {
                    struct sockaddr_in client_address;
                    socklen_t client_addrlength = sizeof( client_address );
                    int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                    if ( connfd < 0 )
                    {
                        printf( "errno is: %d\n", errno );
                        continue;
                    }
                    memset( users[connfd].buf, '\0', BUFFER_SIZE );
                    users[connfd].address = client_address;
                    users[connfd].read_idx = 0;
                    addfd( child_epollfd, connfd );
                }
            }
            else if( events[i].events & EPOLLIN )
            {
                int idx = 0;
                while( true )
                {
                    idx = users[sockfd].read_idx;
                    ret = recv( sockfd, users[sockfd].buf + idx, BUFFER_SIZE-1-idx, 0 );
                    if( ret < 0 )
                    {
                        if( errno != EAGAIN )
                        {
                            epoll_ctl( child_epollfd, EPOLL_CTL_DEL, sockfd, 0 );
                            close( sockfd );
                        }
                        break;
                    }
                    else if( ret == 0 )
                    {
                        epoll_ctl( child_epollfd, EPOLL_CTL_DEL, sockfd, 0 );
                        close( sockfd );
                        break;
                    }
                    else
                    {
                        users[sockfd].read_idx += ret;
                        printf( "user content is: %s\n", users[sockfd].buf );
                        idx = users[sockfd].read_idx;
                        if( ( idx < 2 ) || ( users[sockfd].buf[idx-2] != '\r' ) || ( users[sockfd].buf[idx-1] != '\n' ) )
                        {
                            continue;
                        }
                        users[sockfd].buf[users[sockfd].read_idx-2] = '\0';
                        char* file_name = users[sockfd].buf;
                        if( access( file_name, F_OK ) == -1 )
                        {
                            epoll_ctl( child_epollfd, EPOLL_CTL_DEL, sockfd, 0 );
                            close( sockfd );
                            break;
                        }
                        ret = fork();
                        if( ret == -1 )
                        {
                            epoll_ctl( child_epollfd, EPOLL_CTL_DEL, sockfd, 0 );
                            close( sockfd );
                            break;
                        }
                        else if( ret > 0 )
                        {
                            epoll_ctl( child_epollfd, EPOLL_CTL_DEL, sockfd, 0 );
                            close( sockfd );
                            break;
                        }
                        else
                        {
                            close( STDOUT_FILENO );
                            dup( sockfd );
                            execl( users[sockfd].buf, users[sockfd].buf, 0 );
                            exit( 0 );
                        }
                    }
                }
            }
            else
            {
                continue;
            }
        }
    }

    delete [] users;
    close( pipefd );
    close( child_epollfd );
    return 0;
}

int main( int argc, char* argv[] )
{
    if( argc <= 2 )
    {
        printf( "usage: %s ip_address port_number\n", basename( argv[0] ) );
        return 1;
    }
    const char* ip = argv[1];
    int port = atoi( argv[2] );

    int ret = 0;
    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET;
    inet_pton( AF_INET, ip, &address.sin_addr );
    address.sin_port = htons( port );

    listenfd = socket( PF_INET, SOCK_STREAM, 0 );
    assert( listenfd >= 0 );

    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    assert( ret != -1 );

    ret = listen( listenfd, 5 );
    assert( ret != -1 );

    for( int i = 0; i < PROCESS_COUNT; ++i )
    {
        ret = socketpair( PF_UNIX, SOCK_STREAM, 0, sub_process[i].pipefd );
        assert( ret != -1 );
        sub_process[i].pid = fork();
        if( sub_process[i].pid < 0 )
        {
            continue;
        }
        else if( sub_process[i].pid > 0 )
        {
            close( sub_process[i].pipefd[1] );
            setnonblocking( sub_process[i].pipefd[0] );
            continue;
        }
        else
        {
            close( sub_process[i].pipefd[0] );
            setnonblocking( sub_process[i].pipefd[1] );
            run_child( i );
            exit( 0 );
        }
    }

    epoll_event events[ MAX_EVENT_NUMBER ];
    epollfd = epoll_create( 5 );
    assert( epollfd != -1 );
    addfd( epollfd, listenfd );

    ret = socketpair( PF_UNIX, SOCK_STREAM, 0, sig_pipefd );
    assert( ret != -1 );
    setnonblocking( sig_pipefd[1] );
    addfd( epollfd, sig_pipefd[0] );

    addsig( SIGCHLD, sig_handler );
    addsig( SIGTERM, sig_handler );
    addsig( SIGINT, sig_handler );
    addsig( SIGPIPE, SIG_IGN );
    bool stop_server = false;
    int sub_process_counter = 0;

    while( !stop_server )
    {
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        if ( ( number < 0 ) && ( errno != EINTR ) )
        {
            printf( "epoll failure\n" );
            break;
        }

        for ( int i = 0; i < number; i++ )
        {
            int sockfd = events[i].data.fd;
            if( sockfd == listenfd )
            {
                int new_conn = 1;
                send( sub_process[sub_process_counter++].pipefd[0], ( char* )&new_conn, sizeof( new_conn ), 0 );
                printf( "send request to child %d\n", sub_process_counter-1 );
                sub_process_counter %= PROCESS_COUNT;
            }
            else if( ( sockfd == sig_pipefd[0] ) && ( events[i].events & EPOLLIN ) )
            {
                int sig;
                char signals[1024];
                ret = recv( sig_pipefd[0], signals, sizeof( signals ), 0 );
                if( ret == -1 )
                {
                    continue;
                }
                else if( ret == 0 )
                {
                    continue;
                }
                else
                {
                    for( int i = 0; i < ret; ++i )
                    {
                        switch( signals[i] )
                        {
                            case SIGCHLD:
                            {
	                        pid_t pid;
	                        int stat;
	                        while ( ( pid = waitpid( -1, &stat, WNOHANG ) ) > 0 )
                                {
                                    for( int i = 0; i < PROCESS_COUNT; ++i )
                                    {
                                        if( sub_process[i].pid == pid )
                                        {
                                            close( sub_process[i].pipefd[0] );
                                            sub_process[i].pid = -1;
                                        }
                                    }
                                }
                                stop_server = true;
                                for( int i = 0; i < PROCESS_COUNT; ++i )
                                {
                                    if( sub_process[i].pid != -1 )
                                    {
                                        stop_server = false;
                                    }
                                }
                                break;
                            }
                            case SIGTERM:
                            case SIGINT:
                            {
                                printf( "kill all the clild now\n" );
                                for( int i = 0; i < PROCESS_COUNT; ++i )
                                {
                                    int pid = sub_process[i].pid;
                                    kill( pid, SIGTERM );
                                }
                                break;
                            }
                            default:
                            {
                                break;
                            }
                        }
                    }
                }
            }
            else 
            {
                continue;
            }
        }
    }

    del_resource();
    return 0;
}
