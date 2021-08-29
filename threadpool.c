//
// Created by pc on 2021/8/26.
//

#include "threadpool.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>


//这个nodnumber ，用于调整线程增幅，每隔一段时间，manger会对线程池进行一定的调整。
//每次调整幅度为一个线程。

//线程工作函数，这个函数对外隐藏，放到threadpool.c中
static void * worker(void *arg);
//管理线程函数：负责线程的动态增加 和 删除，这个函数对外隐藏，放到threadpool.c中
static void *manager( void * arg);

#define modnumber 1
threadpool * createThreadPool(int minthreadsize, int maxthreadsize, int taskqcapacity){

    //创建线程池子实例
    threadpool *pool = (threadpool *) malloc(sizeof(threadpool));
    do {
        if ( pool == NULL) break;
        //填入线程池信息
        pool->taskQ = malloc( sizeof (task) * taskqcapacity);
        if ( pool->taskQ == NULL) break;
        pool->taskQCapacity = taskqcapacity;
        pool->taskQFront = pool->taskQRear = pool->taskQSize = 0;

        //初始化线程设置
        pool->work_thread = malloc ( sizeof (pthread_t) * maxthreadsize);
        if ( pool->work_thread == NULL) break;
        pool->minThreadSize = minthreadsize;
        pool->maxThreadSize = maxthreadsize;
        pool->busyThreadSize = 0;
        pool->curThreadSize = minthreadsize;
        pool->exitThreadSize = 0;
        pool->shutdown = 0;

        //存放工作线程的thread_id
        memset(pool->work_thread, 0, sizeof (pthread_t) * maxthreadsize );

        //创建线程。
        int ret = 0;
        for(int i = 0; i < pool->minThreadSize; i ++){
            ret |= pthread_create(&pool->work_thread[i], NULL, worker, pool);
        }
        if ( ret ) break;


        //初始化锁 和 条件变量，初始化成功则返回 0;
        if (pthread_mutex_init( &pool->mutexTaskQ, NULL) ||
        pthread_mutex_init( &pool->mutexThreadSize, NULL) ||
        pthread_cond_init( &pool->notEmptyTaskQ, NULL) ||
        pthread_cond_init( &pool->notFullTaskQ, NULL)
        ) break;

        //创建管理线程
        if (pthread_create( & pool->thread_manager, NULL, manager, pool) != 0) break;
        printf("线程池创建成功\n");
        return pool;
    } while (0);

    if ( pool &&  pool->work_thread)
        free(pool->work_thread);

    if ( pool && pool->taskQ)
        free(pool->work_thread);

    if ( pool )
        free(pool);

    return NULL;
}

//线程工作函数，每次只能从任务中取出一个任务，所以需要加锁
//work 把当前线程的池中的task 取出来，所以需要拿到线程池的实例才可以，就是传递指针。
void * worker(void *arg){

    threadpool *pool = (threadpool *) arg;
    printf("thread : %ld is running\n", pthread_self());
    while ( 1 ){
        pthread_mutex_lock(&pool->mutexTaskQ);

        //如果没有任务的话，那么就阻塞在队列上，等待非空信号。
        //这里为什么要while 循环呢？ 如果是cond_signals时候，可以使用if
        //但是如果是 cond_broadcast，全部环形，那么造成虚假唤醒。
        while ( pool->taskQSize == 0 && ! pool->shutdown){
            pthread_cond_wait(&pool->notEmptyTaskQ, &pool->mutexTaskQ);
            //如果被管理线程要求退出。
            if ( pool -> exitThreadSize ){
                pool->exitThreadSize --;
                pool->curThreadSize -- ;
                pthread_mutex_unlock(&pool->mutexTaskQ);
                //结束线程。
                pthread_t tid = pthread_self();
                for(int i = 0; i < pool-> maxThreadSize; i ++){
                    if ( pool->work_thread[i] == tid){
                        pool->work_thread[i] = 0;
                        break;
                    }
                }
                printf("thread : %ld is ending\n", pthread_self());
                pthread_exit(0);
            }
        }

        //如果线程池关闭了
        if ( pool ->shutdown ){
            pthread_mutex_unlock(&pool->mutexTaskQ);
            printf("thread : %ld is ending\n", pthread_self());
            pthread_exit(NULL);
        }

        //处理任务，此时可以抽出一个元素。
        task ta = pool->taskQ[pool->taskQFront];
        pool->taskQFront = ( pool->taskQFront +  1) % pool -> taskQCapacity;
        pool->taskQSize -- ;

        //唤醒生产者线程
        pthread_cond_signal(&pool->notFullTaskQ);
        pthread_mutex_unlock(&pool->mutexTaskQ);

        //取出请求以后应该唤醒生产者。


        //执行任务，同时标记正在工作的线程数
        pthread_mutex_lock(& pool->mutexThreadSize);
        pool->busyThreadSize ++;
        pthread_mutex_unlock(& pool->mutexThreadSize);

        ta.func(ta.arg);
        free(ta.arg);


        //任务结束后，把正在忙的线程数量 - 1
        pthread_mutex_lock(& pool->mutexThreadSize);
        pool->busyThreadSize --;
        pthread_mutex_unlock(& pool->mutexThreadSize);
    }
    return NULL;
}


