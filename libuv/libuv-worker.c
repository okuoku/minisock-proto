#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <uv.h>
#include "minisock.h"

#include "libuv-worker_priv.h"

void
ensure_in_loop(msck_ctx_t* ctx){
    if(! ctx->in_loop){
        abort();
    }
}

/*
 * SESSION
 */

static void
free_session(msck_ctx_t* ctx, msck_session_t* s){
    /* Return session back to the free list */
    s->session_state = SESSION_FREE;
    s->next = ctx->queue_free;
    ctx->queue_free = s->id;
}

static void
cb_alloc_stream_read(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf){
    buf->base = malloc(suggested_size);
    buf->len = suggested_size;
}

static void
cb_stream_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf){
    size_t total;
    char* p;
    size_t offs;
    int i;
    int sel;
    msck_session_t* s;
    uv_loop_t* loop;
    msck_ctx_t* ctx;
    s = (msck_session_t*)stream->data;
    loop = s->loop;
    ctx = loop->data;
    ensure_in_loop(ctx);
    if(nread == 0){
        /* Do nothing */
        return;
    }
    if(nread < 0){
        (void)uv_read_stop(stream);
        s->session_state = SESSION_DEFUNCT;
        free(s->recvq[0].base);
        free(s->recvq[1].base);
        /* Error case */
        ctx->cb(ctx, MSCK_EVENT_TYPE_SESSION_TERMINATE,
                MSCK_ERROR_BACKEND, s,
                0, (uintptr_t)nread, ctx->data, s->data);
        return;
    }
    if(s->recvq[0].base){
        s->read_active = 0;
        (void)uv_read_stop(stream);
        sel = 1;
    }else{
        sel = 0;
    }
    if(nread > 1){
        /* Merge buffers into one buffer */
        total = 0;
        for(i=0;i!=nread;i++){
            total += buf[i].len;
        }
        p = malloc(total);
        offs = 0;
        for(i=0;i!=nread;i++){
            memcpy(p + offs, buf[i].base, buf[i].len);
            free(buf[i].base);
            offs += buf[i].len;
        }
        s->recvq[sel].base = p;
        s->recvq[sel].len = total;
    }else{
        s->recvq[sel] = buf[0];
    }
    if(sel == 0){
        s->readhead = 0;
    }
    ctx->cb(ctx, MSCK_EVENT_TYPE_SESSION_INCOMING,
            MSCK_SUCCESS, s, 0, 0, ctx->data, s->data);
}

static int /* backend error */
stream_resume_read(msck_session_t* session){
    int r;
    session->read_active = 1;
    r = uv_read_start(&session->handle.stream, cb_alloc_stream_read,
                      cb_stream_read);
    return r;
}

int
msck_session_read(msck_ctx_t* ctx, msck_session_t* session,
                  char* buf, size_t buflen, size_t* out_count){
    int r;
    size_t cur, ncur;
    if(! session->recvq[0].base){
        *out_count = 0;
        return MSCK_SUCCESS;
    }
    cur = session->recvq[0].len - session->readhead;
    if(buflen < cur){
        memcpy(buf, session->recvq[0].base + session->readhead, buflen);
        session->readhead += buflen;
        *out_count = buflen;
    }else{
        memcpy(buf, session->recvq[0].base + session->readhead, cur);
        free(session->recvq[0].base);
        session->recvq[0] = session->recvq[1];
        session->recvq[1].base = 0;
        session->recvq[1].len = 0;
        if(session->recvq[0].base){
            if((buflen - cur) < session->recvq[0].len){
                ncur = buflen - cur;
                memcpy(buf + cur, session->recvq[0].base, ncur);
                *out_count = buflen;
                session->readhead = ncur;
            }else{
                ncur = session->recvq[0].len;
                memcpy(buf + cur, session->recvq[0].base, ncur);
                free(session->recvq[0].base);
                session->recvq[0].base = 0;
                session->readhead = 0;
                *out_count = cur + ncur;
            }
        }else{
            session->readhead = 0;
        }
    }
    if(! session->recvq[1].base){
        if(! session->read_active){
            r = stream_resume_read(session);
            if(! r){
                session->read_active = 1;
            }
        }
    }
    return MSCK_SUCCESS;

}

