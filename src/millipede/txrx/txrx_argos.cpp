/**
 * Author: Jian Ding
 * Email: jianding17@gmail.com
 *
 */

#include "txrx.hpp"

PacketTXRX::PacketTXRX(Config* cfg, int COMM_THREAD_NUM, int in_core_offset)
{
    config_ = cfg;
    comm_thread_num_ = COMM_THREAD_NUM;

    core_id_ = in_core_offset;
    tx_core_id_ = in_core_offset + COMM_THREAD_NUM;

    /* initialize random seed: */
    srand(time(NULL));

    // XXX OBCH XXX
    socket_ = new int[COMM_THREAD_NUM];
#if USE_IPV4
    remote_addr_ = new struct sockaddr_in[COMM_THREAD_NUM];
#else
    remote_addr_ = new struct sockaddr_in6[COMM_THREAD_NUM];
#endif
    // XXX OBCH XXX

    radioconfig_ = new RadioConfig(config_);
}

PacketTXRX::PacketTXRX(Config* cfg, int COMM_THREAD_NUM, int in_core_offset,
    moodycamel::ConcurrentQueue<Event_data>* in_queue_message,
    moodycamel::ConcurrentQueue<Event_data>* in_queue_task,
    moodycamel::ProducerToken** in_rx_ptoks,
    moodycamel::ProducerToken** in_tx_ptoks)
    : PacketTXRX(cfg, COMM_THREAD_NUM, in_core_offset)
{
    message_queue_ = in_queue_message;
    task_queue_ = in_queue_task;
    rx_ptoks_ = in_rx_ptoks;
    tx_ptoks_ = in_tx_ptoks;
}

PacketTXRX::~PacketTXRX()
{
    radioconfig_->radioStop();
    delete[] socket_;             // XXX OBCH XXX
    delete remote_addr_;		  // XXX OBCH XXX
    delete radioconfig_;
}

bool PacketTXRX::startTXRX(Table<char>& in_buffer, Table<int>& in_buffer_status,
    int in_buffer_frame_num, long long in_buffer_length,
    Table<double>& in_frame_start, char* in_tx_buffer, int* in_tx_buffer_status,
    int in_tx_buffer_frame_num, int in_tx_buffer_length)
{
    buffer_ = &in_buffer; // for save data
    buffer_status_ = &in_buffer_status; // for save status
    frame_start_ = &in_frame_start;

    // check length
    buffer_frame_num_ = in_buffer_frame_num;
    // assert(in_buffer_length == packet_length * buffer_frame_num_); // should
    // be integer
    buffer_length_ = in_buffer_length;
    tx_buffer_ = in_tx_buffer; // for save data
    tx_buffer_status_ = in_tx_buffer_status; // for save status
    tx_buffer_frame_num_ = in_tx_buffer_frame_num;
    // assert(in_tx_buffer_length == packet_length * buffer_frame_num_); //
    // should be integer
    tx_buffer_length_ = in_tx_buffer_length;
    // new thread
    // pin_to_core_with_offset(RX, core_id_, 0);

    if (!radioconfig_->radioStart())
        return false;

    printf("create TXRX threads\n");
    for (int i = 0; i < comm_thread_num_; i++) {
        pthread_t txrx_thread;
        auto context = new EventHandlerContext<PacketTXRX>;
        context->obj_ptr = this;
        context->id = i;
        if (pthread_create(&txrx_thread, NULL,
                pthread_fun_wrapper<PacketTXRX, &PacketTXRX::loopTXRX>, context)
            != 0) {
            perror("socket communication thread create failed");
            exit(0);
        }
    }

    // XXX OBCH XXX
    printf("create upper layer comm. threads (Source/Sink) \n");
    pthread_t srcsink_thread;
    auto context = new EventHandlerContext<PacketTXRX>;
    context->obj_ptr = this;
    context->id = 0;
    if (pthread_create(&srcsink_thread, NULL, pthread_fun_wrapper<PacketTXRX, &PacketTXRX::loopSRCSINK>, context) != 0) {
        perror("socket src/sink communication thread create failed");
        exit(0);
    }
    // XXX OBCH END XXX

    sleep(1);
    pthread_cond_broadcast(&cond);
    // sleep(1);
    radioconfig_->go();
    return true;
}


