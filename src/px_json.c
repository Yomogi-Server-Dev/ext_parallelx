#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "px_internal.h"

zend_result px_encode_descriptor_with_task(zval *desc, unsigned long tid, char **out, size_t *out_len) {
    add_assoc_long(desc, "task_id", (zend_long) tid);

    smart_str buf = {0};
    if (php_json_encode(&buf, desc, PHP_JSON_PARTIAL_OUTPUT_ON_ERROR) != SUCCESS) {
        smart_str_free(&buf);
        zend_hash_str_del(Z_ARRVAL_P(desc), "task_id", sizeof("task_id") - 1);
        return FAILURE;
    }

    smart_str_0(&buf);
    if (!buf.s) {
        zend_hash_str_del(Z_ARRVAL_P(desc), "task_id", sizeof("task_id") - 1);
        return FAILURE;
    }

    *out_len = ZSTR_LEN(buf.s);
    if (*out_len > PARALLELX_MAX_MESSAGE) {
        smart_str_free(&buf);
        zend_hash_str_del(Z_ARRVAL_P(desc), "task_id", sizeof("task_id") - 1);
        return FAILURE;
    }

    *out = (char *) emalloc(*out_len + 1);
    memcpy(*out, ZSTR_VAL(buf.s), *out_len + 1);

    smart_str_free(&buf);
    zend_hash_str_del(Z_ARRVAL_P(desc), "task_id", sizeof("task_id") - 1);
    return SUCCESS;
}

zend_result px_decode_worker_json(const char *payload, size_t len, zval *out) {
    ZVAL_UNDEF(out);
    if (php_json_decode_ex(out, (char *) payload, len, PHP_JSON_OBJECT_AS_ARRAY, PHP_JSON_PARSER_DEFAULT_DEPTH) != SUCCESS) {
        return FAILURE;
    }
    return SUCCESS;
}

