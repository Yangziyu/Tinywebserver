#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "locker.h"

// 线程池类，将它定义为模板类是为了代码复用，模板参数T是任务类
template<typename T> //根据任务来选择线程
class threadpool {
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T* request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void* worker(void* arg); //不能访问非晶态成员
    void run();

private:
    // 线程的数量
    int m_thread_number;  
    
    // 描述线程池的数组，大小为m_thread_number    
    pthread_t * m_threads;

    // 请求队列中最多允许的、等待处理的请求的数量  任务
    int m_max_requests; 
    
    // 请求队列
    std::list< T* > m_workqueue;  

    // 保护请求队列的互斥锁
    locker m_queuelocker;   

    // 信号量：是否有任务需要处理
    sem m_queuestat;

    // 是否结束线程          
    bool m_stop;                    
};

template< typename T >
threadpool< T >::threadpool(int thread_number, int max_requests) : 
        m_thread_number(thread_number), m_max_requests(max_requests), 
        m_stop(false), m_threads(NULL) {

    if((thread_number <= 0) || (max_requests <= 0) ) {
        throw std::exception();
    }

    m_threads = new pthread_t[m_thread_number];
    if(!m_threads) {
        throw std::exception();
    }
    // 线程创建成功

    // 创建thread_number 个线程，并将他们设置为脱离线程。
    for ( int i = 0; i < thread_number; ++i ) {
        printf( "create the %dth thread\n", i);
        if(pthread_create(m_threads + i, NULL, worker, this ) != 0) { //worker必须是静态的函数，是子线程要执行的代码
            delete [] m_threads; //线程创建出错了，释放掉
            throw std::exception();
        }
        
        if( pthread_detach( m_threads[i] ) ) { //设置线程脱离
            delete [] m_threads;
            throw std::exception();
        }
    }
}

template< typename T >
threadpool< T >::~threadpool() {
    delete [] m_threads; //释放
    m_stop = true; //线程会停止
}

template< typename T >
bool threadpool< T >::append( T* request ) //添加任务
{
    // 操作工作队列时一定要加锁，因为它被所有线程共享。
    m_queuelocker.lock();
    if ( m_workqueue.size() > m_max_requests ) { //超出了最大的量
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request); //往队列中增加了一个
    m_queuelocker.unlock(); //解锁
    m_queuestat.post(); //信号量增加
    return true;
}

template< typename T >
void* threadpool< T >::worker( void* arg ) //静态，不能访问非静态->传递this进来
{
    threadpool* pool = ( threadpool* )arg; //this，根据this线程池里面的成员
    pool->run(); //线程池要运行
    return pool;
}

template< typename T >
void threadpool< T >::run() { //线程池运行

    while (!m_stop) { //一直循环直到stop
        m_queuestat.wait(); //判断有没有任务可以做，有的话，信号量-1，不阻塞；否则，阻塞
        m_queuelocker.lock();
        if ( m_workqueue.empty() ) {
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front(); //获取第一个任务
        m_workqueue.pop_front(); //出队列
        m_queuelocker.unlock();
        if ( !request ) { //没获取到
            continue;
        }
        request->process(); //任务的函数
    }

}

#endif
