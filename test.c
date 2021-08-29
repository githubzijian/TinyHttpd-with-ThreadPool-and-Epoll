/* J. David's webserver */
/* This is a simple webserver.
 * Created November 1999 by J. David Blackstone.
 * CSE 4344 (Network concepts), Prof. Zeigler
 * University of Texas at Arlington
 */
/* This program compiles for Sparc Solaris 2.6.
 * To compile for Linux:
 *  1) Comment out the #include <pthread.h> line.
 *  2) Comment out the line that defines the variable newthread.
 *  3) Comment out the two lines that run pthread_create().
 *  4) Uncomment the line that runs accept_request().
 *  5) Remove -lsocket from the Makefile.
 */
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/sendfile.h>
#include <stdlib.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/epoll.h>
#include <errno.h>
#include "threadpool.h"

//宏定义，是否是空格
#define ISspace(x) isspace((int)(x))

#define SERVER_STRING "Server: zijian/0.1\r\n"

#define MAX_EVENT_NUMBER 4096

//每次收到请求，创建一个线程来处理接受到的请求
//把client_sock转成地址作为参数传入pthread_create
void * accept_request(void *arg);

//错误请求
void bad_request(int);

//读取文件
void cat(int, FILE *);

//无法执行
void cannot_execute(int);

//错误输出
void error_die(const char *);

//执行cig脚本
void execute_cgi(int, const char *, const char *, const char *);

//得到一行数据,只要发现c为\n,就认为是一行结束，如果读到\r,再用MSG_PEEK的方式读入一个字符，如果是\n，从socket用读出
//如果是下个字符则不处理，将c置为\n，结束。如果读到的数据为0中断，或者小于0，也视为结束，c置为\n
int get_line(int, char *, int);

//返回http头
void headers(int, const char *);

//没有发现文件
void not_found(int);

//如果不是CGI文件，直接读取文件返回给请求的http客户端
void serve_file(int, const char *);

//开启tcp连接，绑定端口等操作
int startup(char *argv[]);

//如果不是Get或者Post，就报方法没有实现
void unimplemented(int);

//非阻塞IO
int setnonblocking(int fd);


//从内核事件表删除
void removefd(int epollfd, int fd);

//把fd 注册到 内核事件表中。
void addfd(int epollfd, int fd);




// Http请求，后续主要是处理这个头
//
// GET / HTTP/1.1
// Host: 192.168.0.23:47310
// Connection: keep-alive
// Upgrade-Insecure-Requests: 1
// User-Agent: Mozilla/5.0 (Windows NT 6.1; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/55.0.2883.87 Safari/537.36
// Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*; q = 0.8
// Accept - Encoding: gzip, deflate, sdch
// Accept - Language : zh - CN, zh; q = 0.8
// Cookie: __guid = 179317988.1576506943281708800.1510107225903.8862; monitor_count = 5
//

// POST / color1.cgi HTTP / 1.1
// Host: 192.168.0.23 : 47310
// Connection : keep - alive
// Content - Length : 10
// Cache - Control : max - age = 0
// Origin : http ://192.168.0.23:40786
// Upgrade - Insecure - Requests : 1
// User - Agent : Mozilla / 5.0 (Windows NT 6.1; WOW64) AppleWebKit / 537.36 (KHTML, like Gecko) Chrome / 55.0.2883.87 Safari / 537.36
// Content - Type : application / x - www - form - urlencoded
// Accept : text / html, application / xhtml + xml, application / xml; q = 0.9, image / webp, */*;q=0.8
// Referer: http://192.168.0.23:47310/
// Accept-Encoding: gzip, deflate
// Accept-Language: zh-CN,zh;q=0.8
// Cookie: __guid=179317988.1576506943281708800.1510107225903.8862; monitor_count=281
// Form Data
// color=gray

/**********************************************************************/
/* A request has caused a call to accept() on the server port to
 * return.  Process the request appropriately.
 * Parameters: the socket connected to the client */
