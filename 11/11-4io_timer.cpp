#define TIMEOUT 5000

int timeout = TIMEOUT;
time_t start = time( NULL );
time_t end = time( NULL );
while( 1 )
{
    printf( "the timeout is now %d mill-seconds\n", timeout );
    start = time( NULL );
    int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, timeout );
    if( ( number < 0 ) && ( errno != EINTR ) )
    {
        printf( "epoll failure\n" );
        break;
    }
    if( number == 0 )
    {
        // timeout
        timeout = TIMEOUT;
        continue;
    }

    end = time( NULL );
    timeout -= ( end - start ) * 1000;
    if( timeout <= 0 )
    {
        // timeout
        timeout = TIMEOUT;
    }

    // handle connections
}