static int /* backend error */
stream_start_read(msck_session_t* session){
    session->recvq[0].base = 0;
    session->recvq[1].base = 0;
    session->readhead = 0;
    return stream_resume_read(session);
}


struct send_task {
    msck_ctx_t* ctx;
    uv_buf_t buf;
    msck_session_t* s;
};

static void
cb_write(uv_write_t* req, int status){
    struct send_task* t;
    msck_session_t* s;
    msck_ctx_t* ctx;
    t = (struct send_task*)req->data;
    s = t->s;
    ctx = t->ctx;
    ensure_in_loop(ctx);
    free(t);
    if(status){
        s->session_state = SESSION_DEFUNCT;
        ctx->cb(ctx, MSCK_EVENT_TYPE_SESSION_SEND_RESULT,
                MSCK_ERROR_BACKEND, s,
                0, status, ctx->data, s->data);
    }else{
        s->session_state = SESSION_IDLE;
        ctx->cb(ctx, MSCK_EVENT_TYPE_SESSION_SEND_RESULT,
                MSCK_SUCCESS, s,
                0, 0, ctx->data, s->data);
    }
}

int
msck_session_write(msck_ctx_t* ctx, msck_session_t* session,
                   const char* data, size_t datalen, size_t* out_count){
    char* p;
    struct send_task* t;
    int r;
    if(session->session_type == MSCK_SESSION_TYPE_STREAM){
        if(session->session_state != SESSION_IDLE){
            return MSCK_ERROR_BUSY;
        }
        if(datalen > ((size_t)SSIZE_MAX + sizeof(struct send_task))){
            return MSCK_ERROR_INVALID_ARGUMENT;
        }
        p = malloc(sizeof(struct send_task) + datalen);
        t = (struct send_task *)p;
        if(! t){
            return MSCK_ERROR_BACKEND;
        }
        t->ctx = ctx;
        t->s = session;
        t->buf.base = p + sizeof(struct send_task);
        memcpy(t->buf.base, data, datalen);
        t->buf.len = datalen;
        session->session_state = SESSION_ACTIVE;
        r = uv_write(&session->req.write, &session->handle.stream, 
                     &t->buf, 1, cb_write);
        session->req.write.data = p;
        if(r){
            free(p);
            return MSCK_ERROR_BACKEND;
        }
        *out_count = datalen;
        return MSCK_SUCCESS;
    }else{
        return MSCK_ERROR_INVALID_ARGUMENT;
    }
}

int /* MSCK error */
msck_session_accept(msck_ctx_t* ctx, msck_session_t* session, uintptr_t data,
                    msck_session_t** out_newsession){
    msck_session_t* s2;
    int sid;
    int r;

    if(session->session_type == MSCK_SESSION_TYPE_STREAM_SERVER){
        /* Pick up a free session */
        sid = ctx->queue_free;
        if(sid < 0){
            return MSCK_ERROR_MAX_SESSION;
        }
        s2 = &ctx->sessions[sid];
        ctx->queue_free = s2->next;
        s2->port0 = 0; /* FIXME: Fill it? */
        s2->port1 = 0;
        s2->data = data;

        s2->session_type = MSCK_SESSION_TYPE_STREAM;
        s2->session_state = SESSION_IDLE;
        r = uv_tcp_init(&ctx->loop, &s2->handle.tcp);
        if(r){
            free_session(ctx, s2);
            return MSCK_ERROR_BACKEND;
        }

        r = uv_accept(&session->handle.stream, &s2->handle.stream);
        if(r){
            free_session(ctx, s2);
            return MSCK_ERROR_BACKEND;
        }
        /* FIXME: Handle error here..? */
        (void)stream_start_read(s2);
        return MSCK_SUCCESS;
    }else{
        return MSCK_ERROR_INVALID_ARGUMENT;
    }
}


