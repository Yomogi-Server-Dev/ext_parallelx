#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "px_internal.h"

#include <stdlib.h>

static void pending_push(pending_node *n) {
    n->next = NULL;
    if (!pending_tail) pending_head = pending_tail = n;
    else {
        pending_tail->next = n;
        pending_tail = n;
    }
}

static pending_node *pending_pop(void) {
    pending_node *n = pending_head;
    if (!n) return NULL;
    pending_head = n->next;
    if (!pending_head) pending_tail = NULL;
    n->next = NULL;
    return n;
}

static zend_result running_add(unsigned long id, zval *cb) {
    running_node *n = (running_node *) malloc(sizeof(running_node));
    if (!n) return FAILURE;
    n->task_id = id;
    n->callback = cb;
    n->next = running_head;
    running_head = n;
    return SUCCESS;
}

zval *px_running_pop(unsigned long id) {
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

void px_invoke_callback(zval *cb, zval *assoc) {
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

void px_fail_task(unsigned long tid, const char *message) {
    zval *cb = px_running_pop(tid);
    if (!cb) return;

    zval result;
    array_init(&result);
    add_assoc_long(&result, "task_id", (zend_long) tid);
    add_assoc_bool(&result, "success", 0);
    add_assoc_string(&result, "data", message ? message : "error");

    px_invoke_callback(cb, &result);

    zval_ptr_dtor(cb);
    efree(cb);
    zval_ptr_dtor(&result);
}

zend_result px_enqueue_payload(unsigned long tid, char *payload, size_t payload_len, zval *callback) {
    pending_node *node = (pending_node *) malloc(sizeof(pending_node));
    if (!node) return FAILURE;

    node->task_id = tid;
    node->payload = payload;
    node->payload_len = payload_len;
    node->next = NULL;

    zval *cb_copy = (zval *) emalloc(sizeof(zval));
    if (!cb_copy) {
        efree(payload);
        free(node);
        return FAILURE;
    }
    ZVAL_COPY(cb_copy, callback);
    node->callback = cb_copy;

    if (running_add(tid, cb_copy) != SUCCESS) {
        zval result;
        array_init(&result);
        add_assoc_long(&result, "task_id", (zend_long) tid);
        add_assoc_bool(&result, "success", 0);
        add_assoc_string(&result, "data", "out of memory");
        px_invoke_callback(cb_copy, &result);
        zval_ptr_dtor(&result);

        zval_ptr_dtor(cb_copy);
        efree(cb_copy);
        efree(payload);
        free(node);
        return FAILURE;
    }

    px_worker *w = px_find_idle_worker();
    if (w) {
        if (px_send_to_worker(w, node->payload, node->payload_len, node->task_id) != 0) {
            pending_push(node);
        } else {
            efree(node->payload);
            free(node);
            return SUCCESS;
        }
    } else {
        pending_push(node);
    }
    return SUCCESS;
}

int px_assign_pending(px_worker *w) {
    pending_node *p = pending_pop();
    if (!p) return 0;
    int rc = px_send_to_worker(w, p->payload, p->payload_len, p->task_id);
    if (rc != 0) {
        pending_push(p);
        return -1;
    }
    efree(p->payload);
    free(p);
    return 0;
}

void px_dispatch_pending_to_idle(void) {
    while (pending_head) {
        px_worker *w = px_find_idle_worker();
        if (!w) break;
        if (px_assign_pending(w) != 0) break;
    }
}

void px_queue_free_all(void) {
    pending_node *pn = pending_head;
    while (pn) {
        pending_node *nx = pn->next;
        if (pn->payload) efree(pn->payload);
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
}
