/* src/parallelx.h */
#ifndef PARALLELX_H
#define PARALLELX_H

#include "php.h"

PHP_FUNCTION(parallelx_init); /* (int workers, string php_cli = null,
                                string worker_script = null, string autoload = null) */
PHP_FUNCTION(parallelx_register); /* (string source, string bound_b64) -> string token */
PHP_FUNCTION(parallelx_submit_token); /* (string token, array args, callable onComplete) */
PHP_FUNCTION(parallelx_submit_desc); /* (array descriptor, callable onComplete) */
PHP_FUNCTION(parallelx_poll); /* () -> bool */
PHP_FUNCTION(parallelx_shutdown); /* () -> bool */

#endif /* PARALLELX_H */