static void
cb_start_tcp(uv_connect_t* req, int status){
    msck_session_t* s;
    msck_ctx_t* ctx;
    uv_loop_t* loop;
    s = (msck_session_t*) req->data;
    loop = s->loop;
    ctx = loop->data;
    ensure_in_loop(ctx);

    if(status){
        s->session_state = SESSION_DEFUNCT;
        ctx->cb(ctx, MSCK_EVENT_TYPE_SESSION_CREATE_RESULT,
                MSCK_ERROR_BACKEND, s, 0, status, ctx->data, s->data);
    }else{
        s->session_state = SESSION_IDLE;
        ctx->cb(ctx, MSCK_EVENT_TYPE_SESSION_CREATE_RESULT,
                MSCK_SUCCESS, s, 0, 0, ctx->data, s->data);
    }
}

static int /* MSCK error */
start_tcp(msck_ctx_t* ctx, msck_session_t* s, const struct sockaddr* addr, 
          int allowfail){
    int r;
    r = uv_tcp_init(&ctx->loop, &s->handle.tcp);
    if(r){
        goto uv_fail;
    }
    s->handle.tcp.data = s;
    s->session_state = SESSION_CONNECTING;
    r = uv_tcp_connect(&s->req.tcp_connect, &s->handle.tcp,
                       addr, cb_start_tcp);
    if(r){
        goto uv_fail;
    }

    return MSCK_SUCCESS;

uv_fail:
    if(! allowfail){
        s->session_state = SESSION_DEFUNCT;
        ctx->cb(ctx, MSCK_EVENT_TYPE_SESSION_CREATE_RESULT,
                MSCK_ERROR_BACKEND, s, 0, r, ctx->data, s->data);
    }
    return MSCK_ERROR_BACKEND;
}

static void
cb_tcp_listen(uv_stream_t* stream, int status){
    msck_session_t* s;
    msck_ctx_t* ctx;
    uv_loop_t* loop;
    s = (msck_session_t*) stream->data;
    loop = s->loop;
    ctx = loop->data;
    ensure_in_loop(ctx);

    if(status){
        s->session_state = SESSION_DEFUNCT;
        ctx->cb(ctx, MSCK_EVENT_TYPE_SESSION_TERMINATE,
                MSCK_ERROR_BACKEND, s, 0, status, ctx->data, s->data);
    }else{
        ctx->cb(ctx, MSCK_EVENT_TYPE_SESSION_INCOMING,
                MSCK_SUCCESS, s, 0, 0, ctx->data, s->data);
    }
}

static int /* MSCK error */
start_tcp_listen(msck_ctx_t* ctx, msck_session_t* s,
                 const struct sockaddr* addr, int allowfail){
    int r;
    r = uv_tcp_init(&ctx->loop, &s->handle.tcp);
    if(r){
        goto uv_fail;
    }
    s->handle.tcp.data = s;
    s->session_state = SESSION_CONNECTING;
    r = uv_tcp_bind(&s->handle.tcp, addr, 0); /* FIXME: UV_TCP_IPV6ONLY */
    if(r){
        goto uv_fail;
    }
    s->handle.stream.data = s;
    r = uv_listen(&s->handle.stream, 20 /* FIXME: ??? */, cb_tcp_listen);
    if(r){
        goto uv_fail;
    }
    return MSCK_SUCCESS;

uv_fail:
    if(! allowfail){
        ctx->cb(ctx, MSCK_EVENT_TYPE_SESSION_CREATE_RESULT,
                MSCK_ERROR_BACKEND, s, 0, r, ctx->data, s->data);
    }
    return MSCK_ERROR_BACKEND;
}


union addr {
    struct sockaddr sa;
    struct sockaddr_in sin;
    struct sockaddr_in6 sin6;
};

static void
addr_fillport(int port, union addr* addr){
    switch(addr->sa.sa_family){
        case AF_INET:
            addr->sin.sin_port = htons(port);
            break;
        case AF_INET6:
            addr->sin6.sin6_port = htons(port);
            break;
        default:
            abort();
            break;
    }
}

