//
// Created by pc on 2021/8/26.
//

#ifndef MYHTTPD_THREADPOOL_H
#define MYHTTPD_THREADPOOL_H

#include <pthread.h>

//任务的定义
typedef struct task{
    //执行的函数， 由pthread_create 来执行。
    void *(*func)(void *);

    //函数参数
    void *arg;
} task;

//一个线程池应该有什么信息。
typedef struct threadpool{

    //任务队列
    task * taskQ;
    int taskQFront; //队列头
    int taskQRear;  //队列尾
    int taskQSize;  //当前队列个数
    int taskQCapacity; //任务队列容量（这不是动态调整的）
    pthread_mutex_t mutexTaskQ; //锁任务队列
    pthread_cond_t notEmptyTaskQ; //任务队列为非空
    pthread_cond_t notFullTaskQ; //任务队列为非满



    //管理线程 和 一堆工作线程
    pthread_t thread_manager; //管理线程
    pthread_t *work_thread; //工作线程
    int minThreadSize; //记录最少工作线程数
    int maxThreadSize; //记录最多工作线程数
    int curThreadSize; //记录当前线程池有多少条线程。
    int busyThreadSize; //记录工作繁忙线程数量
    int exitThreadSize; //记录将要销毁的线程数。
    int shutdown; //是否关闭线程池
    pthread_mutex_t mutexThreadSize;

}threadpool;

//线程池创建，应当返回一个线程池实例
threadpool * createThreadPool(int minthreadsize, int maxthreadsize, int taskqcapacity);

//把请求放到请求队列中，需要加入到具体的池子中，所以需要池子实例以及任务指针。
void poolTaskAppend(threadpool *pool, void *(*func)(void *), void *arg);

//获取线程池中正在处理请求的个数
int poolBusyThreadSizeGet(threadpool *pool);

//获取线程池中的线程条数。
int poolCurThreadSizeGet(threadpool *pool);

//线程池销毁
int pollDestory(threadpool *pool);

//获取请求数量
int getTaskSize(threadpool *pool);


//线程工作函数，这个函数对外隐藏，放到threadpool.c中
//static void * worker(void *arg);

////管理线程函数：负责线程的动态增加 和 删除，这个函数对外隐藏，放到threadpool.c中
//static void *manager( void * arg);

#endif //MYHTTPD_THREADPOOL_H
