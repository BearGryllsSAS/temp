#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>

#include "../lock/locker.h"
#include "../mysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

typedef void (*call_back)(); 

char ms1[] = "与服务器建立连接, 开始进行数据通信 ------ [OK]\n"
             "          epoll服务器聊天室测试版          \n"
             "    (1)匿名聊天   (2)登陆   (3) 注册        \n"
             "========>";

class chat_conn
{
public:
     long m_read_idx;
     static const int BUFFER_SIZE = 2048;

public:
    chat_conn() {}
    ~chat_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    void close_conn(bool real_close = true);
    void process();
    bool read_once();
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    void initmysql_result(connection_pool *connPool);
    int timer_flag;
    int improv;


private:
    void init();

public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL *mysql;
    int m_state;  //读为0, 写为1

private:
    int m_sockfd;
    sockaddr_in m_address;

    map<string, string> m_users;
    int m_TRIGMode;
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];

public:
    int fd;                     // 监听的文件描述符
    int events;                 // 对应监听的事件 EPOLLIN / EPOLLOUT
    call_back fun;              // 回调函数
    void *arg;                  // 上面回调函数的参3
    int status;                 // 是否在监听红黑树上, 1 --- 在, 0 --- 不在
    char buf[BUFFER_SIZE];           // 读写缓冲区
    int len;                    // 本次从客户端读入缓冲区数据的长度
    long long last_active_time; // 该文件描述符最后在监听红黑树上的活跃时间
    int log_step;               // 标记用户位于登陆的操作 0-- 未登陆  1 --- 输入账号  2 ---- 输入密码   3----- 成功登陆  4 --- 注册用户名  5 ------ 输入注册的密码   6 ------- 再次输入密码验证

    char usr_id[8];            // 用户ID    五位 UID
    char usr_name[256];        // 用户名
    char usr_key[40];          // 用户密码
    int  st;                   // 是否在线 0---- 离线     1---- 在线

public:

    // 出错处理函数
    void sys_error(const char *str); 

    // 重新设置监听事件
    // void event_set(myevent_s *ev, int fd, int events, call_back fun, void *arg3);

    // 添加监听事件到树上
    // void event_add(int epfd, myevent_s *ev);

    // 将事件从监听红黑树上摘除
    // void event_del(int epfd, myevent_s *ev);

    // 关闭与客户端通信的文件描述符
    // void close_cfd(int cfd, myevent_s *ev);

    // 监听新的客户端建立连接
    // void cb_accept(int lfd, void * arg);

    // 登陆界面
    void login_menu();

    // 输入账号UID进行登陆
    void login();

    // 注册账号
    void register_id();

    // 获取一个未注册的uid
    void get_uid();

    // 写事件 ---> 向当前在线用户发送信息
    void cb_write();

    // 读事件 -----> 服务器接收的客户端发来的信息
    void cb_read();

    // 登出操作 ---> 必须是登陆上之后进行登出才调用
    void logout();

};

#endif
