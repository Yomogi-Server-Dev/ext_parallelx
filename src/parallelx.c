/* POSIX only. PHP 8.x API */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "parallelx.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <time.h>
#include <limits.h>

#define PARALLELX_VERSION "0.1.0"
#define PARALLELX_MAX_WORKERS 64
#define WORKER_TEMPLATE "/tmp/parallelx_worker_XXXXXX.php"
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
} px_worker;

typedef struct pending_node {
    unsigned long task_id;
    char *payload;
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

static px_worker *workers = NULL;
static int px_worker_count = 0;
static int px_initialized = 0;
static char worker_script_path[PATH_MAX] = {0};
static char php_cli_path[PATH_MAX] = "php";
static unsigned long next_task_id = 1;

static pending_node *pending_head = NULL, *pending_tail = NULL;
static running_node *running_head = NULL;
static closure_entry *closure_head = NULL;

static ssize_t write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *) buf;
    size_t left = len;
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        left -= n;
        p += n;
    }
    return (ssize_t) len;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
}

static char *px_strdup(const char *s) {
    if (!s) return NULL;
    size_t l = strlen(s);
    char *p = (char *) malloc(l + 1);
    if (!p) return NULL;
    memcpy(p, s, l + 1);
    return p;
}

/* pending queue */
static void pending_push(pending_node *n) {
    n->next = NULL;
    if (!pending_tail) pending_head = pending_tail = n;
    else {
        pending_tail->next = n;
        pending_tail = n;
    }
}

static pending_node *pending_pop() {
    pending_node *n = pending_head;
    if (!n) return NULL;
    pending_head = n->next;
    if (!pending_head) pending_tail = NULL;
    n->next = NULL;
    return n;
}

/* running mapping */
static void running_add(unsigned long id, zval *cb) {
    running_node *n = (running_node *) malloc(sizeof(running_node));
    n->task_id = id;
    n->callback = cb;
    n->next = running_head;
    running_head = n;
}

static zval *running_pop(unsigned long id) {
    running_node *prev = NULL, *cur = running_head;
    while (cur) {
        if (cur->task_id == id) {
            if (prev) prev->next = cur->next;
            else running_head = cur->next;
            zval *cb = cur->callback;
            free(cur);
            return cb;
        }
        prev = cur;
        cur = cur->next;
    }
    return NULL;
}

/* closure registry */
static char *generate_token(void) {
    unsigned long t = next_task_id++;
    pid_t pid = getpid();
    time_t ti = time(NULL);
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "px_tok_%u_%lu_%lu", (unsigned) pid, (unsigned long) ti, t);
    return px_strdup(tmp);
}

static closure_entry *registry_find(const char *token) {
    closure_entry *c = closure_head;
    while (c) {
        if (strcmp(c->token, token) == 0) return c;
        c = c->next;
    }
    return NULL;
}

static char *registry_insert(const char *source, const char *bound_b64) {
    char *token = generate_token();
    closure_entry *e = (closure_entry *) malloc(sizeof(closure_entry));
    e->token = token;
    e->source = px_strdup(source ? source : "");
    e->bound_b64 = px_strdup(bound_b64 ? bound_b64 : "");
    e->next = closure_head;
    closure_head = e;
    return token;
}

