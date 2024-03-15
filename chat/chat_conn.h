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
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

typedef void (*call_back)(int, void*); 

// 登陆用户信息
typedef struct user
{
    char usr_id[8];            // 用户ID    五位 UID
    char usr_name[256];        // 用户名
    char usr_key[40];          // 用户密码
    int  st;                   // 是否在线 0---- 离线     1---- 在线
}

user_msg Users[MAX_EVENTS];    // 已注册的所有用户信息
int user_num;                  // 已注册用户信息的数量

//描述监听的文件描述符的相关信息的结构体    
typedef struct myevent_s       
{
    int fd;                     // 监听的文件描述符
    int events;                 // 对应监听的事件 EPOLLIN / EPOLLOUT
    call_back fun;              // 回调函数
    void *arg;                  // 上面回调函数的参3
    int status;                 // 是否在监听红黑树上, 1 --- 在, 0 --- 不在
    char buf[BUFSIZ];           // 读写缓冲区
    int len;                    // 本次从客户端读入缓冲区数据的长度
    long long last_active_time; // 该文件描述符最后在监听红黑树上的活跃时间
    user_msg um;                // 用户登陆的信息
    int log_step;               // 标记用户位于登陆的操作 0-- 未登陆  1 --- 输入账号  2 ---- 输入密码   3----- 成功登陆  4 --- 注册用户名  5 ------ 输入注册的密码   6 ------- 再次输入密码验证
}myevent_s;

int g_efd;                           // 监听红黑树的树根
myevent_s g_events[MAX_EVENTS + 1];  // 用于保存每个文件描述符信息的结构体的数组

// 函数声明
void cb_read(int,  void*);                // 服务器端读事件
void cb_write(int,  void*);               // 向在线用户发送数据写事件
void logout(int cfd, void *);             // 用户登出事件
void login_menu(int cfd , void* arg);     // 登陆界面读事件
void login(int, void*);                   // 输入账号登陆 
void register_id(int, void*);             // 注册新账号
void get_uid(myevent_s*);                 // 获取一个未注册的UID
void load_usermsg();                      // 从文件加载已经注册过的用户信息


char ms1[] = "与服务器建立连接, 开始进行数据通信 ------ [OK]\n"
             "          epoll服务器聊天室测试版          \n"
             "    (1)匿名聊天   (2)登陆   (3) 注册        \n"
             ">>> ";

//========================================================================================//

class chat_conn
{
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    enum chat_conn

    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        chat_conn
    
    };
    enum HTTP_CODE
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

public:
    chat_conn() {}
    ~chat_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    void close_conn(bool real_close = true);
    void process();
    bool read_once();
    bool write();
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    void initmysql_result(connection_pool *connPool);
    int timer_flag;
    int improv;


private:
    void init();
    HTTP_CODE process_read();
    bool process_write(HTTP_CODE ret);
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    HTTP_CODE do_request();
    char *get_line() { return m_read_buf + m_start_line; };
    LINE_STATUS parse_line();
    void unmap();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL *mysql;
    int m_state;  //读为0, 写为1

private:
    int m_sockfd;
    sockaddr_in m_address;
    char m_read_buf[READ_BUFFER_SIZE];
    long m_read_idx;
    long m_checked_idx;
    int m_start_line;
    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_write_idx;
    chat_conn
    m_check_state;
    METHOD m_method;
    char m_real_file[FILENAME_LEN];
    char *m_url;
    char *m_version;
    char *m_host;
    long m_content_length;
    bool m_linger;
    char *m_file_address;
    struct stat m_file_stat;
    struct iovec m_iv[2];
    int m_iv_count;
    int cgi;        //是否启用的POST
    char *m_string; //存储请求头数据
    int bytes_to_send;
    int bytes_have_send;
    char *doc_root;

    map<string, string> m_users;
    int m_TRIGMode;
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];

//==============================================================================//

public:
    int fd;                     // 监听的文件描述符
    int events;                 // 对应监听的事件 EPOLLIN / EPOLLOUT
    call_back fun;              // 回调函数
    void *arg;                  // 上面回调函数的参3
    int status;                 // 是否在监听红黑树上, 1 --- 在, 0 --- 不在
    char buf[BUFSIZ];           // 读写缓冲区
    int len;                    // 本次从客户端读入缓冲区数据的长度
    long long last_active_time; // 该文件描述符最后在监听红黑树上的活跃时间
    user_msg um;                // 用户登陆的信息
    int log_step;               // 标记用户位于登陆的操作 0-- 未登陆  1 --- 输入账号  2 ---- 输入密码   3----- 成功登陆  4 --- 注册用户名  5 ------ 输入注册的密码   6 ------- 再次输入密码验证

    char usr_id[8];            // 用户ID    五位 UID
    char usr_name[256];        // 用户名
    char usr_key[40];          // 用户密码
    int  st;                   // 是否在线 0---- 离线     1---- 在线

public:

    // 出错处理函数
    void sys_error(const char *str) 

    // 重新设置监听事件
    void event_set(myevent_s *ev, int fd, int events, call_back fun, void *arg3);

    // 添加监听事件到树上
    void event_add(int epfd, myevent_s *ev);

    // 将事件从监听红黑树上摘除
    void event_del(int epfd, myevent_s *ev);

    // 关闭与客户端通信的文件描述符
    void close_cfd(int cfd, myevent_s *ev);

    // 监听新的客户端建立连接
    void cb_accept(int lfd, void * arg);

    // 登陆界面
    void login_menu(int cfd , void* arg);

    // 输入账号UID进行登陆
    void login(int cfd, void *arg);

    // 注册账号
    void register_id(int cfd, void *arg);

    // 获取一个未注册的uid
    void get_uid(myevent_s *ev);

    // 写事件 ---> 向当前在线用户发送信息
    void cb_write(int cfd, void *arg);

    // 读事件 -----> 服务器接收的客户端发来的信息
    void cb_read(int cfd, void *arg);

    // 登出操作 ---> 必须是登陆上之后进行登出才调用
    void logout(int cfd, void *arg);

};

#endif
