/**
 *
 */

#include "txrx_mac.hpp"


MacComm::MacComm(Config* cfg, int COMM_THREAD_NUM, int in_core_offset)
{
    config_ = cfg;
    comm_thread_num_ = COMM_THREAD_NUM;

    core_id_ = in_core_offset;
    tx_core_id_ = in_core_offset + COMM_THREAD_NUM;

    /* initialize random seed: */
    srand(time(NULL));

    /* communicate with upper layers */
    socket_ = new int[COMM_THREAD_NUM];
    sock_buf_size_ = 1024 * 1024 * 64 * 8 - 1;
    max_frame_size_ = 1500;   // might want to bring this to config files

#if USE_IPV4
    remote_addr_ = new struct sockaddr_in[COMM_THREAD_NUM];
#else
    remote_addr_ = new struct sockaddr_in6[COMM_THREAD_NUM];
#endif
}


MacComm::MacComm(Config* cfg, int COMM_THREAD_NUM, int in_core_offset,
    moodycamel::ConcurrentQueue<Event_data>* in_queue_message,
    moodycamel::ConcurrentQueue<Event_data>* in_queue_task,
    moodycamel::ProducerToken** in_rx_ptoks,
    moodycamel::ProducerToken** in_tx_ptoks)
    : MacComm(cfg, COMM_THREAD_NUM, in_core_offset)
{
    message_queue_ = in_queue_message;
    task_queue_ = in_queue_task;
    rx_ptoks_ = in_rx_ptoks;
    tx_ptoks_ = in_tx_ptoks;
}


MacComm::~MacComm()
{
    delete[] socket_;
    delete[] remote_addr_;
}


bool MacComm::startComm(Table<char>& in_buffer, Table<int>& in_buffer_status,
    int in_buffer_frame_num, long long in_buffer_length,
    char* in_tx_buffer, int* in_tx_buffer_status,
    int in_tx_buffer_frame_num, int in_tx_buffer_length)
{
    buffer_ = &in_buffer; 		// for save data
    buffer_status_ = &in_buffer_status; // for save status
    buffer_frame_num_ = in_buffer_frame_num;
    buffer_length_ = in_buffer_length;
 
    tx_buffer_ = in_tx_buffer; 			// for save data
    tx_buffer_status_ = in_tx_buffer_status; 	// for save status
    tx_buffer_frame_num_ = in_tx_buffer_frame_num;
    tx_buffer_length_ = in_tx_buffer_length;

    int local_port_id = config_->millipede_up_port + tid;
#if USE_IPV4
    socket_[tid] = setup_socket_ipv4(local_port_id, true, sock_buf_size_);
    setup_sockaddr_remote_ipv4(
        &remote_addr_[tid], config_->src_sink_port + tid, config_->src_sink_addr.c_str());
#else
    socket_[tid] = setup_socket_ipv6(local_port_id, true, sock_buf_size_);
    setup_sockaddr_remote_ipv6(
        &remote_addr_[tid], config_->src_sink_port + tid, config_->src_sink_addr.c_str());
#endif

   printf("create upper layer comm. threads (source/sink) \n");
    for (int i = 0; i < comm_thread_num_; i++) {
        pthread_t src_thread;
        auto context = new EventHandlerContext<MacComm>;
        context->obj_ptr = this;
        context->id = i;
        if (pthread_create(&src_thread, NULL, pthread_fun_wrapper<MacComm, &MacComm::loopFromMac>, context) != 0) {
            perror("socket src communication thread create failed");
            exit(0);
        }
    }

    for (int i = 0; i < comm_thread_num_; i++) {
        pthread_t sink_thread;
        auto context = new EventHandlerContext<MacComm>;
        context->obj_ptr = this;
        context->id = i;
        if (pthread_create(&sink_thread, NULL, pthread_fun_wrapper<MacComm, &MacComm::loopToMac>, context) != 0) {
            perror("socket sink communication thread create failed");
            exit(0);
        }
    }
    sleep(1);
    pthread_cond_broadcast(&cond);
    return true;
}


int MacComm::loopFromMac(int tid)
{
    /*
     * Downlink (Receive from MAC -source- and send to lower PHY)
     */
    pin_to_core_with_offset(ThreadType::kWorkerFromMac, core_id_, tid);
    moodycamel::ProducerToken* local_ptok = rx_ptoks_[tid];

    char* rx_buffer = (*buffer_)[tid];
    int* rx_buffer_status = (*buffer_status_)[tid];
    int rx_buffer_frame_num = buffer_frame_num_;

    socklen_t addrlen = sizeof(remote_addr_[tid]);
    int rx_offset = 0;

    while (config_->running) {
        /* 
         READ from source packet queue
         (1) Continuously check socket buffer for any packets
         (2) Upon reception, start processing and enqueue to message_queue
          - Note: Reshape frame (let's "broadcast", replicate depending on number of UEs)
         */

        // if rx_buffer is full, exit
        if (rx_buffer_status[rx_offset] == 1) {
            printf("Upper PHY RX thread %d buffer full, offset: %d\n", tid, rx_offset);
            exit(0);
        }
        int recvlen = -1;
        if ((recvlen = recvfrom(socket_[tid], (char*)&rx_buffer[rx_offset * max_frame_size_],
                 max_frame_size_, 0, (struct sockaddr*)&remote_addr_[tid],
                 &addrlen))
            < 0) {
            perror("recv failed");
            exit(0);
        }
        printf("XXX RX FROM MAC - Size (bytes): %d\n", recvlen);

	rx_buffer_status[rx_offset] = 1;
    	Event_data dl_message(EventType::kFromMac, rx_offset);
        moodycamel::ProducerToken* local_ptok = rx_ptoks_[tid];
        if (!message_queue_->enqueue(*local_ptok, dl_message)) {
            printf("socket message enqueue failed\n");
            exit(0);
        }
	
	rx_offset++;
        if (rx_offset == rx_buffer_frame_num)
            rx_offset = 0;

    }
    return 0;
}


int MacComm::loopToMac(int tid)
{
    /*
     * Uplink (Receive from lower PHY, send up to MAC)
     */
    pin_to_core_with_offset(ThreadType::kWorkerToMac, tx_core_id_, tid);	
    Event_data task_event;
    if (!task_queue_->try_dequeue_from_producer(*tx_ptoks_[tid], task_event))
        return -1;
    if (task_event.event_type != EventType::kToMac) {
        printf("Wrong event type!");
        exit(0
    }
    
    socklen_t addrlen = sizeof(remote_addr_[tid]);
    int tx_offset = 0;

    while (config_->running) {
        // WRITE to sink packet queue
        // (1) Get packet from decoder
        // (2) Write to buffer used for transmission
        // decoded_buffer_.calloc(TASK_BUFFER_SUBFRAME_NUM, num_decoded_bytes * cfg->UE_NUM, 64);
        if (packet available in decoded_buffer_) {   // FIXME
            cur_buffer_ptr_up = (uint8_t*)decoded_buffer_[symbol_offset];  // FIXME

            if (sendto(socket_[tid], (char*)cur_buffer_ptr_up, max_frame_size_, 0,
                    (struct sockaddr*)&servaddr_[tid], sizeof(servaddr_[tid]))
                < 0) {
                perror("socket sendto failed");
                exit(0);
            }

            Event_data tx_message(EventType::kToMac, tx_offset);
            moodycamel::ProducerToken* local_ptok = rx_ptoks_[tid];
            if (!message_queue_->enqueue(*local_ptok, tx_message)) {
                printf("socket message enqueue failed\n");
                exit(0);
            }
        }
    }
    return 0;
}
