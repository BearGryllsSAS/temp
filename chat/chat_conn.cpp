#include "chat_conn.h"
#include "../server.h"

#include <vector>
#include <unordered_set>
#include <mysql/mysql.h>
#include <fstream>

//服务器读取缓冲区的数据的判断统一在外面进行处理，因为是在外面读的。并且要根据客户端的状态进行相应的处理。

//服务器发送数据的len的判断也是在外面进行处理。同上

locker m_lock;
map<string, pair<string, string>> users;  //通过id映射用户名和密码
vector<int> onlineUsers;    //记录在线用户，存放的是cfd
unordered_set<int> onlineUsersId   //记录在线用户，存放的是用户id。方便login函数判断

int chat_conn::m_user_count = 0;
int chat_conn::m_epollfd = -1;

//初始化连接
void chat_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    //到时候改成user_id passeword
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数 目前来看好像没用
    //int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组 目前来看好像没用
    //MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        string temp3(row[2]);
        users[temp1] = {temp2, temp3};
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    
    this->status = 1        //是否在监听红黑树上

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

//关闭连接，关闭一个连接，客户总量减一
void chat_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);

        this->status = 0;        //是否在监听红黑树上

        // 原来的打印错误信息的逻辑，以后更改为日志操作
        // char str[BUFSIZ];
        // event_del(g_efd, ev);
        // close(cfd);
        // sprintf(str, "the client fd: %d is close\n", ev->fd);
        // write(STDOUT_FILENO, str, strlen(str));
        
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void chat_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    // 设置回调函数 --- 登录界面
    fun = login_menu;

    // 添加监听事件
    addfd(m_epollfd, sockfd, true, m_TRIGMode);    //这里有点问题？？？最后一个参数还初始化呢
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();

    // 添加日志 -- 待定


    // 发送信息提示用户
    write(cfd, ms1, sizeof ms1);
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void chat_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool chat_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, buf + m_read_idx, BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, buf + m_read_idx, BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

// 出错处理函数
// 以后这些都会做一个封装，做成动态库
void sys_error(const char *str) 
{
    perror(str);
    exit;
}


//==========================================================================================//


//========================== 当前在线用户这里出了点小问题,先不管
// 登陆界面
void chat_conn::login_menu()
{
    if(this->buf[0] == '1')           // 匿名用户登陆
    {
        sprintf(this->usr_name, "匿名用户 %ld", time(NULL)) ;    // 设置匿名登陆的用户名
        strcpy(this->uusr_id, "00000");                           // 所有匿名用户的账号为00000
        // 加入到聊天回调然后监听 
        this->log_step = 3;       // 表示为已登录状态

        // list_push(cfd);         // 加入当前的在线列表
        onlineUsers.emplace_back(this->fd);
        chat_conn:::m_user_count++;        // 在线人数加一

        sprintf(this->buf,">               用户: %s  已登录,当前在线人数为 %d          \n\n>>>", this->usr_name, chat_conn:::m_user_count);
        this->len = strlen(this->buf);
        char s[] = "----------------------epoll聊天室测试版--------------------\n";
        write(this->fd, s, sizeof s);
        write(this->fd, this->buf, this->len);

        // 重新设定监听事件为写, 写的内容为向当前在线用户发送XXX已登录的信息
        // event_del(g_efd, ev);
        // event_set(ev, cfd, EPOLLOUT , cb_write, ev);
        // event_add(g_efd, ev);

        this->fun = cb_write;
        modfd(chat_conn::m_epollfd, this->fd, EPOLLOUT, this->m_TRIGMode);
    }
    else if(this->buf[0] == '2')          // 账号UID登陆
    {
        this->log_step = 1;
        strcpy(this->buf, "请输入登陆的UID:");
        write(this->fd, this->buf, strlen(this->buf));

        // 将事件设定为登陆的回调 
        // event_del();
        // event_set(ev, cfd, EPOLLIN | EPOLLET , login, ev);
        // event_add(g_efd, ev);

        this->fun = login;
        modfd(chat_conn::m_epollfd, this->fd, EPOLLIN, this->m_TRIGMode);
    }
    else   // 注册
    {
        strcpy(this->buf, "注册账号\n###请输入注册的用户名(中文/英文, 注意不要包含特殊符号): ");
        write(this->fd, this->buf, strlen(this->buf));
        this->log_step = 4;           // 标记为进行注册状态的准备输入注册的用户名

        // 将事件监听设置为注册的回调
        // event_del(g_efd, ev);
        // event_set(ev, cfd, EPOLLIN | EPOLLET, register_id, ev);
        // event_add(g_efd, ev);

        this->fun = register_id;
        modfd(chat_conn::m_epollfd, this->fd, EPOLLIN, this->m_TRIGMode);
    }

}

