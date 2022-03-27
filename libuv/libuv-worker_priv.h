struct msck_session_s {
    int id;
    int next;
    enum {
        SESSION_FREE,
        SESSION_IDLE, /* handle valid, Waiting for command */

        /* Waiting for libuv callback */
        SESSION_ACTIVE, /* Listen, Write callbacks */
        SESSION_CONNECTING,
        SESSION_IN_GAI,

        /* Fail */
        SESSION_DEFUNCT
    } session_state;
    uv_loop_t* loop;
    union {
        /* Handle */
        uv_stream_t stream;
        uv_udp_t udp;
        uv_tcp_t tcp;
    } handle;
    union {
        /* req */
        uv_getaddrinfo_t gai;
        uv_connect_t tcp_connect;
        uv_write_t write;
    } req;
    int port0;
    int port1;
    int read_active;
    uintptr_t data;
    msck_session_type_t session_type;
    uv_buf_t recvq[2];
    size_t readhead;
};


#define MAX_SESSIONS (64*1024)
struct msck_ctx_s {
    uv_loop_t loop;
    uv_prepare_t prepare;
    uintptr_t data;
    msck_ctx_callback_t cb;
    int done;
    int in_destroy;
    int in_loop;

    int queue_udp_ready;
    int queue_free;
    msck_session_t sessions[MAX_SESSIONS];
};

