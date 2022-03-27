#ifndef __YUNI_MINISOCK_H
#define __YUNI_MINISOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
/* } */

enum msck_session_type_e {
    MSCK_SESSION_TYPE_STREAM,
    MSCK_SESSION_TYPE_STREAM_SERVER,
    MSCK_SESSION_TYPE_DATAGRAM
};
typedef enum msck_session_type_e msck_session_type_t;

enum msck_name_type_e {
    MSCK_NAME_TYPE_VIRTUAL,
    MSCK_NAME_TYPE_IPV4,
    MSCK_NAME_TYPE_IPV6,
    MSCK_NAME_TYPE_DNS,
    MSCK_NAME_TYPE_DNS_IPV4,
    MSCK_NAME_TYPE_DNS_IPV6
};
typedef enum msck_name_type_e msck_name_type_t;

enum msck_event_e {
    MSCK_EVENT_TYPE_SESSION_CREATE_RESULT,
    MSCK_EVENT_TYPE_SESSION_SEND_RESULT,
    MSCK_EVENT_TYPE_SESSION_INCOMING,
    MSCK_EVENT_TYPE_SESSION_DATA,
    MSCK_EVENT_TYPE_SESSION_TERMINATE
};
typedef enum msck_event_e msck_event_t;

enum msck_error_e {
    MSCK_SUCCESS = 0,
    MSCK_ERROR_INVALID_ARGUMENT,
    MSCK_ERROR_UNIMPLEMENTED,
    MSCK_ERROR_BUSY,
    MSCK_ERROR_MAX_SESSION, /* FIXME: Rename? */
    MSCK_ERROR_BACKEND,
    MSCK_ERROR_NAME_LOOKUP,
};

typedef enum msck_error_e msck_error_t;


typedef struct msck_ctx_s msck_ctx_t;
typedef struct msck_session_s msck_session_t;

typedef void (*msck_ctx_callback_t)(msck_ctx_t* ctx,
                                    msck_event_t type,
                                    msck_error_t err,
                                    msck_session_t* session,
                                    const char* buf, uintptr_t arg0,
                                    uintptr_t data_ctx, uintptr_t data_session);
int msck_ctx_create_default(msck_ctx_callback_t cb, uintptr_t data, msck_ctx_t** out_ctx);
void msck_ctx_destroy(msck_ctx_t* ctx);
void msck_ctx_step(msck_ctx_t* ctx, int waitok);

int msck_session_create(msck_ctx_t* ctx,
                        msck_session_type_t st,
                        msck_name_type_t nt,
                        const char* name, size_t namelen,
                        uintptr_t arg0, uintptr_t arg1,
                        uintptr_t data,
                        msck_session_t** out_session);
void msck_session_destroy(msck_ctx_t* ctx, msck_session_t* session);

int msck_session_accept(msck_ctx_t* ctx, msck_session_t* session,
                        uintptr_t data, msck_session_t** out_newsession);

int msck_session_write(msck_ctx_t* ctx, msck_session_t* session,
                       const char* data, size_t datalen, size_t* out_count);
int msck_session_read(msck_ctx_t* ctx, msck_session_t* session,
                      char* buf, size_t buflen, size_t* out_count);


/* { */
#ifdef __cplusplus
};
#endif

#endif