// 输入账号UID进行登陆
void chat_conn::login()
{
    if(1 == this->log_step)     // 读取用户输入用户名
    {
        int id = atoi(this->buf);
        strcpy(this->usr_id, this->buf);
        char s[100];
        if(id > chat_conn::m_user_count || id <= 0)      
        {
            sprintf(s, "!用户UID:%s 不存在\n请重新输入账号UID:", this->buf);
            write(this->fd, s, strlen(s));
            return;
        }
        if(onlineUsersId.contains(id))                      
        {
            sprintf(s, "!用户UID:%s 已登陆\n请重新输入账号UID:", this->buf);
            write(this->fd, s, strlen(s));
            return;
        }
        strcpy(this->buf, "请输入密码:");
        write(this->fd, this->buf, strlen(this->buf));

    }
    else if(2 == this->log_step)  // 输入用户密码
    {
        int id = atoi(this->usr_id);
        strcpy(this->usr_key, buf);
        if(!strcmp(this->buf, users[id].second))
        {
            strcpy(this->usr_name, users[id].first);  //数据库中还是得记录用户名，并且初始化的时候还是得把用户名读出来

            // list_push(cfd);                                 // 将当前的cfd添加进在线列表中
            onlineUsers.emplace_back(this->fd);         

            // Users[id].st = 1;
            onlineUsersId.insert(this->user_id);

            //在线人数加一
            chat_conn:::m_user_count++;
            
            sprintf(this->buf,">               用户: %s  已登录,当前在线人数为 %d          \n\n>>>", this->usr_name, chat_conn:::m_user_count);
            this->len = strlen(this->buf);
            char s[] = "----------------------epoll聊天室测试版--------------------\n";
            write(this->fd, s, sizeof s);
            write(this->fd, this->buf, this->len);

            // 设定为写事件, 向当前在线用户发送XXX用户已登陆的信息
            // event_del(g_efd, ev);
            // event_set(ev, cfd, EPOLLOUT , cb_write, ev);
            // event_add(g_efd, ev);

            this->fun = cb_write;
            modfd(chat_conn::m_epollfd, this->fd, EPOLLOUT, this->m_TRIGMode);
        }
        else
        {
            strcpy(this->buf, "密码错误, 请重新输入密码:");
            write(this->fd, this->buf, strlen(this->buf));
            return;
        }

    }
    this->log_step++;
}

// 注册账号
void chat_conn::register_id()
{
    if(4 == this->log_step)     // 输入注册的用户名
    {
        strcpy(this->usr_name, this->buf);
        strcpy(this->buf, "请设定账号的密码: ");
        write(this->fd, this->buf, strlen(this->buf));
    }
    else if(5 == this->log_step)  // 输入用户密码
    {
        strcpy(this->usr_key, this->buf);
        strcpy(this->buf, "请再次输入密码: ");
        write(this->fd, this->buf, strlen(this->buf));
    }
    else if(6 == this->log_step)  // 验证两次用户的密码
    {
        if(strcmp(this->usr_key, this->buf))
        { 
            strcpy(this->buf, "两次密码输入不一致, 请重新输入:");
            write(this->fd, this->buf, strlen(this->buf));
            return;
        }
        this->get_uid();
        sprintf(this->buf, "注册成功, 你的账号uid: %s  用户名为%s, 现在重新返回登陆界面 \n\n", this->usr_id,this->usr_name);
  
        // strcpy(Users[chat_conn:::m_user_count].usr_id, this->usr_id);          
        // strcpy(Users[chat_conn:::m_user_count].usr_name, this->usr_name);
        // strcpy(Users[chat_conn:::m_user_count].usr_key, this->usr_key);
       
        this->log_step = 0;
        write(this->fd, this->buf, strlen(this->buf));
        write(this->fd, ms1, sizeof ms1);

        // 注册完账号, 重新返回登陆界面的程序进行监听
        // event_del(g_efd, ev);
        // event_set(ev, cfd, EPOLLIN | EPOLLET, login_menu, ev);
        // event_add(g_efd, ev);

        this->fun = login_menu;
        modfd(chat_conn::m_epollfd, this->fd, EPOLLIN, this->m_TRIGMode);

        return;
    }
    this->log_step++;
}

