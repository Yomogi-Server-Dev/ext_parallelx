/* POSIX only. PHP 8.x API */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"

#include "parallelx.h"
#include "px_internal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#define PARALLELX_VERSION "0.1.0"

px_worker *workers = NULL;
int px_worker_count = 0;
int px_initialized = 0;
char worker_script_path[PATH_MAX] = {0};
char php_cli_path[PATH_MAX] = "php";
unsigned long next_task_id = 1;

pending_node *pending_head = NULL;
pending_node *pending_tail = NULL;
running_node *running_head = NULL;
closure_entry *closure_head = NULL;

/* -------------------- PHP API  -------------------- */

/* parallelx_init(workers, php_cli = null, worker_script = null, autoload = null) */
PHP_FUNCTION(parallelx_init) {
    zend_long workers_z = 0;
    char *php_bin = NULL;
    size_t php_bin_len = 0;
    char *user_script = NULL;
    size_t user_script_len = 0;
    char *autoload = NULL;
    size_t autoload_len = 0;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "l|s!s!s!", &workers_z, &php_bin, &php_bin_len, &user_script,
                              &user_script_len, &autoload, &autoload_len) == FAILURE) {
        RETURN_FALSE;
    }
    if (workers_z <= 0) workers_z = 2;
    if (workers_z > PARALLELX_MAX_WORKERS) workers_z = PARALLELX_MAX_WORKERS;

    if (px_initialized) {
        php_error_docref(NULL, E_NOTICE, "parallelx already initialized");
        RETURN_FALSE;
    }

    if (php_bin && php_bin_len > 0) {
        strncpy(php_cli_path, php_bin, sizeof(php_cli_path) - 1);
        php_cli_path[sizeof(php_cli_path) - 1] = '\0';
    }

    if (autoload && autoload_len > 0) {
        setenv(ENV_AUTLOAD, autoload, 1);
    }

    if (px_create_worker_script_if_missing(user_script ? user_script : NULL) != 0) {
        php_error_docref(NULL, E_WARNING, "parallelx: failed to create/find worker script");
        RETURN_FALSE;
    }

    if (px_spawn_workers((int) workers_z) != 0) {
        php_error_docref(NULL, E_WARNING, "parallelx: spawn_workers failed");
        RETURN_FALSE;
    }

    px_initialized = 1;
    RETURN_TRUE;
}

/* parallelx_register(source, bound_b64) -> token */
PHP_FUNCTION(parallelx_register) {
    char *source = NULL;
    size_t source_len = 0;
    char *bound_b64 = NULL;
    size_t bound_len = 0;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "s|s", &source, &source_len, &bound_b64, &bound_len) == FAILURE) {
        RETURN_FALSE;
    }
    if (!px_initialized) {
        php_error_docref(NULL, E_WARNING, "parallelx: not initialized");
        RETURN_FALSE;
    }
    char *token = px_registry_insert(source, bound_b64 ? bound_b64 : "");
    if (!token) {
        php_error_docref(NULL, E_WARNING, "parallelx_register: out of memory");
        RETURN_FALSE;
    }
    RETVAL_STRING(token);
}

/* parallelx_submit_desc(descriptor_array, callable) */
PHP_FUNCTION(parallelx_submit_desc) {
    zval *desc = NULL;
    zval *callback = NULL;
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "zz", &desc, &callback) == FAILURE) {
        RETURN_FALSE;
    }
    if (Z_TYPE_P(desc) != IS_ARRAY) {
        php_error_docref(NULL, E_WARNING, "parallelx_submit_desc: descriptor must be array");
        RETURN_FALSE;
    }
    if (!zend_is_callable(callback, 0, NULL)) {
        php_error_docref(NULL, E_WARNING, "parallelx_submit_desc: second param must be callable");
        RETURN_FALSE;
    }
    if (!px_initialized) {
        php_error_docref(NULL, E_WARNING, "parallelx_submit_desc: not initialized");
        RETURN_FALSE;
    }

    unsigned long tid = next_task_id++;

    char *json_payload = NULL;
    size_t payload_len = 0;
    if (px_encode_descriptor_with_task(desc, tid, &json_payload, &payload_len) != SUCCESS) {
        php_error_docref(NULL, E_WARNING, "parallelx_submit_desc: json_encode failed");
        RETURN_FALSE;
    }

    if (px_enqueue_payload(tid, json_payload, payload_len, callback) != SUCCESS) {
        efree(json_payload);
        php_error_docref(NULL, E_WARNING, "parallelx_submit_desc: enqueue failed");
        RETURN_FALSE;
    }

    RETURN_TRUE;
}

