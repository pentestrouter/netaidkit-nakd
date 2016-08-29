#include <string.h>
#include <strings.h>
#include <json-c/json.h>
#include "httpd.h"
#include "module.h"
#include "config.h"
#include "log.h"
#include "workqueue.h"
#include "jsonrpc.h"
#include "request.h"
#include "thread.h"

#define PORT 8000

struct http_request {
    struct MHD_Connection *connection;
    json_tokener *jtok;
    json_object *jrequest;
    size_t request_len;
};

static struct MHD_Daemon *_daemon;

static struct http_request *_init_http_request(struct MHD_Connection *c) {
    struct http_request *req;
    nakd_assert((req = malloc(sizeof(struct http_request))) != NULL);
    req->connection = c;
    nakd_assert((req->jtok = json_tokener_new()) != NULL);
    req->jrequest = NULL;
    req->request_len = 0;
    return req;
}

static struct http_request *_free_http_request(struct http_request *c) {
    if (c->jrequest != NULL)
        json_object_put(c->jrequest);
    if (c->jtok != NULL)
        json_tokener_free(c->jtok);
    free(c);
}

static void _http_rpc_completion(json_object *jresponse, void *priv) {
    struct http_request *c = priv;

    const char *jrstr = json_object_to_json_string_ext(jresponse,
                                        JSON_C_TO_STRING_PRETTY);
    nakd_log(L_DEBUG, "Sending response: %s", jrstr);
    struct MHD_Response *mhd_response = MHD_create_response_from_buffer(
                 strlen(jrstr), (void *)(jrstr), MHD_RESPMEM_MUST_COPY);
    json_object_put(jresponse);

    MHD_queue_response(c->connection, MHD_HTTP_OK, mhd_response);
    MHD_destroy_response(mhd_response);
    MHD_resume_connection(c->connection);
    _free_http_request(c);
}

static void _http_rpc_timeout(void *priv) {
    struct http_request *req = priv;
    const char *jstr = json_object_to_json_string_ext(req->jrequest,
                                           JSON_C_TO_STRING_PRETTY);
    nakd_log(L_CRIT, "RPC timeout while handling: %s", jstr);
}

static int _http_handler(void *cls,
          struct MHD_Connection *connection,
          const char *url,
          const char *method,
          const char *version,
          const char *post_data, size_t *post_data_size, void **ptr) {
    if (strcmp(method, MHD_HTTP_METHOD_POST))
        return MHD_NO;

    if (*ptr == NULL) {
        struct http_request *req = _init_http_request(connection);
        *ptr = req;
        return MHD_YES;
    }

    /* second+ pass */
    int ret;
    struct http_request *req = *ptr;
    if (*post_data_size) {
        if (req->request_len += *post_data_size >
                  NAKD_JSONRPC_RCVMSGLEN_LIMIT) {
            nakd_log(L_NOTICE, "JSONRPC message longer than %d bytes, "
                       "disconnecting.", NAKD_JSONRPC_RCVMSGLEN_LIMIT);
            _free_http_request(req), *ptr = NULL;
            ret = MHD_NO;
        } else {
            json_object *jreq = json_tokener_parse_ex(req->jtok, post_data,
                                                          *post_data_size);
            enum json_tokener_error jerr = json_tokener_get_error(req->jtok);

            if (jerr == json_tokener_continue) {
                ret = MHD_YES;
            } else if (jerr == json_tokener_success) {
                req->jrequest = jreq;

                /* doesn't allocate memory */
                const char *jreq_string = json_object_to_json_string_ext(jreq,
                                                     JSON_C_TO_STRING_PRETTY);
                nakd_log(L_DEBUG, "Got a message: %s", jreq_string);

                MHD_suspend_connection(req->connection), *ptr = NULL;
                nakd_handle_message(req->jrequest, _http_rpc_completion,
                                                _http_rpc_timeout, req); 
                ret = MHD_YES;
            } else {
                json_object *jresponse = nakd_jsonrpc_response_error(NULL,
                                                       PARSE_ERROR, NULL);
                const char *jrstr = json_object_to_json_string_ext(jresponse,
                                                    JSON_C_TO_STRING_PRETTY);
                struct MHD_Response *mhd_response = MHD_create_response_from_buffer(
                             strlen(jrstr), (void *)(jrstr), MHD_RESPMEM_MUST_COPY);
                json_object_put(jresponse);

                ret = MHD_queue_response(connection, MHD_HTTP_BAD_REQUEST,
                                                            mhd_response);
                MHD_destroy_response(mhd_response);
                /* 
                 * reset ptr: we're ready for another request after this call
                 * completes
                 */
                _free_http_request(req), *ptr = NULL;
            }
        }
    } else {
        _free_http_request(req), *ptr = NULL;
        ret = MHD_YES;
    }
    return ret;
}

static void _httpd_logger(void *arg, const char *fmt, va_list ap) {
    nakd_log_va(L_DEBUG, fmt, ap);
}

static int _httpd_init(void) {
    _daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, PORT, NULL, NULL,
                              &_http_handler, NULL, MHD_USE_SUSPEND_RESUME, 
                                           MHD_OPTION_CONNECTION_LIMIT, 64,
                                         MHD_OPTION_CONNECTION_TIMEOUT, 10,
                      MHD_OPTION_THREAD_STACK_SIZE, NAKD_THREAD_STACK_SIZE,
                                            MHD_OPTION_THREAD_POOL_SIZE, 2,
                           MHD_OPTION_EXTERNAL_LOGGER, _httpd_logger, NULL,
                                                             MHD_USE_DEBUG,
                                                           MHD_OPTION_END);
    nakd_assert(_daemon != NULL);
    return 0;
}

static int _httpd_cleanup(void) {
    MHD_stop_daemon(_daemon);
    return 0;
}

static struct nakd_module module_httpd = {
    .name = "httpd",
    .deps = (const char *[]){ "config", "command", "workqueue", NULL },
    .init = _httpd_init,
    .cleanup = _httpd_cleanup
};
NAKD_DECLARE_MODULE(module_httpd);
