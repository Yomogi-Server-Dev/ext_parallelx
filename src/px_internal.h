/* internal shared definitions for ext_parallelx */
#ifndef PX_INTERNAL_H
#define PX_INTERNAL_H

#include "php.h"
#include "ext/json/php_json.h"
#include "zend_smart_str.h"

#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <sys/types.h>

#define PARALLELX_MAX_WORKERS 64
#define PARALLELX_MAX_MESSAGE (8 * 1024 * 1024)
#define WORKER_TEMPLATE "/var/tmp/parallelx_worker_XXXXXXphp"
#define ENV_AUTLOAD "PARALLELX_AUTOLOAD"

typedef struct px_worker {
    pid_t pid;
    int to_child;
    int from_child;
    char *recv_buf;
    size_t recv_used;
    size_t recv_cap;
    int busy;
    unsigned long current_task_id;
    int dead;
} px_worker;

typedef struct pending_node {
    unsigned long task_id;
    char *payload;
    size_t payload_len;
    zval *callback;
    struct pending_node *next;
} pending_node;

typedef struct running_node {
    unsigned long task_id;
    zval *callback;
    struct running_node *next;
} running_node;

typedef struct closure_entry {
    char *token;
    char *source;
    char *bound_b64;
    struct closure_entry *next;
} closure_entry;

/* global state (defined in src/parallelx.c) */
extern px_worker *workers;
extern int px_worker_count;
extern int px_initialized;
extern char worker_script_path[PATH_MAX];
extern char php_cli_path[PATH_MAX];
extern unsigned long next_task_id;

extern pending_node *pending_head;
extern pending_node *pending_tail;
extern running_node *running_head;
extern closure_entry *closure_head;

/* json */
zend_result px_encode_descriptor_with_task(zval *desc, unsigned long tid, char **out, size_t *out_len);
zend_result px_decode_worker_json(const char *payload, size_t len, zval *out);

/* queue/callback */
void px_invoke_callback(zval *cb, zval *assoc);
void px_fail_task(unsigned long tid, const char *message);
zval *px_running_pop(unsigned long id);
zend_result px_enqueue_payload(unsigned long tid, char *payload, size_t payload_len, zval *callback);
void px_dispatch_pending_to_idle(void);
void px_queue_free_all(void);

/* registry */
closure_entry *px_registry_find(const char *token);
char *px_registry_insert(const char *source, const char *bound_b64);
void px_registry_free_all(void);

/* worker/process */
int px_create_worker_script_if_missing(const char *user_script);
int px_spawn_workers(int count);
px_worker *px_find_idle_worker(void);
int px_send_to_worker(px_worker *w, const char *json, size_t len, unsigned long tid);
int px_assign_pending(px_worker *w);
void px_read_from_worker(px_worker *w);
int px_try_extract(px_worker *w, char **payload_out, size_t *len_out);
int px_restart_worker(int idx);

/* misc */
char *px_strdup(const char *s);

#endif /* PX_INTERNAL_H */