static int /* MSCK error */
name_resolved(msck_ctx_t* ctx, msck_session_t* s, const struct sockaddr* addr,
              int allowfail){

    /* Start connection */
    switch(s->session_type){
        case MSCK_SESSION_TYPE_STREAM:
            return start_tcp(ctx, s, addr, allowfail);
        case MSCK_SESSION_TYPE_STREAM_SERVER:
            return start_tcp_listen(ctx, s, addr, allowfail);
        case MSCK_SESSION_TYPE_DATAGRAM:
            break;
        default:
            if(! allowfail){
                /* Unknown session_type for us */
                s->session_state = SESSION_DEFUNCT;
                ctx->cb(ctx, MSCK_EVENT_TYPE_SESSION_CREATE_RESULT,
                        MSCK_ERROR_INVALID_ARGUMENT,
                        s, 0, 0, ctx->data, s->data);
            }
            break;
    }
    return MSCK_ERROR_INVALID_ARGUMENT;
}


static void
cb_gai(uv_getaddrinfo_t* req, int status, 
       struct addrinfo* res){
    /* FIXME: How should we behave in dual-stack situation..? 
     *        (e.g. prefer IPv6 then implicitly try IPv4?) */
    uv_loop_t* loop;
    msck_ctx_t* ctx;
    msck_session_t* s;
    s = (msck_session_t*) req->data;
    loop = s->loop;
    ctx = loop->data;
    ensure_in_loop(ctx);

    if(status){
        /* Invoke error callback */
        s->session_state = SESSION_DEFUNCT;
        ctx->cb(ctx, MSCK_EVENT_TYPE_SESSION_CREATE_RESULT,
                MSCK_ERROR_NAME_LOOKUP, s, 0,
                status /* FIXME: decode backend error? */, 
                ctx->data, s->data);
    }else{
        /* Complete connection */
        switch(res->ai_family){
            case AF_INET:
            case AF_INET6:
                /* FIXME: Type pun */
                addr_fillport(s->port0, (union addr*)res->ai_addr);
                (void)name_resolved(ctx, s, res->ai_addr, 0);
                break;
            default:
                /* Invoke error callback */
                s->session_state = SESSION_DEFUNCT;
                ctx->cb(ctx, MSCK_EVENT_TYPE_SESSION_CREATE_RESULT,
                        MSCK_ERROR_NAME_LOOKUP,
                        s, 0, status /* FIXME: decode backend error? */, 
                        ctx->data, s->data);
                break;
        }
        uv_freeaddrinfo(res);
    }
}

int 
msck_session_create(msck_ctx_t* ctx,
                    msck_session_type_t st,
                    msck_name_type_t nt,
                    const char* name, size_t namelen,
                    uintptr_t arg0, uintptr_t arg1,
                    uintptr_t data,
                    msck_session_t** out_session){
    int sid;
    msck_session_t* s;
    int require_gai;
    int require_start_read;
    int r;
    union addr addr;
    char* namebuf;

    /* Check arguments first */
    switch(st){
        case MSCK_SESSION_TYPE_STREAM:
            require_start_read = 1;
            break;
        case MSCK_SESSION_TYPE_STREAM_SERVER:
        case MSCK_SESSION_TYPE_DATAGRAM:
            require_start_read = 0;
            break;

        default:
            return MSCK_ERROR_UNIMPLEMENTED;
    }

    switch(nt){
        case MSCK_NAME_TYPE_IPV4:
            memset(&addr, 0, sizeof(addr));
            if(namelen != 4){
                return MSCK_ERROR_INVALID_ARGUMENT;
            }
            require_gai = 0;
            addr.sin.sin_family = AF_INET;
            memcpy(&addr.sin.sin_addr, name, namelen);
            addr_fillport(arg0, &addr);
            break;
        case MSCK_NAME_TYPE_IPV6:
            memset(&addr, 0, sizeof(addr));
            if(namelen != 16){
                return MSCK_ERROR_INVALID_ARGUMENT;
            }
            require_gai = 0;
            addr.sin6.sin6_family = AF_INET6;
            memcpy(&addr.sin6.sin6_addr, name, namelen);
            addr_fillport(arg0, &addr);
            break;
        case MSCK_NAME_TYPE_DNS:
            require_gai = 1;
            break;

        case MSCK_NAME_TYPE_DNS_IPV4:
        case MSCK_NAME_TYPE_DNS_IPV6:
        default:
            return MSCK_ERROR_UNIMPLEMENTED;
    }

    if(arg0 > 65535){
        return MSCK_ERROR_INVALID_ARGUMENT;
    }
    if(arg1 > 65535){
        return MSCK_ERROR_INVALID_ARGUMENT;
    }

    /* Pick up a free session */
    sid = ctx->queue_free;
    if(sid < 0){
        return MSCK_ERROR_MAX_SESSION;
    }
    s = &ctx->sessions[sid];
    ctx->queue_free = s->next;
    s->session_type = st;
    s->port0 = arg0;
    s->port1 = arg1;
    s->data = data;

    if(require_gai){
        namebuf = malloc(namelen+1);
        if(! namebuf){
            free_session(ctx, s);
            return MSCK_ERROR_BACKEND;
        }
        memset(namebuf, 0, namelen+1);
        memcpy(namebuf, name, namelen);
        s->session_state = SESSION_IN_GAI;
        s->req.gai.data = s;
        r = uv_getaddrinfo(&ctx->loop, &s->req.gai, cb_gai, namebuf, NULL,
                           NULL);
        free(namebuf);
        if(r){
            free_session(ctx, s);
            return MSCK_ERROR_BACKEND;
        }else{
            *out_session = s;
        }
    }else{
        r = name_resolved(ctx, s, &addr.sa, 1);
        if(r){
            free_session(ctx, s);
            return r;
        }else{
            *out_session = s;
        }
    }
    if(require_start_read){
        stream_start_read(s);
    }
    return MSCK_SUCCESS;
}