/**********************************************************************/
void * accept_request(void *arg)
{
//    volatile int ret = 0;
//    for(int i = 0; i < 1000000; i ++){
//        ret ++;
//    }

    //socket
    int client = *(int *)arg;
//    char buff[1024] = "hello world";
//    send(client, buff, strlen(buff), 0);
//    close(client);
//    return NULL;

    //printf("client is %d\n", client);
    char buf[1024];
    int numchars;
    char method[255];
    char url[255];
    char path[512];
    size_t i, j;
    struct stat st;
    int cgi = 0;      /* becomes true if server decides this is a CGI
                    * program */
    char *query_string = NULL;
    //根据上面的Get请求，可以看到这边就是取第一行
    //这边都是在处理第一条http信息
    //"GET / HTTP/1.1\n"
    numchars = get_line(client, buf, sizeof(buf));
    i = 0; j = 0;

    //第一行字符串提取Get
    while (!ISspace(buf[j]) && (i < sizeof(method) - 1))
    {
        method[i] = buf[j];
        i++; j++;
    }
    //结束，得到Method ，只能处理GET 或者 POST
    method[i] = '\0';

    //判断是Get还是Post，如果这两种方法都不是的话，发送 501
    if (strcasecmp(method, "GET") && strcasecmp(method, "POST"))
    {
        unimplemented(client);
        return NULL;
    }

    //如果是POST，cgi置为1
    if (strcasecmp(method, "POST") == 0)
        cgi = 1;

    i = 0;
    //跳过空格
    while (ISspace(buf[j]) && (j < sizeof(buf)))
        j++;


    //获取GET 或者 POST 后面的那个资源名，比如GET /index.html，那么url 就是/index.html
    while (!ISspace(buf[j]) && (i < sizeof(url) - 1) && (j < sizeof(buf)))
    {
        url[i] = buf[j];
        i++; j++;
    }
    url[i] = '\0';



    //判断Get请求，如果带有问号，则是动态请求，cgi = 1
    if (strcasecmp(method, "GET") == 0)
    {
        query_string = url;
        while ((*query_string != '?') && (*query_string != '\0'))
            query_string++;
        if (*query_string == '?')
        {
            cgi = 1;
            *query_string = '\0';
            query_string++;
        }
    }

    //资源路径。
    sprintf(path, "/var/www/html%s", url);


    //默认地址，解析到的路径如果为/，则自动加上index.html
    //如果请求是 10.0.0.3,那么GET 请求会默认加上 /
    //如果是默认的，那么发送一个index.html
    if (path[strlen(path) - 1] == '/'){
        strcat(path, "index.html");
    }


    //获得文件信息，如果找不到文件
    if (stat(path, &st) == -1) {
        //如果找不到资源文件，把请求剩余数据全部读取并且丢弃。
        while ((numchars > 0) && strcmp("\n", buf))  /* read & discard headers */
            numchars = get_line(client, buf, sizeof(buf));

        //发送找不到
        not_found(client);
    }
    else
    {
        //如果访问的是某个目录，那么定位到这个目录的index.html文件。
        if ((st.st_mode & S_IFMT) == S_IFDIR)
            strcat(path, "/index.html");

        //printf("请求资源文件:%s\n", path);

        //如果你的文件默认是有执行权限的，自动解析成cgi程序，如果有执行权限但是不能执行，会接受到报错信号
        if ((st.st_mode & S_IXUSR) ||
            (st.st_mode & S_IXGRP) ||
            (st.st_mode & S_IXOTH)    )
            cgi = 1;
        if (!cgi)
            //如果不是cgi，直接读取一个html文件给客户端。
            serve_file(client, path);
        else
            //执行cgi文件
            execute_cgi(client, path, method, query_string);
    }
    //执行完毕关闭socket
    close(client);
    return NULL;
}

/**********************************************************************/
/* Inform the client that a request it has made has a problem.
 * Parameters: client socket */
/**********************************************************************/
void bad_request(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 400 BAD REQUEST\r\n");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "Content-type: text/html\r\n");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "<P>Your browser sent a bad request, ");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "such as a POST without a Content-Length.\r\n");
    send(client, buf, sizeof(buf), 0);
}


/**********************************************************************/
/* Inform the client that a CGI script could not be executed.
 * Parameter: the client socket descriptor. */