// XXX OBCH XXX
void* PacketTXRX::loopSRCSINK(int tid)
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

    /* Read from source */
    char* buffer_ptr = (*buffer_)[tid];
    int* buffer_status_ptr = (*buffer_status_)[tid];
    long long buffer_length = buffer_length_;
    int buffer_frame_num = buffer_frame_num_;
    char* cur_buffer_ptr = buffer_ptr;
    int* cur_buffer_status_ptr = buffer_status_ptr;
    socklen_t addrlen = sizeof(remote_addr_[tid]);
    size_t offset = 0;
    /* Read from source END */



    /* Write to sink */
    int packet_length = config_->packet_length;
    char* cur_buffer_ptr_up = tx_buffer_ + socket_subframe_offset * packet_length;
    /* Write to sink END */


    while (config_->running) {

        /* 
         READ from source packet queue
         (1) Check if we are ready to process next frame
         (2) Check if frame available at upper phy queue
         (3) Read frame and put into cur_buffer_ptr
         (4) Reshape frame (let's "broadcast", replicate depending on number of UEs)
         */

        /* if buffer is full, exit */
        if (cur_buffer_status_ptr[0] == 1) {
            printf("Receive thread %d buffer full, offset: %zu\n", tid, offset);
            exit(0);
        }

        int recvlen = -1;
        if ((recvlen = recvfrom(socket_local, (char*)cur_buffer_ptr,
                 cfg->packet_length, 0, (struct sockaddr*)&remote_addr_[tid],  // TODO - packet length
                 &addrlen))
            < 0) {
            perror("recv failed");
            exit(0);
        }

        char* pkt_dwn;
        pkt_dwn = cur_buffer_ptr;

        /* get the position in buffer */
        offset = cur_buffer_status_ptr - buffer_status_ptr;
        cur_buffer_status_ptr[0] = 1;
        cur_buffer_status_ptr
            = buffer_status_ptr + (offset + 1) % buffer_frame_num;
        cur_buffer_ptr = buffer_ptr
            + (cur_buffer_ptr - buffer_ptr + cfg->packet_length)
                % buffer_length;

        /* Push packet-received-from-source event into the queue */
        Event_data packet_message(
            EventType::kFromSrc, rx_tag_t(tid, offset)._tag);

        if (!message_queue_->enqueue(*local_ptok, packet_message)) {
            printf("socket message enqueue failed\n");
            exit(0);
        }

        /* Reshape here ??? TODO, or in millipede.cpp? */
        // THIS IS WHAT dl_IQ_data expects (Table<int8_t> dl_IQ_data;)       
        //dl_IQ_data.malloc(dl_data_symbol_num_perframe, OFDM_DATA_NUM * UE_ANT_NUM, 64);
        // Dimension 1: dl_data_symbol_num_perframe
        // Dimension 2: OFDM_DATA_NUM * UE_ANT_NUM, e.g., 100 * 2
        // [UE0_OFDM0, UE1_OFDM0, UE0_OFDM1 UE1_OFDM1]
        // FIXME - data from pkt_dwn
        for (size_t i = 0; i < dl_data_symbol_num_perframe; i++) {
            std::vector<int8_t> in_modul;
            for (size_t ue_id = 0; ue_id < UE_ANT_NUM; ue_id++) {
                for (size_t j = 0; j < OFDM_DATA_NUM; j++) {
                    int cur_offset = j * UE_ANT_NUM + ue_id;
                    dl_IQ_data[i][cur_offset] = rand() % mod_order;
                    if (ue_id == 0)
                        in_modul.push_back(dl_IQ_data[i][cur_offset]);
                }
            }
        }

        /* WRITE to sink packet queue
         (1) Get packet from decoder
         (2) Write to buffer used for transmission TODO */
        if (packet available in decoded_buffer_) {   // FIXME
            cur_buffer_ptr_up = (uint8_t*)decoded_buffer_[symbol_offset]; // XXX FIXME - ASSIGN XXX 
            if (sendto(socket_[tid], (char*)cur_buffer_ptr_up, packet_length, 0,
                    (struct sockaddr*)&servaddr_[tid], sizeof(servaddr_[tid]))
                < 0) {
                perror("socket sendto failed");
                exit(0);
            }

            /* Push packet-sent-to-sink-event into the queue */
            Event_data packet_message(
                EventType::kToSink, rx_tag_t(tid, offset)._tag);

            if (!message_queue_->enqueue(*local_ptok, packet_message)) {
                printf("socket message enqueue failed\n");
                exit(0);
            }
        }
    }
    return 0;
}
// XXX OBCH END XXX