void 
msck_session_destroy(msck_ctx_t* ctx, msck_session_t* session){

}

/* 
 * CTX
 */

static void
predispatch(uv_prepare_t* prepare){
    uv_loop_t* loop;
    msck_ctx_t* ctx;
    msck_session_t* s;
    loop = prepare->loop;
    ctx = loop->data;

    /* Check for queue destroy */
    if(ctx->in_destroy){
        // FIXME: Implement session destroy here.
        return;
    }

    /* Check for pre-start UDP sessions */
    while(ctx->queue_udp_ready >= 0){
        s = &ctx->sessions[ctx->queue_udp_ready];
        ctx->queue_udp_ready = s->next;
        /* Invoke UDP connect callback */
    }
}


int
msck_ctx_create_default(msck_ctx_callback_t cb,
                        uintptr_t data,
                        msck_ctx_t** out_ctx){
    int i;
    msck_ctx_t* res;
    res = malloc(sizeof(msck_ctx_t));
    for(i=0;i!=MAX_SESSIONS;i++){
        res->sessions[i].id = i;
        res->sessions[i].next = (i+1);
        res->sessions[i].session_state = SESSION_FREE;
        res->sessions[i].loop = &res->loop;
    }
    res->sessions[MAX_SESSIONS-1].next = -1;
    res->queue_udp_ready = -1;
    res->queue_free = 0;
    res->data = data;
    res->cb = cb;
    res->in_destroy = 0;
    res->in_loop = 0;
    res->done = 0;
    *out_ctx = res;
    uv_loop_init(&res->loop);
    res->loop.data = res;
    uv_prepare_init(&res->loop, &res->prepare);
    uv_prepare_start(&res->prepare, predispatch);
    return 0;
}

int
msck_ctx_destory(msck_ctx_t* ctx){
    if(! ctx->in_loop){
        uv_loop_close(&ctx->loop);
        free(ctx);
        return 0;
    }
    if(! ctx->in_destroy){
        ctx->in_destroy = 1;
        return 0;
    }
    /* Something wrong */
    return 0;
}

void /* FIXME: Should return runme status? */
msck_ctx_step(msck_ctx_t* ctx, int waitok){
    if(ctx->in_loop){
        /* I'm not reentrant: Something wrong */
        return;
    }
    if(ctx->in_destroy){
        /* Do nothing for in_destroy loop */
        return;
    }

    /* Single step */
    ctx->in_loop = 1;
    uv_run(&ctx->loop, waitok ? UV_RUN_ONCE : UV_RUN_NOWAIT);
    ctx->in_loop = 0;
}



