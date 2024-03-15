#include "chat_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;

//初始化连接
void chat_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
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

int chat_conn::m_user_count = 0;
int chat_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void chat_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
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
    addfd(m_epollfd, sockfd, true, m_TRIGMode);
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

// 出错处理函数
void sys_error(const char *str) 
{
    perror(str);
    exit(1);
}

// 重新设置监听事件
void chat_conn::event_set(call_back fun)
{
    this->fun = fun;
}

// 添加监听事件到树上
void chat_conn::event_add(int epfd, myevent_s *ev)
{
    struct epoll_event tep;
    tep.data.ptr = ev;
    tep.events = ev->events;
    if(epoll_ctl(epfd, EPOLL_CTL_ADD, ev->fd, &tep) == -1)
        printf("fail: epoll_ctl add fd: %d, events is %d\n",ev->fd, ev->events);
    else
        ev->status = 1, ev->last_active_time = time(NULL);
}

// 将事件从监听红黑树上摘除
void chat_conn::event_del()
{
    this->status = 0;
    epoll_ctl(this->m_epollfd, EPOLL_CTL_DEL, this->fd, NULL);
}

// 关闭与客户端通信的文件描述符
void chat_conn::close_cfd(int cfd, myevent_s *ev)
{
    char str[BUFSIZ];
    event_del(g_efd, ev);
    close(cfd);
    sprintf(str, "the client fd: %d is close\n", ev->fd);
    write(STDOUT_FILENO, str, strlen(str));
    return;
}

/*  用不到了
// 监听新的客户端建立连接
void chat_conn::cb_accept(int lfd, void * arg)
{
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof client_addr;
    int cfd = accept(lfd, (struct sockaddr*)&client_addr, &client_addr_len);
    if(cfd == -1) sys_error("accept error");
    int i = 0;
    for(i = 0; i < MAX_EVENTS && g_events[i].status != 0; ++i);
    if(i == MAX_EVENTS) 
    {
        printf("the client num is max\n");
        return;
    }
    struct myevent_s *ev = &g_events[i];
    int flag = fcntl(cfd, F_GETFL);             // 设置为非阻塞状态
    flag |= O_NONBLOCK;
    fcntl(cfd, F_SETFL, flag);
    event_set(ev, cfd, EPOLLIN | EPOLLET, login_menu, ev);      // 建立新连接后,事件设定为登陆界面程序
    event_add(g_efd, ev);
    printf("the new client ip is %s, the client port is %d\n",  // 服务器端打印客户端的地址信息 
           inet_ntop(AF_INET, &client_addr.sin_addr.s_addr, ev->buf, sizeof ev->buf),
           ntohs(client_addr.sin_port));
    write(cfd, ms1, sizeof ms1);
}
*/

// 登陆界面
void chat_conn::login_menu()
{
    int cfd = this->fd;
    char *buf = this->buf;
    int ret = read(cfd, buf, BUFSIZ);
    if(ret <= 0)
    {
        close_cfd(cfd, ev);
        return;
    }
    if(buf[0] == '1')           // 匿名用户登陆
    {
        sprintf(this->usr_name, "匿名用户 %ld", time(NULL)) ;    // 设置匿名登陆的用户名
        strcpy(this->uusr_id, "00000");                           // 所有匿名用户的账号为00000
        // 加入到聊天回调然后监听 
        this->log_step = 3;       // 表示为已登录状态
        list_push(cfd);         // 加入当前的在线列表
        sprintf(buf,">               用户: %s  已登录,当前在线人数为 %d          \n\n>>>", ev->um.usr_name, online_num);
        this->len = strlen(buf);
        char s[] = "----------------------epoll聊天室测试版--------------------\n";
        write(cfd, s, sizeof s);
        write(cfd, buf, ev->len);

        // 重新设定监听事件为写, 写的内容为向当前在线用户发送XXX已登录的信息
        event_del(g_efd, ev);
        event_set(ev, cfd, EPOLLOUT , cb_write, ev);
        event_add(g_efd, ev);
    }
    else if(buf[0] == '2')          // 账号UID登陆
    {
        ev->log_step = 1;
        strcpy(buf, "请输入登陆的UID:");
        write(cfd, buf, strlen(buf));

        // 将事件设定为登陆的回调 
        event_del();
        event_set(ev, cfd, EPOLLIN | EPOLLET , login, ev);
        event_add(g_efd, ev);
    }
    else   // 注册
    {
        strcpy(buf, "注册账号\n###请输入注册的用户名(中文/英文, 注意不要包含特殊符号): ");
        write(cfd, buf, strlen(buf));
        ev->log_step = 4;           // 标记为进行注册状态的准备输入注册的用户名

        // 将事件监听设置为注册的回调
        event_del(g_efd, ev);
        event_set(ev, cfd, EPOLLIN | EPOLLET, register_id, ev);
        event_add(g_efd, ev);
    }

}

