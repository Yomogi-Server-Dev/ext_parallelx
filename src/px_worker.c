#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "px_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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

int px_create_worker_script_if_missing(const char *user_script) {
    if (user_script && access(user_script, R_OK) == 0) {
        strncpy(worker_script_path, user_script, PATH_MAX - 1);
        worker_script_path[PATH_MAX - 1] = '\0';
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
            "  if (!is_array($desc)) { $out = ['task_id'=>0,'success'=>false,'data'=>'invalid descriptor']; }\n"
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
    strncpy(worker_script_path, template, PATH_MAX - 1);
    worker_script_path[PATH_MAX - 1] = '\0';
    return 0;
}

int px_spawn_workers(int count) {
    if (count <= 0 || count > PARALLELX_MAX_WORKERS) return -1;
    workers = (px_worker *) calloc(count, sizeof(px_worker));
    if (!workers) return -1;
    int i;
    for (i = 0; i < count; ++i) {
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
            workers[i].dead = 0;
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

px_worker *px_find_idle_worker(void) {
    for (int i = 0; i < px_worker_count; ++i) if (!workers[i].busy && !workers[i].dead) return &workers[i];
    return NULL;
}

int px_send_to_worker(px_worker *w, const char *json, size_t len, unsigned long tid) {
    uint32_t be = htonl((uint32_t) len);
    if (write_all(w->to_child, &be, 4) != 4) {
        w->dead = 1;
        return -1;
    }
    if (write_all(w->to_child, json, len) != (ssize_t) len) {
        w->dead = 1;
        return -1;
    }
    w->busy = 1;
    w->current_task_id = tid;
    return 0;
}

void px_read_from_worker(px_worker *w) {
    char tmp[4096];
    ssize_t n;
    while ((n = read(w->from_child, tmp, sizeof(tmp))) > 0) {
        if (w->recv_used + (size_t) n + 1 > w->recv_cap) {
            size_t nc = (w->recv_cap == 0) ? 8192 : w->recv_cap * 2;
            while (nc < w->recv_used + (size_t) n + 1) nc *= 2;
            char *nb = (char *) realloc(w->recv_buf, nc);
            if (!nb) {
                w->dead = 1;
                return;
            }
            w->recv_buf = nb;
            w->recv_cap = nc;
        }
        memcpy(w->recv_buf + w->recv_used, tmp, (size_t) n);
        w->recv_used += (size_t) n;
        w->recv_buf[w->recv_used] = '\0';
    }
    if (n == 0) {
        w->dead = 1;
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        w->dead = 1;
    }
}

int px_try_extract(px_worker *w, char **payload_out, size_t *len_out) {
    if (w->recv_used < 4) return 0;
    uint32_t be = 0;
    memcpy(&be, w->recv_buf, 4);
    uint32_t len = ntohl(be);
    if (len > PARALLELX_MAX_MESSAGE) {
        return -1;
    }
    if (w->recv_used < 4 + (size_t) len) return 0;
    char *payload = (char *) malloc((size_t) len + 1);
    if (!payload) {
        w->dead = 1;
        return -1;
    }
    memcpy(payload, w->recv_buf + 4, (size_t) len);
    payload[len] = '\0';
    size_t rem = w->recv_used - (4 + (size_t) len);
    if (rem) memmove(w->recv_buf, w->recv_buf + 4 + (size_t) len, rem);
    w->recv_used = rem;
    *payload_out = payload;
    *len_out = (size_t) len;
    return 1;
}

int px_restart_worker(int idx) {
    if (!workers || idx < 0 || idx >= px_worker_count) return -1;
    px_worker *w = &workers[idx];

    if (w->busy && w->current_task_id) {
        px_fail_task(w->current_task_id, "worker restarted");
    }

    if (w->pid > 0) {
        kill(w->pid, SIGKILL);
        waitpid(w->pid, NULL, 0);
    }
    if (w->to_child > 0) close(w->to_child);
    if (w->from_child > 0) close(w->from_child);
    if (w->recv_buf) free(w->recv_buf);

    memset(w, 0, sizeof(*w));
    w->to_child = -1;
    w->from_child = -1;
    w->pid = -1;
    w->dead = 0;

    int p2c[2], c2p[2];
    if (pipe(p2c) < 0) return -1;
    if (pipe(c2p) < 0) {
        close(p2c[0]);
        close(p2c[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(p2c[0]);
        close(p2c[1]);
        close(c2p[0]);
        close(c2p[1]);
        return -1;
    } else if (pid == 0) {
        dup2(p2c[0], STDIN_FILENO);
        dup2(c2p[1], STDOUT_FILENO);
        close(p2c[0]);
        close(p2c[1]);
        close(c2p[0]);
        close(c2p[1]);
        execl(php_cli_path, php_cli_path, worker_script_path, (char *) NULL);
        _exit(127);
    }

    close(p2c[0]);
    close(c2p[1]);
    w->pid = pid;
    w->to_child = p2c[1];
    w->from_child = c2p[0];
    w->busy = 0;
    w->current_task_id = 0;
    w->recv_buf = NULL;
    w->recv_used = 0;
    w->recv_cap = 0;
    w->dead = 0;
    set_nonblocking(w->from_child);
    return 0;
}