/**********************************************************************/
void cannot_execute(int client)
{
    char buf[1024];
    sprintf(buf, "HTTP/1.0 500 Internal Server Error\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<P>Error prohibited CGI execution.\r\n");
    send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
/* Print out an error message with perror() (for system errors; based
 * on value of errno, which indicates system call errors) and exit the
 * program indicating an error. */
/**********************************************************************/
void error_die(const char *sc)
{
    perror(sc);
    exit(1);
}

/**********************************************************************/
/* Execute a CGI script.  Will need to set environment variables as
 * appropriate.
 * Parameters: client socket descriptor
 *             path to the CGI script */
/**********************************************************************/
void execute_cgi(int client, const char *path,
                 const char *method, const char *query_string)
{
    //缓冲区
    char buf[1024];

    //2根管道
    int cgi_output[2];
    int cgi_input[2];

    //进程pid和状态
    pid_t pid;
    int status;

    int i;
    char c;

    //读取的字符数
    int numchars = 1;

    //http的content_length
    int content_length = -1;

    //默认字符
    buf[0] = 'A'; buf[1] = '\0';

    //忽略大小写比较字符串
    if (strcasecmp(method, "GET") == 0)
        //读取数据，把整个header都读掉，以为Get写死了直接读取index.html，没有必要分析余下的http信息了
        while ((numchars > 0) && strcmp("\n", buf))  /* read & discard headers */
            numchars = get_line(client, buf, sizeof(buf));
    else    /* POST */
    {
        numchars = get_line(client, buf, sizeof(buf));
        while ((numchars > 0) && strcmp("\n", buf))
        {
            //如果是POST请求，就需要得到Content-Length，Content-Length：这个字符串一共长为15位，所以
            //取出头部一句后，将第16位设置结束符，进行比较
            //第16位置为结束
            buf[15] = '\0';
            if (strcasecmp(buf, "Content-Length:") == 0)
                //内存从第17位开始就是长度，将17位开始的所有字符串转成整数就是content_length
                content_length = atoi(&(buf[16]));
            numchars = get_line(client, buf, sizeof(buf));
        }
        if (content_length == -1) {
            bad_request(client);
            return;
        }
    }

    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    send(client, buf, strlen(buf), 0);
    //建立output管道
    if (pipe(cgi_output) < 0) {
        cannot_execute(client);
        return;
    }

    //建立input管道
    if (pipe(cgi_input) < 0) {
        cannot_execute(client);
        return;
    }
    //       fork后管道都复制了一份，都是一样的
    //       子进程关闭2个无用的端口，避免浪费
    //       ×<------------------------->1    output
    //       0<-------------------------->×   input

    //       父进程关闭2个无用的端口，避免浪费
    //       0<-------------------------->×   output
    //       ×<------------------------->1    input
    //       此时父子进程已经可以通信


    //fork进程，子进程用于执行CGI
    //父进程用于收数据以及发送子进程处理的回复数据
    if ( (pid = fork()) < 0 ) {
        cannot_execute(client);
        return;
    }
    if (pid == 0)  /* child: CGI script */
    {
        char meth_env[255];
        char query_env[255];
        char length_env[255];

        //子进程输出重定向到output管道的1端
        dup2(cgi_output[1], STDOUT_FILENO);
        //子进程输入重定向到input管道的0端
        dup2(cgi_input[0], STDIN_FILENO);



        //关闭无用管道口
        close(cgi_output[0]);
        close(cgi_input[1]);

        //CGI环境变量
        sprintf(meth_env, "REQUEST_METHOD=%s", method);
        putenv(meth_env);
        if (strcasecmp(method, "GET") == 0) {
            sprintf(query_env, "QUERY_STRING=%s", query_string);
            putenv(query_env);
        }
        else {   /* POST */
            sprintf(length_env, "CONTENT_LENGTH=%d", content_length);
            putenv(length_env);
        }
        //替换执行path
        execl(path, path, NULL);
        //int m = execl(path, path, NULL);
        //如果path有问题，例如将html网页改成可执行的，但是执行后m为-1
        //退出子进程，管道被破坏，但是父进程还在往里面写东西，触发Program received signal SIGPIPE, Broken pipe.
        exit(0);
    } else {    /* parent */

        //关闭无用管道口
        close(cgi_output[1]);
        close(cgi_input[0]);
        if (strcasecmp(method, "POST") == 0)
            for (i = 0; i < content_length; i++) {
                //得到post请求数据，写到input管道中，供子进程使用
                recv(client, &c, 1, 0);
                write(cgi_input[1], &c, 1);
            }
        //从output管道读到子进程处理后的信息，然后send出去
        while (read(cgi_output[0], &c, 1) > 0)
            send(client, &c, 1, 0);

        //完成操作后关闭管道
        close(cgi_output[0]);
        close(cgi_input[1]);

        //等待子进程返回
        waitpid(pid, &status, 0);

    }
}

/**********************************************************************/
/* Get a line from a socket, whether the line ends in a newline,
 * carriage return, or a CRLF combination.  Terminates the string read
 * with a null character.  If no newline indicator is found before the
 * end of the buffer, the string is terminated with a null.  If any of
 * the above three line terminators is read, the last character of the
 * string will be a linefeed and the string will be terminated with a
 * null character.
 * Parameters: the socket descriptor
 *             the buffer to save the data in
 *             the size of the buffer
 * Returns: the number of bytes stored (excluding null) */
/**********************************************************************/

//得到一行数据,只要发现c为\n,就认为是一行结束，如果读到\r,再用MSG_PEEK的方式读入一个字符，如果是\n，从socket用读出
//如果是下个字符则不处理，将c置为\n，结束。如果读到的数据为0中断，或者小于0，也视为结束，c置为\n

//读取一行数据到 buffer中。
int get_line(int sock, char *buf, int size)
{
    int i = 0;
    char c = '\0';
    int n;

    while ((i < size - 1) && (c != '\n'))
    {
        n = recv(sock, &c, 1, 0);
        /* DEBUG printf("%02X\n", c); */
        if (n > 0)
        {
            if (c == '\r')
            {
                //偷窥一个字节，如果是\n就读走
                n = recv(sock, &c, 1, MSG_PEEK);
                /* DEBUG printf("%02X\n", c); */
                if ((n > 0) && (c == '\n'))
                    recv(sock, &c, 1, 0);
                else
                    //不是\n（读到下一行的字符）或者没读到，置c为\n 跳出循环,完成一行读取
                    c = '\n';
            }
            buf[i] = c;
            i++;
        }
        else
            c = '\n';
    }
    buf[i] = '\0';
    return(i);
}


//加入http的headers
void headers(int client, const char *filename)
{
    char buf[1024];
    (void)filename;  /* could use filename to determine file type */

    strcpy(buf, "HTTP/1.0 200 OK\r\n");
    send(client, buf, strlen(buf), 0);
    strcpy(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    strcpy(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
/* Give a client a 404 not found status message. */
/**********************************************************************/

//如果资源没有找到得返回给客户端下面的信息
void not_found(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 404 NOT FOUND\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<HTML><TITLE>Not Found</TITLE>\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<BODY><P>The server could not fulfill\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "your request because the resource specified\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "is unavailable or nonexistent.\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</BODY></HTML>\r\n");
    send(client, buf, strlen(buf), 0);
}


//如果不是CGI文件，直接读取文件返回给请求的http客户端
void serve_file(int client, const char *filename)
{
    int resourcefd = open(filename, O_RDONLY);
    int numchars = 1;
    char buf[1024];

    //默认字符
    buf[0] = 'A'; buf[1] = '\0';
    //这是在干嘛？


    //前面已经放弃过一次头部信息了，我也不知道为什么还要接着读
    /* read & discard headers */
    while ((numchars > 0) && strcmp("\n", buf)){
        numchars = get_line(client, buf, sizeof(buf));
    }

    if ( -1 ==  resourcefd){
        not_found(client);
    }
    else
    {
        //发送一个头部。
        headers(client, filename);

        struct stat stat_buf;
        fstat(resourcefd, & stat_buf);
        //执行sendfile，零拷贝发送文件。
        sendfile(client, resourcefd, NULL, stat_buf.st_size);
    }
    close(resourcefd);
}

//初始化服务端的监听socket。
int startup(char *argv[])
{
    const char *ip = argv[1];
    int port = atoi(argv[2]);

    int httpd = 0;
    struct sockaddr_in name;

    //设定address的地址信息
    memset(&name, 0, sizeof(name));
    bzero( & name, sizeof (name)); //清空地址信息
    name.sin_family = AF_INET;//协议族
    inet_pton(AF_INET, ip, & name.sin_addr); //填入网络序IP
    name.sin_port = htons(port); //填入网络序 port

    httpd = socket(PF_INET, SOCK_STREAM, 0);
    if (httpd == -1)
        error_die("socket");

    //用于调试
    int reuse=1;
    setsockopt(httpd,SOL_SOCKET,SO_REUSEADDR, &reuse,sizeof(reuse));


    //绑定socket
    if (bind(httpd, (struct sockaddr *)&name, sizeof(name)) < 0)
        error_die("bind");

    //监听
    if (listen(httpd, 5) < 0)
        error_die("listen");

    return(httpd);
}

/**********************************************************************/
/* Inform the client that the requested web method has not been
 * implemented.
 * Parameter: the client socket */
/**********************************************************************/

//如果方法没有实现，就返回此信息
void unimplemented(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 501 Method Not Implemented\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<HTML><HEAD><TITLE>Method Not Implemented\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</TITLE></HEAD>\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<BODY><P>HTTP request method not supported.\r\n");
//    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</BODY></HTML>\r\n");
    send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
//非阻塞IO
int setnonblocking(int fd){
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//把fd 注册到 内核事件表中。
void addfd(int epollfd, int fd) {
    struct epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    setnonblocking(fd);
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
}

//把fd 注册到 内核事件表中。
void addfd2(int epollfd, int fd) {
    struct epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN  | EPOLLRDHUP;
//    setnonblocking(fd);
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
}

//把 fd 从内核事件表移除
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
}

int main(int argc, char *argv[])
{
    assert(argc > 2);
    int server_sock = -1;

    int client_sock = -1;
    struct sockaddr_in client_name;
    server_sock = startup(argv);


//原版代码
//    socklen_t client_name_len = sizeof(client_name);
//    pthread_t newthread;
//    while (1)
//    {
//        client_sock = accept(server_sock,
//                             (struct sockaddr *)&client_name,
//                             &client_name_len);
//        if (client_sock == -1)
//            error_die("accept");
//        /* accept_request(client_sock); */
//
//        //每次收到请求，创建一个线程来处理接受到的请求
//        //把client_sock转成地址作为参数传入pthread_create
//
//
//        if ( pthread_create(&newthread, NULL, (void *)accept_request, (void *)(&client_sock)) != 0 ){
//            perror("pthread_create");
//        }
//
//    }

    printf("进程pid = %d\n", getpid());
    //创建线程池
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);

    threadpool *pool = createThreadPool(2,6,50000);
    int epollfd = epoll_create(10);
    addfd(epollfd, server_sock); //非阻塞的listensocket 配合 epoll 边缘触发
    struct epoll_event events[MAX_EVENT_NUMBER];

    //这里改用线程池来处理
    while( 1 ) {
        int n = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        for(int i = 0; i < n; i ++){
            int sockfd = events[i].data.fd;

            //如果是来了新的连接
            if ( (sockfd == server_sock ) && (events[i].events & EPOLLIN)){
                int ret;
                while ( 1 ){
                    ret = accept(server_sock, (struct sockaddr *)&client_address, &client_addrlength);
                    if ( ret == -1 ){
                        if ( errno == EAGAIN || errno == EWOULDBLOCK){
                            break;
                        }
                        perror("accept error");
                        exit(0);
                    }

                    if (ret == 0) break;
                    addfd(epollfd, ret);
                }
            }

            //如果是客户请求，那么使用线程池来完成任务。
            else if ( (sockfd != server_sock) && (events[i].events & EPOLLIN)){
                //这里应该把所有的数据都读取完毕才对的....
                int *requestSockfd = malloc(sizeof (int));
                *requestSockfd = sockfd;
                removefd(epollfd, sockfd);
                poolTaskAppend(pool, accept_request, requestSockfd);
            }

            else if ( (sockfd != server_sock) && (events[i].events & EPOLLRDHUP)){
                printf("从队列删除\n");
                removefd(epollfd, sockfd);
            }

        }
    }
    pollDestory(pool);
    removefd(epollfd, server_sock);
    close(server_sock);
    close(epollfd);
    return(0);
}