// 获取一个未注册的uid
void chat_conn::get_uid()
{
    char str[10];
    sprintf(str, "%05d", chat_conn:::m_user_count + 1);   
    strcpy(this->usr_id, str); 

    //将注册的用户信息加入users中 将注册的用户信息加入数据库中
    char *sql_insert = (char *)malloc(sizeof(char) * 200);
    strcpy(sql_insert, "INSERT INTO user(userid, username, passwd) VALUES(");
    strcat(sql_insert, "'");
    strcat(sql_insert, this->user_id);
    strcat(sql_insert, "', '");
    strcat(sql_insert, this->user_name);
    strcat(sql_insert, "', '");
    strcat(sql_insert, this_user_key);
    strcat(sql_insert, "')");
   
    m_lock.lock();
    int res = mysql_query(mysql, sql_insert);
    users[this->user_id] = {this->user_name, this_user_key};
    m_lock.unlock();
}

// 写事件 ---> 向当前在线用户发送信息
void chat_conn::lcb_write()
{
    if(this->len <= 0) 
    {
        logout(this->fd, ev);
        close_cfd(this->fd, ev);
        return;
    }
    for(const auto& onlinefd : onlineUsers)             // 遍历当前的在线链表, 向在线用户发送
    {
        if(onlinefd == this->fd) continue;           // 发送数据给服务器的客户端一方并不需要发送
        write(onlinefd, this->buf, this->len);
    }

    if(this->log_step == 3) write(this->fd, "\n>>>", 4);   // 界面的优化,与主要逻辑无关 

    // 执行完一次事件之后--> 从树上摘下 ---> 重新设定要监听事件 ---> 重新挂上树监听
    // event_del(g_efd, ev);
    // event_set(ev, cfd, EPOLLIN | EPOLLET, cb_read, ev);
    // event_add(g_efd, ev);

    this->fun = cb_read;
    modfd(chat_conn::m_epollfd, this->fd, EPOLLIN, this->m_TRIGMode);
}

// 读事件 -----> 服务器接收的客户端发来的信息
void chat_conn::cb_read()
{
    char str[BUFSIZ], str2[1024];
    // int ret = read(cfd, str, sizeof str);
    if(this->len <= 0)    //这种判断应该是在读数据中做的
    {
        logout(this->fd, ev);
        close_cfd(this->fd, ev);
        return;
    }
    //str[ret] = '\0';

    strcpy(str, this->buf); 
    
    //不确定是否否要用
    memset(this->buf, '\0', sizeof(this->buf));
    
    sprintf(str2, "from client fd: %d receive data is :", cfd);
    //这里好像有点错误
    //if(ret > 0)  write(STDOUT_FILENO, str2, strlen(str2));
    write(STDOUT_FILENO, str, ret);    // 将客户端发来的数据在服务器端进行打印

    sprintf(this->buf, "(%s):%s\n>>>", this->usr_name, str);   // 格式化客户端发来的数据 --- 数据处理
    this->len = strlen(this->buf);

    // 此时服务器端接收客户端发来的数据并进行数据,然后发送给其他的在线用户,故此时事件要重新设定为写事件
    // event_del(g_efd, ev);
    // event_set(ev, cfd, EPOLLOUT, cb_write, ev);
    // event_add(g_efd, ev);

    this->fun = cb_write;
    modfd(chat_conn::m_epollfd, this->fd, EPOLLOUT, this->m_TRIGMode);
}

// 登出操作 ---> 必须是登陆上之后进行登出才调用
void chat_conn::logout(int cfd, void *arg)
{
    myevent_s *ev = (myevent_s*)arg;         
    char str[1024];                         
    list_del(cfd);                     // 从在线列表中删除
    ev->log_step = 0;                  // 标记为登出
    Users[atoi(ev->um.usr_id)].st = 0; // 用户信息中将其标记为离线状态

    sprintf(str, "已退出聊天室, 当前在线人数为%d\n", online_num);
    sprintf(ev->buf, "(%s) %s\n>>>", ev->um.usr_name, str);
    ev->len = strlen(ev->buf);
    cb_write(cfd, ev);                  // 手动调用向其他用户发送XXX用户登出的信息
}



void chat_conn::process()
{
    /*
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
    */

    // 调用回调函数
    this->fun();
}