//管理线程函数， 因为需要拿到线程池的实例才能管理线程池，所以这里传递线程池的指针
//管理线程， 用于动态调整线程池。
void *manager( void * arg){
    threadpool *pool = (threadpool *)arg;
    while(! pool->shutdown){
        sleep(2);
        //获取当前的任务数量，由于上面的线程可能会减少，所以这里需要用锁整个池
        pthread_mutex_lock( & pool->mutexTaskQ);
        int tasksize = pool-> taskQSize;
        int curthreadsize = pool->curThreadSize;
        pthread_mutex_unlock( & pool->mutexTaskQ);

        //获取当前忙的线程数 和 池子目前的线程数量
        pthread_mutex_lock( & pool->mutexThreadSize);
        int busythreadsize = pool->busyThreadSize;
        pthread_mutex_unlock( & pool->mutexThreadSize);

        printf("管理线程报告, 忙线程个数:%d, 当前线程数:%d, 请求队列个数:%d\n", busythreadsize, curthreadsize, tasksize);
//
        //如果 忙线程数量 * 2 < 请求队列 & 忙线程没有到达最大线程数，那么增加线程。
        if ( busythreadsize * 2 < tasksize && busythreadsize < pool->maxThreadSize){

            //由于锁住整个任务，可以使得所有的工作线程没办法向下执行，所以这里使用 mutexTaskQ 锁。
            //这里锁住以后暂时不允许线程退出了
            pthread_mutex_lock( & pool->mutexTaskQ);
            //从线程id中查找一个空闲线程id 出来，最多添加 modnumber 个线程。
            for(int i = 0, cnt = 0; i < pool->maxThreadSize &&
            pool->curThreadSize < pool->maxThreadSize &&
            cnt < modnumber;
            i ++){
                if  (pool ->work_thread[i] != 0) continue;
                int ret =  pthread_create( &pool->work_thread[i], NULL, worker, pool);
                if ( ret ){
                    break;
                }
                pool->curThreadSize ++;
            }
            pthread_mutex_unlock( & pool->mutexTaskQ);
        }

        //如果忙线程数量 * 2 < 线程池线程 && 最小线程数 < 线程池数量，结束多余线程。
        if (busythreadsize * 2 < curthreadsize && pool->minThreadSize < curthreadsize){
            //应该进行递减，但是这里不能强制让线程退出，线程的工作可能还没做完，所以要通知线程。
            //因为要改变线程的数量，所以使用这个锁
            pthread_mutex_lock( & pool->mutexThreadSize);
            pool->exitThreadSize = modnumber;
            pthread_mutex_unlock( & pool->mutexThreadSize);

            //唤醒两个线程，让他们向下执行，通知结束。
            //具体的话，让子线程来判断 exitThreadSize ，他就知道是否需要结束。
            for(int i = 0; i < modnumber; i ++){
                pthread_cond_signal(&pool->notEmptyTaskQ);
            }
        }
    }
    return NULL;
}

//往请求队列上添加数据，需要持有锁。
void poolTaskAppend(threadpool *pool, void *(*func)(void *), void *arg){

    pthread_mutex_lock( & pool->mutexTaskQ);

    //如果请求队列已经满了，需要阻塞。
    while ( pool->taskQSize >= pool->taskQCapacity && ! pool->shutdown){
        pthread_cond_wait(&pool->notFullTaskQ, & pool->mutexTaskQ);
    }

    //如果线程池已经停止运行的话，解锁返回。
    if( pool->shutdown){
        pthread_mutex_unlock( & pool->mutexTaskQ);
        return ;
    }

    pool->taskQ[pool->taskQRear].func = func;
    pool->taskQ[pool->taskQRear].arg = arg;
    pool->taskQRear = (pool->taskQRear + 1) % pool->taskQCapacity;
    pool->taskQSize ++;

    //唤醒消费者
    pthread_cond_signal(&pool->notEmptyTaskQ);
    pthread_mutex_unlock( & pool->mutexTaskQ);
}

//获取线程池中正在处理请求的个数
int getBusyThreadSize(threadpool *pool){
    pthread_mutex_lock ( &pool->mutexThreadSize);
    int ret = pool->busyThreadSize;
    pthread_mutex_lock ( &pool->mutexThreadSize);
    return ret;
}

//获取线程池中的线程个数，不允许线程中途退出，所以用池锁。
int poolCurThreadSizeGet(threadpool *pool){
    pthread_mutex_lock ( &pool->mutexTaskQ);
    int ret = pool->curThreadSize;
    pthread_mutex_lock ( &pool->mutexTaskQ);
    return ret;
}

//线程池销毁
int poolDestory(threadpool *pool){
    if ( pool == NULL) return -1;
    pool->shutdown = 1;
    pthread_join(pool->thread_manager, NULL);

    //锁住线程池，不允许插入。
    pthread_mutex_lock ( &pool->mutexTaskQ);
    for(int i = 0; i < pool-> curThreadSize; i ++){
        //唤醒所有线程，并且进行销毁。
        pthread_cond_signal(&pool->notEmptyTaskQ);
    }
    pthread_mutex_unlock ( &pool->mutexTaskQ);

    //回收线程资源。
    pthread_t tid;
    for(int i = 0; i < pool->maxThreadSize; i ++){
        tid = pool->work_thread[i];
        if ( tid != 0) pthread_join(tid, NULL);
    }
    //回收所有的锁 和 条件变量。

    if ( pool &&  pool->work_thread)
        free(pool->work_thread);

    if ( pool && pool->taskQ)
        free(pool->taskQ);

    if ( pool ){
        pthread_mutex_destroy(&pool->mutexTaskQ);
        pthread_mutex_destroy(&pool->mutexThreadSize);
        pthread_cond_destroy(&pool->notEmptyTaskQ);
        pthread_cond_destroy(&pool->notFullTaskQ);
        free(pool);
    }
    return 1;
}