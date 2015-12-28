#ifndef SRVMGR_H
#define SRVMGR_H

#include <map>
#include <arpa/inet.h>
#include "fdwrapper.h"
#include "conn.h"

using std::map;

class host
{
public:
    char m_hostname[1024];
    int m_port;
    int m_conncnt;
};

class mgr
{
public:
    mgr( int epollfd, const host& srv );
    ~mgr();
    int conn2srv( const sockaddr_in& address );
    conn* pick_conn( int sockfd );
    void free_conn( conn* connection );
    int get_used_conn_cnt();
    void recycle_conns();
    RET_CODE process( int fd, OP_TYPE type );

private:
    static int m_epollfd;
    map< int, conn* > m_conns;
    map< int, conn* > m_used;
    map< int, conn* > m_freed;
    host m_logic_srv;
};

#endif