static int create_worker_script_if_missing(const char *user_script) {
    if (user_script && access(user_script, R_OK) == 0) {
        strncpy(worker_script_path, user_script, sizeof(worker_script_path) - 1);
        worker_script_path[sizeof(worker_script_path) - 1] = '\0';
        return 0;
    }

    char template[] = WORKER_TEMPLATE;
    int fd = mkstemp(template);
    if (fd < 0) return -1;
    const char *script =
            "<?php\n"
            "$autoload = getenv('" ENV_AUTLOAD "');\n"
            "if ($autoload !== false && file_exists($autoload)) { @require_once $autoload; }\n"
            "while (!feof(STDIN)) {\n"
            "  $len_bytes = fread(STDIN, 4);\n"
            "  if ($len_bytes === false || strlen($len_bytes) < 4) break;\n"
            "  $arr = unpack('Nlen', $len_bytes);\n"
            "  $len = $arr['len'];\n"
            "  $data = ''; $remain = $len;\n"
            "  while ($remain > 0 && !feof(STDIN)) {\n"
            "    $chunk = fread(STDIN, $remain);\n"
            "    if ($chunk === false) break;\n"
            "    $data .= $chunk; $remain = $len - strlen($data);\n"
            "  }\n"
            "  if ($data === '') continue;\n"
            "  $desc = json_decode($data, true);\n"
            "  if (!is_array($desc)) { $out = ['task_id'=>($desc['task_id'] ?? 0),'success'=>false,'data'=>'invalid descriptor']; }\n"
            "  else {\n"
            "    $tid = $desc['task_id'] ?? 0;\n"
            "    try {\n"
            "      if (($desc['type'] ?? '') === 'closure_exec') {\n"
            "        $src = $desc['source'] ?? '';\n"
            "        $bound_b64 = $desc['bound_b64'] ?? '';\n"
            "        $args = $desc['args'] ?? [];\n"
            "        if ($bound_b64 !== '') { $b = @unserialize(base64_decode($bound_b64)); if (is_array($b)) extract($b); }\n"
            "        ob_start();\n"
            "        $closure = eval('return ' . $src . ';');\n"
            "        if (!is_callable($closure)) { $out = ['task_id'=>$tid,'success'=>false,'data'=>'eval did not return callable']; }\n"
            "        else { $ret = call_user_func_array($closure, $args); $outbuf = ob_get_clean(); $payload = ['return'=>$ret,'output'=>$outbuf]; $out = ['task_id'=>$tid,'success'=>true,'data'=>base64_encode(serialize($payload))]; }\n"
            "      } else { $out = ['task_id'=>($desc['task_id'] ?? 0),'success'=>false,'data'=>'unknown type']; }\n"
            "    } catch (Throwable $e) { $out = ['task_id'=>$tid,'success'=>false,'data'=>'exception: ' . $e->getMessage()]; }\n"
            "  }\n"
            "  $json = json_encode($out);\n"
            "  $len2 = strlen($json);\n"
            "  echo pack('N', $len2) . $json; fflush(STDOUT);\n"
            "}\n"
            "exit(0);\n";
    ssize_t wrote = write_all(fd, script, strlen(script));
    close(fd);
    if (wrote != (ssize_t) strlen(script)) {
        unlink(template);
        return -1;
    }
    strncpy(worker_script_path, template, sizeof(worker_script_path) - 1);
    worker_script_path[sizeof(worker_script_path) - 1] = '\0';
    return 0;
}

/* spawn workers */
static int spawn_workers(int count) {
    if (count <= 0 || count > PARALLELX_MAX_WORKERS) return -1;
    workers = (px_worker *) calloc(count, sizeof(px_worker));
    if (!workers) return -1;
    for (int i = 0; i < count; ++i) {
        int p2c[2], c2p[2];
        if (pipe(p2c) < 0) goto spawn_err;
        if (pipe(c2p) < 0) {
            close(p2c[0]);
            close(p2c[1]);
            goto spawn_err;
        }
        pid_t pid = fork();
        if (pid < 0) {
            close(p2c[0]);
            close(p2c[1]);
            close(c2p[0]);
            close(c2p[1]);
            goto spawn_err;
        } else if (pid == 0) {
            dup2(p2c[0], STDIN_FILENO);
            dup2(c2p[1], STDOUT_FILENO);
            close(p2c[0]);
            close(p2c[1]);
            close(c2p[0]);
            close(c2p[1]);
            execl(php_cli_path, php_cli_path, worker_script_path, (char *) NULL);
            _exit(127);
        } else {
            /* parent */
            close(p2c[0]);
            close(c2p[1]);
            workers[i].pid = pid;
            workers[i].to_child = p2c[1];
            workers[i].from_child = c2p[0];
            workers[i].recv_buf = NULL;
            workers[i].recv_cap = 0;
            workers[i].recv_used = 0;
            workers[i].busy = 0;
            workers[i].current_task_id = 0;
            set_nonblocking(workers[i].from_child);
        }
    }
    px_worker_count = count;
    return 0;
spawn_err:
    for (int k = 0; k < i; ++k) {
        if (workers[k].to_child) close(workers[k].to_child);
        if (workers[k].from_child) close(workers[k].from_child);
        if (workers[k].pid > 0) kill(workers[k].pid, SIGKILL);
    }
    free(workers);
    workers = NULL;
    px_worker_count = 0;
    return -1;
}