/* parallelx_submit_token(token, args_array, callable) */
PHP_FUNCTION(parallelx_submit_token) {
    char *token = NULL;
    size_t token_len = 0;
    zval *args = NULL;
    zval *callback = NULL;
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "szz", &token, &token_len, &args, &callback) == FAILURE) {
        RETURN_FALSE;
    }
    if (!zend_is_callable(callback, 0, NULL)) {
        php_error_docref(NULL, E_WARNING, "parallelx_submit_token: third param must be callable");
        RETURN_FALSE;
    }
    if (Z_TYPE_P(args) != IS_ARRAY) {
        php_error_docref(NULL, E_WARNING, "parallelx_submit_token: args must be array");
        RETURN_FALSE;
    }
    if (!px_initialized) {
        php_error_docref(NULL, E_WARNING, "parallelx_submit_token: not initialized");
        RETURN_FALSE;
    }
    closure_entry *e = px_registry_find(token);
    if (!e) {
        php_error_docref(NULL, E_WARNING, "parallelx_submit_token: token not found");
        RETURN_FALSE;
    }

    unsigned long tid = next_task_id++;

    zval desc;
    array_init(&desc);
    add_assoc_string(&desc, "type", "closure_exec");
    add_assoc_string(&desc, "source", e->source ? e->source : "");
    add_assoc_string(&desc, "bound_b64", e->bound_b64 ? e->bound_b64 : "");
    zval args_copy;
    ZVAL_COPY(&args_copy, args);
    add_assoc_zval(&desc, "args", &args_copy);

    char *json_payload = NULL;
    size_t payload_len = 0;
    if (px_encode_descriptor_with_task(&desc, tid, &json_payload, &payload_len) != SUCCESS) {
        zval_ptr_dtor(&desc);
        php_error_docref(NULL, E_WARNING, "parallelx_submit_token: json_encode failed");
        RETURN_FALSE;
    }

    if (px_enqueue_payload(tid, json_payload, payload_len, callback) != SUCCESS) {
        efree(json_payload);
        zval_ptr_dtor(&desc);
        php_error_docref(NULL, E_WARNING, "parallelx_submit_token: enqueue failed");
        RETURN_FALSE;
    }

    zval_ptr_dtor(&desc);
    RETURN_TRUE;
}