void* PacketTXRX::loopTXRX(int tid)
{
    pin_to_core_with_offset(ThreadType::kWorkerTXRX, core_id_, tid);
    double* rx_frame_start = (*frame_start_)[tid];
    int rx_offset = 0;
    // printf("Recv thread: thread %d start\n", tid);
    int radio_lo = tid * config_->nRadios / comm_thread_num_;
    int radio_hi = (tid + 1) * config_->nRadios / comm_thread_num_;
    int nradio_cur_thread = radio_hi - radio_lo;
    // printf("receiver thread %d has %d radios\n", tid, nradio_cur_thread);
    // get pointer of message queue

    //// Use mutex to sychronize data receiving across threads
    pthread_mutex_lock(&mutex);
    printf("Thread %d: waiting for release\n", tid);

    pthread_cond_wait(&cond, &mutex);
    pthread_mutex_unlock(&mutex); // unlocking for all other threads

    // downlink socket buffer
    // char *tx_buffer_ptr = tx_buffer_;
    // char *tx_cur_buffer_ptr;
#if DEBUG_DOWNLINK
    size_t txSymsPerFrame = config_->dl_data_symbol_num_perframe;
    std::vector<size_t> txSymbols = config_->DLSymbols[0];
    std::vector<std::complex<int16_t>> zeros(config_->sampsPerSymbol);
#endif

    printf("receiver thread %d has %d radios\n", tid, nradio_cur_thread);

    // to handle second channel at each radio
    // this is assuming buffer_frame_num_ is at least 2
    int prev_frame_id = -1;
    int nChannels = config_->nChannels;
    int radio_id = radio_lo;
    while (config_->running) {
        // receive data
        if (-1 != dequeue_send(tid))
            continue;
        rx_offset = (rx_offset + nChannels) % buffer_frame_num_;
        struct Packet* pkt = recv_enqueue(tid, radio_id, rx_offset);
        if (pkt == NULL)
            continue;
        int frame_id = pkt->frame_id;
#if MEASURE_TIME
        // read information from received packet
        // frame_id = *((int *)cur_ptr_buffer);
        if (frame_id > prev_frame_id) {
            *(rx_frame_start + frame_id) = get_time();
            prev_frame_id = frame_id;
            if (frame_id % 512 == 200) {
                _mm_prefetch(
                    (char*)(rx_frame_start + frame_id + 512), _MM_HINT_T0);
            }
        }
#endif
#if DEBUG_RECV
        printf("PacketTXRX %d: receive frame_id %d, symbol_id %d, ant_id "
               "%d, offset %d\n",
            tid, pkt->frame_id, pkt->symbol_id, pkt->ant_id, rx_offset);
#endif

        if (++radio_id == radio_hi)
            radio_id = radio_lo;
    }
    return 0;
}

struct Packet* PacketTXRX::recv_enqueue(int tid, int radio_id, int rx_offset)
{
    moodycamel::ProducerToken* local_ptok = rx_ptoks_[tid];
    char* rx_buffer = (*buffer_)[tid];
    int* rx_buffer_status = (*buffer_status_)[tid];
    int packet_length = config_->packet_length;
    int nChannels = config_->nChannels;
    struct Packet* pkt[nChannels];
    void* samp[nChannels];
    for (int ch = 0; ch < nChannels; ++ch) {
        // if rx_buffer is full, exit
        if (rx_buffer_status[rx_offset + ch] == 1) {
            printf("Receive thread %d rx_buffer full, offset: %d\n", tid,
                rx_offset);
            config_->running = false;
            break;
        }
        pkt[ch] = (struct Packet*)&rx_buffer[(rx_offset + ch) * packet_length];
        samp[ch] = pkt[ch]->data;
    }