static px_worker *find_idle_worker() {
    for (int i = 0; i < px_worker_count; ++i) if (!workers[i].busy) return &workers[i];
    return NULL;
}

static int send_to_worker(px_worker *w, const char *json, size_t len, unsigned long tid) {
    uint32_t be = htonl((uint32_t) len);
    if (write_all(w->to_child, &be, 4) != 4) return -1;
    if (write_all(w->to_child, json, len) != (ssize_t) len) return -1;
    w->busy = 1;
    w->current_task_id = tid;
    return 0;
}

static int assign_pending(px_worker *w) {
    pending_node *p = pending_pop();
    if (!p) return 0;
    int rc = send_to_worker(w, p->payload, strlen(p->payload), p->task_id);
    if (rc != 0) {
        pending_push(p);
        return -1;
    }
    free(p->payload);
    free(p);
    return 0;
}

static void read_from_worker(px_worker *w) {
    char tmp[4096];
    ssize_t n;
    while ((n = read(w->from_child, tmp, sizeof(tmp))) > 0) {
        if (w->recv_used + n + 1 > w->recv_cap) {
            size_t nc = (w->recv_cap == 0) ? 8192 : w->recv_cap * 2;
            while (nc < w->recv_used + n + 1) nc *= 2;
            char *nb = (char *) realloc(w->recv_buf, nc);
            if (!nb) return;
            w->recv_buf = nb;
            w->recv_cap = nc;
        }
        memcpy(w->recv_buf + w->recv_used, tmp, n);
        w->recv_used += n;
        w->recv_buf[w->recv_used] = '\0';
    }
}

static char *try_extract(px_worker *w) {
    if (w->recv_used < 4) return NULL;
    uint32_t be = 0;
    memcpy(&be, w->recv_buf, 4);
    uint32_t len = ntohl(be);
    if (w->recv_used < 4 + len) return NULL;
    char *payload = (char *) malloc(len + 1);
    memcpy(payload, w->recv_buf + 4, len);
    payload[len] = '\0';
    size_t rem = w->recv_used - (4 + len);
    if (rem) memmove(w->recv_buf, w->recv_buf + 4 + len, rem);
    w->recv_used = rem;
    return payload;
}