/* parallelx_poll() - PMMPのメインスレッド側でポール(1tickごとの呼出しが理想) */
PHP_FUNCTION(parallelx_poll) {
    if (zend_parse_parameters_none() == FAILURE) RETURN_FALSE;
    if (!px_initialized) RETURN_FALSE;

    for (int i = 0; i < px_worker_count; ++i) {
        px_worker *w = &workers[i];

        px_read_from_worker(w);
        if (w->dead) {
            if (w->busy && w->current_task_id) {
                px_fail_task(w->current_task_id, "worker died");
            }
            px_restart_worker(i);
            continue;
        }

        while (1) {
            char *payload = NULL;
            size_t payload_len = 0;
            int ex = px_try_extract(w, &payload, &payload_len);
            if (ex == 0) break;
            if (ex < 0) {
                if (w->busy && w->current_task_id) {
                    px_fail_task(w->current_task_id, "protocol error");
                }
                px_restart_worker(i);
                break;
            }

            zval result;
            if (px_decode_worker_json(payload, payload_len, &result) != SUCCESS) {
                php_error_docref(NULL, E_WARNING, "parallelx: json_decode failed");
                w->busy = 0;
                w->current_task_id = 0;
                px_assign_pending(w);
                free(payload);
                continue;
            }

            if (Z_TYPE(result) == IS_ARRAY) {
                zval *ztid = zend_hash_str_find(Z_ARRVAL(result), "task_id", sizeof("task_id") - 1);
                unsigned long tid = 0;
                if (ztid) {
                    if (Z_TYPE_P(ztid) == IS_LONG) tid = (unsigned long) Z_LVAL_P(ztid);
                    else if (Z_TYPE_P(ztid) == IS_STRING) tid = strtoul(Z_STRVAL_P(ztid), NULL, 10);
                }
                zval *cb = px_running_pop(tid);
                if (cb) {
                    px_invoke_callback(cb, &result);
                    zval_ptr_dtor(cb);
                    efree(cb);
                } else {
                    php_error_docref(NULL, E_NOTICE, "parallelx: callback not found for task_id %lu", tid);
                }

                if (w->current_task_id == tid) {
                    w->busy = 0;
                    w->current_task_id = 0;
                    px_assign_pending(w);
                }
            } else {
                php_error_docref(NULL, E_WARNING, "parallelx: worker returned non-array JSON");
                w->busy = 0;
                w->current_task_id = 0;
                px_assign_pending(w);
            }

            zval_ptr_dtor(&result);
            free(payload);
        }
    }

    px_dispatch_pending_to_idle();
    RETURN_TRUE;
}

/* parallelx_shutdown() */
PHP_FUNCTION(parallelx_shutdown) {
    if (zend_parse_parameters_none() == FAILURE) RETURN_FALSE;
    if (!px_initialized) RETURN_FALSE;

    for (int i = 0; i < px_worker_count; ++i) {
        if (workers[i].pid > 0) kill(workers[i].pid, SIGTERM);
    }
    for (int i = 0; i < px_worker_count; ++i) {
        if (workers[i].pid > 0) waitpid(workers[i].pid, NULL, 0);
        if (workers[i].to_child) close(workers[i].to_child);
        if (workers[i].from_child) close(workers[i].from_child);
        if (workers[i].recv_buf) free(workers[i].recv_buf);
    }
    free(workers);
    workers = NULL;
    px_worker_count = 0;
    px_initialized = 0;

    px_queue_free_all();

    px_registry_free_all();

    unsetenv(ENV_AUTLOAD);

    RETURN_TRUE;
}

PHP_MINFO_FUNCTION(parallelx) {
    php_info_print_table_start();
    php_info_print_table_row(2, "parallelx support", "enabled");
    php_info_print_table_row(2, "parallelx version", PARALLELX_VERSION);
    php_info_print_table_row(2, "worker script", worker_script_path[0] ? worker_script_path : "not created");
    php_info_print_table_end();
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_parallelx_init, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_parallelx_register, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_parallelx_submit_token, 0, 0, 2)
    ZEND_ARG_CALLABLE_INFO(0, task, 0)
    ZEND_ARG_CALLABLE_INFO(0, callback, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_parallelx_submit_desc, 0, 0, 2)
    ZEND_ARG_INFO(0, desc)
    ZEND_ARG_CALLABLE_INFO(0, callback, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_parallelx_poll, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_parallelx_shutdown, 0, 0, 0)
ZEND_END_ARG_INFO()

const zend_function_entry parallelx_functions[] = {
    PHP_FE(parallelx_init, arginfo_parallelx_init)
    PHP_FE(parallelx_register, arginfo_parallelx_register)
    PHP_FE(parallelx_submit_token, arginfo_parallelx_submit_token)
    PHP_FE(parallelx_submit_desc, arginfo_parallelx_submit_desc)
    PHP_FE(parallelx_poll, arginfo_parallelx_poll)
    PHP_FE(parallelx_shutdown, arginfo_parallelx_shutdown)
    PHP_FE_END
};

zend_module_entry parallelx_module_entry = {
    STANDARD_MODULE_HEADER,
    "parallelx",
    parallelx_functions,
    NULL, NULL, NULL, NULL,
    PHP_MINFO(parallelx),
    PARALLELX_VERSION,
    STANDARD_MODULE_PROPERTIES
};

ZEND_GET_MODULE(parallelx)