// 输入账号UID进行登陆
void chat_conn::login(int cfd, void *arg)
{
    myevent_s *ev = (myevent_s*) arg;
    char *buf = ev->buf;
    int ret = read(cfd, buf, BUFSIZ);
    if(ret <= 0) 
    {
        close_cfd(cfd, ev);
        return;
    }
    buf[ret - 1] = '\0';

    if(1 == ev->log_step)     // 读取用户输入用户名
    {
        int id = atoi(buf);
        strcpy(ev->um.usr_id, buf);
        char s[100];
        if(id > user_num || id <= 0)      
        {
            sprintf(s, "!用户UID:%s 不存在\n请重新输入账号UID:", buf);
            write(cfd, s, strlen(s));
            return;
        }
        if(Users[id].st)
        {
            sprintf(s, "!用户UID:%s 已登陆\n请重新输入账号UID:", buf);
            write(cfd, s, strlen(s));
            return;
        }
        strcpy(buf, "请输入密码:");
        write(cfd, buf, strlen(buf));

    }
    else if(2 == ev->log_step)  // 输入用户密码
    {
        int id = atoi(ev->um.usr_id);
        strcpy(ev->um.usr_key, buf);
        if(!strcmp(buf, Users[id].usr_key))
        {
            strcpy(ev->um.usr_name, Users[id].usr_name);
            list_push(cfd);                                 // 将当前的cfd添加进在线列表中
            Users[id].st = 1;
            sprintf(buf,">               用户: %s  已登录,当前在线人数为 %d          \n\n>>>", ev->um.usr_name, online_num);
            ev->len = strlen(buf);
            char s[] = "----------------------epoll聊天室测试版--------------------\n";
            write(cfd, s, sizeof s);
            write(cfd, buf, ev->len);

            // 设定为写事件, 向当前在线用户发送XXX用户已登陆的信息
            event_del(g_efd, ev);
            event_set(ev, cfd, EPOLLOUT , cb_write, ev);
            event_add(g_efd, ev);
        }
        else
        {
            strcpy(buf, "密码错误, 请重新输入密码:");
            write(cfd, buf, strlen(buf));
            return;
        }

    }
    ev->log_step++;
}

// 注册账号
void chat_conn::register_id(int cfd, void *arg)
{
    myevent_s *ev = (myevent_s*) arg;
    char *buf = ev->buf;

    int ret = read(cfd, buf, BUFSIZ);
    if(ret <= 0) 
    {
        close_cfd(cfd, ev);
        return;
    }
    buf[ret - 1] = '\0';

    if(4 == ev->log_step)     // 输入注册的用户名
    {
        strcpy(ev->um.usr_name, buf);
        strcpy(buf, "请设定账号的密码: ");
        write(cfd, buf, strlen(buf));
    }
    else if(5 == ev->log_step)  // 输入用户密码
    {
        strcpy(ev->um.usr_key, buf);
        strcpy(buf, "请再次输入密码: ");
        write(cfd, buf, strlen(buf));
    }
    else if(6 == ev->log_step)  // 验证两次用户的密码
    {
        if(strcmp(ev->um.usr_key, buf))
        { 
            strcpy(buf, "两次密码输入不一致, 请重新输入:");
            write(cfd, buf, strlen(buf));
            return;
        }
        get_uid(ev);
        sprintf(buf, "注册成功, 你的账号uid: %s  用户名为%s, 现在重新返回登陆界面 \n\n", ev->um.usr_id,ev->um.usr_name);
        user_num++;
        strcpy(Users[user_num].usr_id, ev->um.usr_id);
        strcpy(Users[user_num].usr_name, ev->um.usr_name);
        strcpy(Users[user_num].usr_key, ev->um.usr_key);
        ev->log_step = 0;
        write(cfd, buf, strlen(buf));
        write(cfd, ms1, sizeof ms1);

        // 注册完账号, 重新返回登陆界面的程序进行监听
        event_del(g_efd, ev);
        event_set(ev, cfd, EPOLLIN | EPOLLET, login_menu, ev);
        event_add(g_efd, ev);
        return;
    }
    ev->log_step++;
}