static void invoke_callback(zval *cb, zval *assoc) {
    if (!cb || Z_TYPE_P(cb) == IS_UNDEF) return;
    zval retval;
    ZVAL_UNDEF(&retval);
    zval param;
    ZVAL_COPY(&param, assoc);
    if (call_user_function(EG(function_table), NULL, cb, &retval, 1, &param) != SUCCESS) {
        php_error_docref(NULL, E_WARNING, "parallelx: callback invocation failed");
    }
    zval_ptr_dtor(&param);
    if (!Z_ISUNDEF(retval)) zval_ptr_dtor(&retval);
}

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

    if (create_worker_script_if_missing(user_script ? user_script : NULL) != 0) {
        php_error_docref(NULL, E_WARNING, "parallelx: failed to create/find worker script");
        RETURN_FALSE;
    }

    if (spawn_workers((int) workers_z) != 0) {
        php_error_docref(NULL, E_WARNING, "parallelx: spawn_workers failed");
        unlink(worker_script_path);
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
    char *token = registry_insert(source, bound_b64 ? bound_b64 : "");
    RETVAL_STRING(token);
    RETURN_EMPTY_STRING();
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

    add_assoc_long(desc, "task_id", (zend_long) tid);

    zval zfn;
    ZVAL_STRING(&zfn, "json_encode");
    zval params[1];
    ZVAL_COPY(&params[0], desc);
    zval json_ret;
    ZVAL_UNDEF(&json_ret);

    if (call_user_function(EG(function_table), NULL, &zfn, &json_ret, 1, params) != SUCCESS) {
        zval_ptr_dtor(&params[0]);
        zval_dtor(&zfn);
        zend_hash_str_del(Z_ARRVAL_P(desc), "task_id", sizeof("task_id") - 1);
        php_error_docref(NULL, E_WARNING, "parallelx_submit_desc: json_encode failed");
        RETURN_FALSE;
    }

    zval_ptr_dtor(&params[0]);
    zval_ptr_dtor(&zfn);

    if (Z_TYPE(json_ret) != IS_STRING) {
        zval_ptr_dtor(&json_ret);
        zend_hash_str_del(Z_ARRVAL_P(desc), "task_id", sizeof("task_id") - 1);
        php_error_docref(NULL, E_WARNING, "parallelx_submit_desc: json_encode did not return string");
        RETURN_FALSE;
    }

    char *json_payload = (char *) malloc(Z_STRLEN(json_ret) + 1);
    memcpy(json_payload, Z_STRVAL(json_ret), Z_STRLEN(json_ret));
    json_payload[Z_STRLEN(json_ret)] = '\0';
    zend_hash_str_del(Z_ARRVAL_P(desc), "task_id", sizeof("task_id") - 1);

    pending_node *node = (pending_node *) malloc(sizeof(pending_node));
    node->task_id = tid;
    node->payload = json_payload;
    node->next = NULL;
    zval *cb_copy = (zval *) emalloc(sizeof(zval));
    ZVAL_COPY(cb_copy, callback);
    node->callback = cb_copy;
    running_add(tid, cb_copy);

    px_worker *w = find_idle_worker();
    if (w) {
        if (send_to_worker(w, node->payload, strlen(node->payload), node->task_id) != 0) {
            //フォールバック
            pending_push(node);
            RETURN_TRUE;
        } else {
            free(node->payload);
            free(node);
            RETURN_TRUE;
        }
    } else {
        pending_push(node);
        RETURN_TRUE;
    }
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
    closure_entry *e = registry_find(token);
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
    add_assoc_long(&desc, "task_id", (zend_long) tid);

    zval zfn;
    ZVAL_STRING(&zfn, "json_encode");
    zval p[1];
    ZVAL_COPY(&p[0], &desc);
    zval json_ret;
    ZVAL_UNDEF(&json_ret);

    if (call_user_function(EG(function_table), NULL, &zfn, &json_ret, 1, p) != SUCCESS) {
        zval_ptr_dtor(&p[0]);
        zval_dtor(&zfn);
        zval_ptr_dtor(&desc);
        zend_hash_str_del(Z_ARRVAL_P(&desc), "task_id", sizeof("task_id") - 1);
        php_error_docref(NULL, E_WARNING, "parallelx_submit_token: json_encode failed");
        RETURN_FALSE;
    }

    zval_ptr_dtor(&p[0]);
    zval_ptr_dtor(&zfn);

    if (Z_TYPE(json_ret) != IS_STRING) {
        zval_ptr_dtor(&json_ret);
        zval_ptr_dtor(&desc);
        zend_hash_str_del(Z_ARRVAL_P(&desc), "task_id", sizeof("task_id") - 1);
        php_error_docref(NULL, E_WARNING, "parallelx_submit_token: json_encode did not return string");
        RETURN_FALSE;
    }

    char *json_payload = (char *) malloc(Z_STRLEN(json_ret) + 1);
    memcpy(json_payload, Z_STRVAL(json_ret), Z_STRLEN(json_ret));
    json_payload[Z_STRLEN(json_ret)] = '\0';
    zend_hash_str_del(Z_ARRVAL_P(&desc), "task_id", sizeof("task_id") - 1);
    zval_ptr_dtor(&desc);

    pending_node *node = (pending_node *) malloc(sizeof(pending_node));
    node->task_id = tid;
    node->payload = json_payload;
    node->next = NULL;
    zval *cb_copy = (zval *) emalloc(sizeof(zval));
    ZVAL_COPY(cb_copy, callback);
    node->callback = cb_copy;

    running_add(tid, cb_copy);

    px_worker *w = find_idle_worker();
    if (w) {
        if (send_to_worker(w, node->payload, strlen(node->payload), node->task_id) != 0) {
            pending_push(node);
            RETURN_TRUE;
        } else {
            free(node->payload);
            free(node);
            RETURN_TRUE;
        }
    } else {
        pending_push(node);
        RETURN_TRUE;
    }
}

