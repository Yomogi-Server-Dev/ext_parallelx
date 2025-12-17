#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "px_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

char *px_strdup(const char *s) {
    if (!s) return NULL;
    size_t l = strlen(s);
    char *p = (char *) malloc(l + 1);
    if (!p) return NULL;
    memcpy(p, s, l + 1);
    return p;
}

static char *generate_token(void) {
    unsigned long t = next_task_id++;
    pid_t pid = getpid();
    time_t ti = time(NULL);
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "px_tok_%u_%lu_%lu", (unsigned) pid, (unsigned long) ti, t);
    return px_strdup(tmp);
}

closure_entry *px_registry_find(const char *token) {
    closure_entry *c = closure_head;
    while (c) {
        if (strcmp(c->token, token) == 0) return c;
        c = c->next;
    }
    return NULL;
}

char *px_registry_insert(const char *source, const char *bound_b64) {
    char *token = generate_token();
    if (!token) return NULL;
    closure_entry *e = (closure_entry *) malloc(sizeof(closure_entry));
    if (!e) {
        free(token);
        return NULL;
    }
    e->token = token;
    e->source = px_strdup(source ? source : "");
    e->bound_b64 = px_strdup(bound_b64 ? bound_b64 : "");
    if (!e->source || !e->bound_b64) {
        if (e->source) free(e->source);
        if (e->bound_b64) free(e->bound_b64);
        free(e->token);
        free(e);
        return NULL;
    }
    e->next = closure_head;
    closure_head = e;
    return token;
}

void px_registry_free_all(void) {
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
}