// 获取一个未注册的uid
void chat_conn::get_uid(myevent_s *ev)
{
    char str[10];
    user_msg *p = &ev->um;
    sprintf(str, "%05d", user_num + 1);   
    strcpy(ev->um.usr_id, str); 

    FILE *fp = fopen("/home/dd/01Linux/user_msg", "a+");
    if(fp == NULL) 
    {
        write(ev->fd, "error\n", 6);
        fprintf(stderr, "get_uid open file error\n");
    }
    fprintf(fp,"%s %s %s\n", p->usr_id, p->usr_name, p->usr_key);     // 将新注册的用户信息写入保存用户信息的文件
    fflush(fp);         // 刷新缓冲区, 将内容写入到文件中
}

// 写事件 ---> 向当前在线用户发送信息
void chat_conn::lcb_write(int cfd, void *arg)
{
    char str[BUFSIZ];
    myevent_s *ev = (myevent_s*)arg;
    if(ev->len <= 0) 
    {
        logout(cfd, ev);
        close_cfd(cfd, ev);
        return;
    }
    for(int i = r[0]; i != 1; i = r[i])             // 遍历当前的在线链表, 向在线用户发送
    {
        if(online_fd[i] == cfd) continue;           // 发送数据给服务器的客户端一方并不需要发送
        write(online_fd[i], ev->buf, ev->len);
    }

    if(ev->log_step == 3) write(cfd, "\n>>>", 4);   // 界面的优化,与主要逻辑无关 

    // 执行完一次事件之后--> 从树上摘下 ---> 重新设定要监听事件 ---> 重新挂上树监听
    event_del(g_efd, ev);
    event_set(ev, cfd, EPOLLIN | EPOLLET, cb_read, ev);
    event_add(g_efd, ev);
}

// 读事件 -----> 服务器接收的客户端发来的信息
void chat_conn::cb_read(int cfd, void *arg)
{
    char str[BUFSIZ],str2[1024];
    myevent_s *ev = (myevent_s *) arg;
    int ret = read(cfd, str, sizeof str);
    if(ret <= 0)
    {
        logout(cfd, ev);
        close_cfd(cfd, ev);
        return;
    }
    str[ret] = '\0';
    sprintf(str2, "from client fd: %d receive data is :", cfd);
    if(ret > 0)  write(STDOUT_FILENO, str2, strlen(str2));
    write(STDOUT_FILENO, str, ret);    // 将客户端发来的数据在服务器端进行打印

    sprintf(ev->buf, "(%s):%s\n>>>", ev->um.usr_name, str);   // 格式化客户端发来的数据 --- 数据处理
    ev->len = strlen(ev->buf);

    // 此时服务器端接收客户端发来的数据并进行数据,然后发送给其他的在线用户,故此时事件要重新设定为写事件
    event_del(g_efd, ev);
    event_set(ev, cfd, EPOLLOUT, cb_write, ev);
    event_add(g_efd, ev);
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
    this->fun(this->fd);
}




























































//=============================================================================//


//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
chat_conn::LINE_STATUS chat_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
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
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
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
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
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

//解析http请求行，获得请求方法，目标url及http版本号
chat_conn::HTTP_CODE chat_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
chat_conn::HTTP_CODE chat_conn::parse_headers(char *text)
{
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
chat_conn::HTTP_CODE chat_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

chat_conn::HTTP_CODE chat_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

chat_conn::HTTP_CODE chat_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');

    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}
void chat_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
bool chat_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
bool chat_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}
bool chat_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool chat_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
bool chat_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool chat_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool chat_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool chat_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool chat_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
bool chat_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