/* parallelx_poll() - PMMPのメインスレッド側でポール(1tickごとの呼出しが理想) */
PHP_FUNCTION(parallelx_poll) {
    if (zend_parse_parameters_none() == FAILURE) RETURN_FALSE;
    if (!px_initialized) RETURN_FALSE;

    for (int i = 0; i < px_worker_count; ++i) {
        px_worker *w = &workers[i];
        read_from_worker(w);
        while (1) {
            char *payload = try_extract(w);
            if (!payload) break;

            zval zfn;
            ZVAL_STRING(&zfn, "json_decode");
            zval params[2];
            ZVAL_STRING(&params[0], payload);
            ZVAL_TRUE(&params[1]);

            zval result;
            ZVAL_UNDEF(&result);
            if (call_user_function(EG(function_table), NULL, &zfn, &result, 2, params) != SUCCESS) {
                php_error_docref(NULL, E_WARNING, "parallelx: json_decode failed");
                zval_ptr_dtor(&params[0]);
                zval_ptr_dtor(&params[1]);
                zval_dtor(&zfn);
                free(payload);
                break;
            }

            zval_ptr_dtor(&params[0]);
            zval_ptr_dtor(&params[1]);
            zval_dtor(&zfn);

            if (Z_TYPE(result) == IS_ARRAY) {
                zval *ztid = zend_hash_str_find(Z_ARRVAL(result), "task_id", sizeof("task_id") - 1);
                unsigned long tid = 0;
                if (ztid) {
                    if (Z_TYPE_P(ztid) == IS_LONG) tid = (unsigned long) Z_LVAL_P(ztid);
                    else if (Z_TYPE_P(ztid) == IS_STRING) tid = strtoul(Z_STRVAL_P(ztid), NULL, 10);
                }
                zval *cb = running_pop(tid);
                if (cb) {
                    invoke_callback(cb, &result);
                    zval_ptr_dtor(cb);
                    efree(cb);
                } else {
                    php_error_docref(NULL, E_NOTICE, "parallelx: callback not found for task_id %lu", tid);
                }
                if (w->current_task_id == tid) {
                    w->busy = 0;
                    w->current_task_id = 0;
                    assign_pending(w);
                }
            } else {
                php_error_docref(NULL, E_WARNING, "parallelx: worker returned non-array JSON");
            }

            zval_ptr_dtor(&result);
            free(payload);
        }
    }

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

    if (worker_script_path[0]) {
        unlink(worker_script_path);
        worker_script_path[0] = '\0';
    }
    pending_node *pn = pending_head;
    while (pn) {
        pending_node *nx = pn->next;
        if (pn->payload) free(pn->payload);
        if (pn->callback) {
            zval_ptr_dtor(pn->callback);
            efree(pn->callback);
        }
        free(pn);
        pn = nx;
    }
    pending_head = pending_tail = NULL;
    running_node *rn = running_head;
    while (rn) {
        running_node *nx = rn->next;
        if (rn->callback) {
            zval_ptr_dtor(rn->callback);
            efree(rn->callback);
        }
        free(rn);
        rn = nx;
    }
    running_head = NULL;
    closure_entry *ce = closure_head;
    while (ce) {
        closure_entry *nx = ce->next;
        if (ce->token) free(ce->token);
        if (ce->source) free(ce->source);
        if (ce->bound_b64) free(ce->bound_b64);
        free(ce);
        ce = nx;
    }
    closure_head = NULL;

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

const zend_function_entry parallelx_functions[] = {
    PHP_FE(parallelx_init, NULL)
    PHP_FE(parallelx_register, NULL)
    PHP_FE(parallelx_submit_token, NULL)
    PHP_FE(parallelx_submit_desc, NULL)
    PHP_FE(parallelx_poll, NULL)
    PHP_FE(parallelx_shutdown, NULL)
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