    // TODO: this is probably a really bad implementation, and needs to be
    // revamped
    long long frameTime;
    if (!config_->running
        || radioconfig_->radioRx(radio_id, samp, frameTime) <= 0) {
        return NULL;
    }

    int frame_id = (int)(frameTime >> 32);
    int symbol_id = (int)((frameTime >> 16) & 0xFFFF);
    int ant_id = radio_id * nChannels;

    for (int ch = 0; ch < nChannels; ++ch) {
        new (pkt[ch]) Packet(frame_id, symbol_id, 0 /* cell_id */, ant_id + ch);
        // move ptr & set status to full
        rx_buffer_status[rx_offset + ch]
            = 1; // has data, after it is read, it is set to 0

        // Push kPacketRX event into the queue.
        Event_data rx_message(
            EventType::kPacketRX, rx_tag_t(tid, rx_offset + ch)._tag);

        if (!message_queue_->enqueue(*local_ptok, rx_message)) {
            printf("socket message enqueue failed\n");
            exit(0);
        }
    }
    return pkt[0];
}

int PacketTXRX::dequeue_send(int tid)
{
    Event_data task_event;
    if (!task_queue_->try_dequeue_from_producer(*tx_ptoks_[tid], task_event))
        return -1;

    // printf("tx queue length: %d\n", task_queue_->size_approx());
    if (task_event.event_type != EventType::kPacketTX) {
        printf("Wrong event type!");
        exit(0);
    }

    int BS_ANT_NUM = config_->BS_ANT_NUM;
    int data_subframe_num_perframe = config_->data_symbol_num_perframe;
    int packet_length = config_->packet_length;
    int offset = task_event.data;
    int ant_id = offset % BS_ANT_NUM;
    int symbol_id = offset / BS_ANT_NUM % data_subframe_num_perframe;
    symbol_id += config_->UE_NUM;
    int frame_id = offset / (BS_ANT_NUM * data_subframe_num_perframe);

#if DEBUG_BS_SENDER
    printf("In TX thread %d: Transmitted frame %d, subframe %d, "
           "ant %d, offset: %d, msg_queue_length: %zu\n",
        tid, frame_id, symbol_id, ant_id, offset,
        message_queue_->size_approx());
#endif

    int socket_subframe_offset = offset
        % (SOCKET_BUFFER_FRAME_NUM * data_subframe_num_perframe * BS_ANT_NUM);
    char* cur_buffer_ptr = tx_buffer_ + socket_subframe_offset * packet_length;
    struct Packet* pkt = (struct Packet*)cur_buffer_ptr;
    char* tx_cur_buffer_ptr = (char*)pkt->data;
    frame_id += TX_FRAME_DELTA;

    void* txbuf[2];
    long long frameTime = ((long long)frame_id << 32) | (symbol_id << 16);
    int nChannels = config_->nChannels;
    int ch = ant_id % nChannels;
#if DEBUG_DOWNLINK
    std::vector<std::complex<int16_t>> zeros(config_->sampsPerSymbol);
    if (ant_id != (int)config_->ref_ant)
        txbuf[ch] = zeros.data();
    else if (config_->getDownlinkPilotId(frame_id, symbol_id) >= 0)
        txbuf[ch] = config_->pilot_ci16.data();
    else
        txbuf[ch] = (void*)config_->dl_IQ_symbol[offset / BS_ANT_NUM
            % data_subframe_num_perframe];
#else
    txbuf[ch] = tx_cur_buffer_ptr + ch * packet_length;
#endif
    int last = config_->isUE ? config_->ULSymbols[0].back()
                             : config_->DLSymbols[0].back();
    int flags = (symbol_id != last) ? 1 // HAS_TIME
                                    : 2; // HAS_TIME & END_BURST, fixme
    radioconfig_->radioTx(ant_id / nChannels, txbuf, flags, frameTime);

    Event_data tx_message(EventType::kPacketTX, offset);
    moodycamel::ProducerToken* local_ptok = rx_ptoks_[tid];
    if (!message_queue_->enqueue(*local_ptok, tx_message)) {
        printf("socket message enqueue failed\n");
        exit(0);
    }
    return offset;
}
