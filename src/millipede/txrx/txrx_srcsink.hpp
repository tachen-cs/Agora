/**
 *
 */

#ifndef SRCSINKCOMM
#define SRCSINKCOMM

#include "Symbols.hpp"
#include "buffer.hpp"
#include "concurrentqueue.h"
#include "gettime.h"
#include "net.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <ctime>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <numeric>
#include <pthread.h>
#include <stdio.h> /* for fprintf */
#include <stdlib.h>
#include <string.h> /* for memcpy */
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include "config.hpp"

typedef unsigned short ushort;
class SrcSinkComm {
public:
    //     // use for create pthread
    //     struct PacketTXRXContext
    //     {
    //         PacketTXRX *ptr;
    //         int tid;
    //     };

public:
    SrcSinkComm(Config* cfg, int COMM_THREAD_NUM = 1, int in_core_offset = 1);
    /**
     * COMM_THREAD_NUM: socket thread number
     * in_queue: message queue to communicate with main thread
     */
    SrcSinkComm(Config* cfg, int COMM_THREAD_NUM, int in_core_offset,
        moodycamel::ConcurrentQueue<Event_data>* in_queue_message,
        moodycamel::ConcurrentQueue<Event_data>* in_queue_task,
        moodycamel::ProducerToken** in_rx_ptoks,
        moodycamel::ProducerToken** in_tx_ptoks);
    ~SrcSinkComm();

    /**
     * called in main threads to start the socket threads
     * in_buffer: ring buffer to save packets
     * in_buffer_status: record the status of each memory block (0: empty, 1:
     * full) in_buffer_frame_num: number of packets the ring buffer could hold
     * in_buffer_length: size of ring buffer
     * in_core_id: attach socket threads to {in_core_id, ..., in_core_id +
     * COMM_THREAD_NUM - 1}
     */
    bool startComm(Table<char>& in_buffer, Table<int>& in_buffer_status,
        int in_buffer_frame_num, long long in_buffer_length,
        Table<double>& in_frame_start, char* in_tx_buffer,
        int* in_tx_buffer_status, int in_tx_buffer_frame_num,
        int in_tx_buffer_length);
    /**
     * receive thread
     */
    void* loopTXRX(int tid);

#if USE_IPV4
    typedef struct sockaddr_in sockaddr_t;
#else
    typedef struct sockaddr_in6 sockaddr_t;
#endif
    int dequeue_send(int tid);
    struct Packet* recv_enqueue(int tid, int radio_id, int rx_offset);

private:
#if USE_IPV4
    struct sockaddr_in* remote_addr_;
#else
    struct sockaddr_in6* remote_addr_;  /* server address */
#endif
    int* socket_;
    char* socket_up_buffer_;
    int sock_buf_size_;

    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

    Table<char>* buffer_;
    Table<int>* buffer_status_;
    long long buffer_length_;
    int buffer_frame_num_;

    char* tx_buffer_;
    int* tx_buffer_status_;
    long long tx_buffer_length_;
    int tx_buffer_frame_num_;

    int comm_thread_num_;

    Table<double>* frame_start_;
    // pointer of message_queue_
    moodycamel::ConcurrentQueue<Event_data>* message_queue_;
    moodycamel::ConcurrentQueue<Event_data>* task_queue_;
    moodycamel::ProducerToken** rx_ptoks_;
    moodycamel::ProducerToken** tx_ptoks_;
    int core_id_;
    int tx_core_id_;

    Config* config_;
};

#endif
