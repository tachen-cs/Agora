/**
 *
 */

#include "txrx_srcsink.hpp"

SrcSinkComm::SrcSinkComm(Config* cfg, int COMM_THREAD_NUM, int in_core_offset)
{
    config_ = cfg;
    comm_thread_num_ = COMM_THREAD_NUM;

    core_id_ = in_core_offset;
    tx_core_id_ = in_core_offset + COMM_THREAD_NUM;

    /* initialize random seed: */
    srand(time(NULL));

    /* communicate with upper layers */
    socket_ = new int[COMM_THREAD_NUM];
#if USE_IPV4
    remote_addr_ = new struct sockaddr_in[COMM_THREAD_NUM];
#else
    remote_addr_ = new struct sockaddr_in6[COMM_THREAD_NUM];
#endif
}

SrcSinkComm::SrcSinkComm(Config* cfg, int COMM_THREAD_NUM, int in_core_offset,
    moodycamel::ConcurrentQueue<Event_data>* in_queue_message,
    moodycamel::ConcurrentQueue<Event_data>* in_queue_task,
    moodycamel::ProducerToken** in_rx_ptoks,
    moodycamel::ProducerToken** in_tx_ptoks)
    : SrcSinkComm(cfg, COMM_THREAD_NUM, in_core_offset)
{
    message_queue_ = in_queue_message;
    task_queue_ = in_queue_task;
    rx_ptoks_ = in_rx_ptoks;
    tx_ptoks_ = in_tx_ptoks;
}

SrcSinkComm::~SrcSinkComm()
{
    delete[] socket_;
    delete[] remote_addr_;
}

bool SrcSinkComm::startComm(Table<char>& in_buffer, Table<int>& in_buffer_status,
    int in_buffer_frame_num, long long in_buffer_length,
    Table<double>& in_frame_start, char* in_tx_buffer, int* in_tx_buffer_status,
    int in_tx_buffer_frame_num, int in_tx_buffer_length)
{
    buffer_ = &in_buffer; 		// for save data
    buffer_status_ = &in_buffer_status; // for save status
    frame_start_ = &in_frame_start;

    // check length
    buffer_frame_num_ = in_buffer_frame_num;
    buffer_length_ = in_buffer_length;
    tx_buffer_ = in_tx_buffer; 			// for save data
    tx_buffer_status_ = in_tx_buffer_status; 	// for save status
    tx_buffer_frame_num_ = in_tx_buffer_frame_num;
    tx_buffer_length_ = in_tx_buffer_length;

    printf("create upper layer comm. threads (source/sink) \n");
    for (int i = 0; i < comm_thread_num_; i++) {
        pthread_t srcsink_thread;
        auto context = new EventHandlerContext<PacketTXRX>;
        context->obj_ptr = this;
        context->id = i;
        if (pthread_create(&srcsink_thread, NULL, pthread_fun_wrapper<PacketTXRX, &PacketTXRX::loopSrcSink>, context) != 0) {
            perror("socket src/sink communication thread create failed");
            exit(0);
        }
    }
    sleep(1);
    pthread_cond_broadcast(&cond);
    return true;
}


// XXX OBCH XXX
void* SrcSinkComm::loopTXRX(int tid)
{
    pin_to_core_with_offset(ThreadType::kWorkerTXRX, core_id_, tid);

    int sock_buf_size = 1024 * 1024 * 64 * 8 - 1;
    int local_port_id = config_->millipede_up_port + tid;

#if USE_IPV4
    socket_[tid] = setup_socket_ipv4(local_port_id, true, sock_buf_size);
    setup_sockaddr_remote_ipv4(
        &remote_addr_[tid], config_->src_sink_port + tid, config_->src_sink_addr.c_str());
#else
    socket_[tid] = setup_socket_ipv6(local_port_id, true, sock_buf_size);
    setup_sockaddr_remote_ipv6(
        &remote_addr_[tid], config_->src_sink_port + tid, config_->src_sink_addr.c_str());
#endif

    int packet_length = config_->packet_length;
    socklen_t addrlen = sizeof(servaddr_[tid]);

    while (config_->running) {

        /* 
         READ from source packet queue
         (1) Continuously check socket buffer for any packets
         (2) Upon reception, start processing and enqueue to message_queue
          - Note: Reshape frame (let's "broadcast", replicate depending on number of UEs)
         */

        int recvlen = -1;
        if ((recvlen = recvfrom(socket_[tid], socket_up_buffer_,
                 packet_length, 0, (struct sockaddr*)&remote_addr_[tid],
                 &addrlen))
            < 0) {
            perror("recv failed");
            exit(0);
        }
        printf("XXX RX FROM MAC - Size (bytes): %d\n", recvlen);

        // dl_IQ_data.malloc(dl_data_symbol_num_perframe, OFDM_DATA_NUM * UE_ANT_NUM, 64);
        // Dimension 1: dl_data_symbol_num_perframe
        // Dimension 2: OFDM_DATA_NUM * UE_ANT_NUM
        // [UE0_OFDM0, UE1_OFDM0, UE0_OFDM1 UE1_OFDM1]
        for (size_t i = 0; i < config_->dl_data_symbol_num_perframe; i++) {   // One symbol is a full packet...?
            std::vector<int8_t> in_modul;
            for (size_t ue_id = 0; ue_id < config_->UE_ANT_NUM; ue_id++) {
                for (size_t j = 0; j < recvlen - 1; j++) {   // config_->OFDM_DATA_NUM; j++) {
                    int cur_offset = j * UE_ANT_NUM + ue_id;
                    config_->dl_IQ_data[i][cur_offset] = (int8_t) socket_up_buffer_[j];
                    if (ue_id == 0)
                        in_modul.push_back(dl_IQ_data[i][cur_offset]);
                }
            }
        }

	// Push packet-received-from-source event into the queue 
        Event_data packet_message(
            EventType::kFromSrc, rx_tag_t(tid, offset)._tag);

        if (!message_queue_->enqueue(*local_ptok, packet_message)) {
            printf("socket message enqueue failed\n");
            exit(0);
	}

	/*
        // WRITE to sink packet queue
        // (1) Get packet from decoder
        // (2) Write to buffer used for transmission TODO
        // decoded_buffer_.calloc(TASK_BUFFER_SUBFRAME_NUM, num_decoded_bytes * cfg->UE_NUM, 64);
        if (packet available in decoded_buffer_) {   // FIXME
            cur_buffer_ptr_up = (uint8_t*)decoded_buffer_[symbol_offset];
            if (sendto(socket_[tid], (char*)cur_buffer_ptr_up, packet_length, 0,
                    (struct sockaddr*)&servaddr_[tid], sizeof(servaddr_[tid]))
                < 0) {
                perror("socket sendto failed");
                exit(0);
            }

            // Push packet-sent-to-sink-event into the queue
            Event_data packet_message(
                EventType::kToSink, rx_tag_t(tid, offset)._tag);

            if (!message_queue_->enqueue(*local_ptok, packet_message)) {
                printf("socket message enqueue failed\n");
                exit(0);
            }
        }*/ 
    }
    return 0;
}
// XXX OBCH END XXX
