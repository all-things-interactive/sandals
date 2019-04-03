#define _GNU_SOURCE
#include "sandals.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <unistd.h>

struct sandals_sink {
    const char *file;
    const char *fifo;
    int fd;
    int splice;
    long limit;
};

static void sink_init(
    size_t index, const struct sandals_pipe *pipe,
    struct sandals_sink *sink) {

    sink[index].fifo = pipe->fifo;
    sink[index].splice = 1;
    sink[index].limit = pipe->limit;
    sink[index].fd = open_checked(
        sink[index].file = pipe->file,
        O_CLOEXEC|O_WRONLY|O_TRUNC|O_CREAT|O_NOCTTY, 0600);
    // Non-blocking mode screws data forwarder (do_pipes).
    // May happen if pipe->file is /proc/self/fd/*.
    if (fcntl(sink[index].fd, F_GETFL)&O_NONBLOCK)
        fail(kStatusInternalError,
            "File '%s': non-blocking mode not supported", pipe->file);
}

enum {
    HYPER_INDEX,
#if HAVE_SECCOMPUSERNOTIFY
    SECCOMPUSERNOTIFY_INDEX,
#endif
    MEMORYEVENTS_INDEX,
    STATUSFIFO_INDEX,
    TIMER_INDEX,
    SPAWNEROUT_INDEX,
    PIPE0_INDEX
};

struct sandals_supervisor {
    int exiting;
    const struct sandals_request *request;
    size_t npipe;
    struct pollfd *pollfd;
    int npollfd;
    struct sandals_sink *sink;
    void *cmsgbuf;
    struct sandals_response response, uresponse;
};

static void do_hyper(struct sandals_supervisor *s) {
    char buf[1];
    struct iovec iovec = { .iov_base = buf, .iov_len = sizeof buf };
    struct msghdr msghdr = {
        .msg_iov = &iovec,
        .msg_iovlen = 1,
        .msg_control = s->cmsgbuf,
        .msg_controllen = CMSG_SPACE(sizeof(int)*(2+s->npipe))
    };
    struct cmsghdr *cmsghdr;
    ssize_t rc = recvmsg(
        s->pollfd[HYPER_INDEX].fd, &msghdr, MSG_DONTWAIT|MSG_CMSG_CLOEXEC);
    if (rc==-1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        fail(kStatusInternalError, "recvmsg: %s", strerror(errno));
    }
    s->pollfd[HYPER_INDEX].fd = -1;
    if (rc>0
        && (cmsghdr = CMSG_FIRSTHDR(&msghdr))
        && cmsghdr->cmsg_level == SOL_SOCKET
        && cmsghdr->cmsg_type == SCM_RIGHTS
        && cmsghdr->cmsg_len == CMSG_LEN(sizeof(int)
            *(SECCOMPUSERNOTIFY+s->npipe+(s->request->status_fifo!=NULL)))
    ) {
        const int *fd = (const int *)CMSG_DATA(cmsghdr);
#if SECCOMPUSERNOTIFY
        s->pollfd[SECCOMPUSERNOTIFY_INDEX].fd = fd[0];
#endif
        for (size_t i = 0; i < s->npipe; ++i) {
            s->pollfd[PIPE0_INDEX+i].fd = fd[SECCOMPUSERNOTIFY+i];
            s->pollfd[PIPE0_INDEX+i].events = POLLIN;
        };
        if (s->request->status_fifo)
            s->pollfd[STATUSFIFO_INDEX].fd = fd[SECCOMPUSERNOTIFY+s->npipe];
        s->npollfd = PIPE0_INDEX+s->npipe;
    }
}

static int do_statusfifo(struct sandals_supervisor *s) {
    enum { TOKEN_COUNT = 64 };
    jstr_parser_t parser;
    jstr_token_t root[TOKEN_COUNT];
    const jstr_token_t *root_end, *tok;
    ssize_t rc = read(
        s->pollfd[STATUSFIFO_INDEX].fd,
        s->uresponse.buf+s->uresponse.size,
        (sizeof s->uresponse.buf)-s->uresponse.size+1);
    if (rc>0) {
        s->uresponse.size += rc;
        return 0;
    }
    if (rc==-1) {
        if (errno==EAGAIN || errno==EWOULDBLOCK) return 0;
        fail(kStatusInternalError,
            "Receiving response: %s", strerror(errno));
    }
    s->pollfd[STATUSFIFO_INDEX].fd = -1;
    if (!s->uresponse.size) goto bad_response;
    // copy first since validation mutates uresponse
    memcpy(
        s->response.buf, s->uresponse.buf,
        s->response.size = s->uresponse.size);
    jstr_init(&parser);
    s->uresponse.buf[s->uresponse.size] = 0;
    if (s->uresponse.size > sizeof s->uresponse.buf
        || jstr_parse(
            &parser, s->uresponse.buf,
            root, TOKEN_COUNT) != s->uresponse.size
        || jstr_type(root)!=JSTR_OBJECT
    ) goto bad_response;
    root_end = jstr_next(root); tok = root+1;
    while (tok!=root_end) {
        if (!strcmp(jstr_value(tok), "status")
            && (jstr_type(tok+1)!=JSTR_STRING
                || !strncmp(jstr_value(tok+1), "sys.", 4))
        ) goto bad_response;
        tok = jstr_next(tok+1);
    }
    return 1;
bad_response:
    // don't use fail(), might lose piped data
    s->response.size = 0;
    response_append_raw(&s->response, "{\"status\":\"");
    response_append_esc(&s->response, kStatusStatusInvalid);
    response_append_raw(&s->response, "\"}\n");
    return -1;
}

static int do_spawnerout(struct sandals_supervisor *s) {
    ssize_t rc = read(
        s->pollfd[SPAWNEROUT_INDEX].fd,
        s->response.buf+s->response.size,
        (sizeof s->response.buf)-s->response.size+1);
    if (!rc>0) {
        if (!s->response.size)
            fail(kStatusInternalError, "Empty response");
        return 1;
    }
    if (rc==-1) {
        if (errno==EINTR) return 0;
        fail(kStatusInternalError,
            "Receiving response: %s", strerror(errno));
    }
    s->response.size += rc;
    return 0;
}

static int do_pipes(struct sandals_supervisor *s) {
    int status = 0;
    size_t rc;
    struct sandals_sink *sink;
    for (int i = s->npollfd; --i >= PIPE0_INDEX; ) {
        if (s->pollfd[i].fd==-1 || !s->pollfd[i].revents && !s->exiting)
            continue;
        sink = s->sink+i-PIPE0_INDEX;
        if (sink->limit && sink->splice) {
            if ((rc = splice(
                s->pollfd[i].fd, NULL, sink->fd, NULL, sink->limit,
                SPLICE_F_NONBLOCK)) == -1
            ) {
                if (errno==EINVAL) { sink->splice = 0; ++i; continue; }
                if (errno==EAGAIN) continue;
                fail(kStatusInternalError,
                    "Writing '%s': %s", sink->file, strerror(errno));
            }
        } else {
            char buf[PIPE_BUF];
            char *p, *e;
            if ((rc = read(s->pollfd[i].fd, buf, sizeof buf)) == -1) {
                if (errno==EAGAIN || errno==EWOULDBLOCK) continue;
                fail(kStatusInternalError,
                    "Reading '%s': %s", sink->fifo, strerror(errno));
            }
            p = buf; e = buf+(rc > sink->limit ? sink->limit : rc);
            while (p != e) {
                ssize_t sizewr = write(sink->fd, p, e-p);
                if (sizewr==-1) {
                    if (errno==EINTR) continue;
                    fail(kStatusInternalError,
                        "Writing '%s': %s", sink->file, strerror(errno));
                }
                p += sizewr;
            }
        }
        if (rc && rc <= sink->limit) {
            sink->limit -= rc;
            i += s->exiting;
            // in 'exiting' mode process the same pipe until fully
            // drained or limit exceeded
        } else {
            close(s->pollfd[i].fd);
            s->pollfd[i].fd = -1;
            if (rc) {
                s->response.size = 0;
                response_append_raw(&s->response, "{\"status\":\"");
                response_append_esc(&s->response, kStatusPipeLimit);
                response_append_raw(&s->response, "\",\"fifo\":\"");
                response_append_esc(&s->response, sink->fifo);
                response_append_raw(&s->response, "\",\"file\":\"");
                response_append_esc(&s->response, sink->file);
                response_append_raw(&s->response, "\"}\n");
                status = -1;
            }
        }
    }
    return status;
}

int supervisor(
    const struct sandals_request *request,
    const struct cgroup_ctx *cgroup_ctx,
    int spawnerout_fd, int hyper_fd) {

    int timer_fd;
    struct itimerspec itimerspec = {};
    ssize_t rc;
    struct sandals_supervisor s = {
        .request = request, .npollfd = PIPE0_INDEX
    };

    if ((timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC)) == -1)
        fail(kStatusInternalError, "Create timer: %s", strerror(errno));
    itimerspec.it_value = request->time_limit;
    if (timerfd_settime(timer_fd, 0, &itimerspec, NULL) == -1)
        fail(kStatusInternalError, "Set timer: %s", strerror(errno));

    s.npipe = pipe_count(request);
    if (!(s.sink = malloc(sizeof(struct sandals_sink)*s.npipe
        +sizeof(struct pollfd)*(PIPE0_INDEX+s.npipe)
        +CMSG_SPACE(sizeof(int)*(2+s.npipe)))
    )) fail(kStatusInternalError, "malloc");
    s.pollfd = (struct pollfd *)(s.sink+s.npipe);
    s.cmsgbuf = s.pollfd+PIPE0_INDEX+s.npipe;

    pipe_foreach(request, sink_init, s.sink);

    s.pollfd[HYPER_INDEX].fd = hyper_fd;
    s.pollfd[HYPER_INDEX].events = POLLIN;
#if SECCOMPUSERNOTIFY
    s.pollfd[SECCOMPUSERNOTIFY_INDEX].fd = -1;
    s.pollfd[SECCOMPUSERNOTIFY_INDEX].events = POLLIN;
#endif
    s.pollfd[MEMORYEVENTS_INDEX].fd = cgroup_ctx->memoryevents_fd;
    s.pollfd[MEMORYEVENTS_INDEX].events = POLLPRI;
    s.pollfd[STATUSFIFO_INDEX].fd = -1;
    s.pollfd[STATUSFIFO_INDEX].events = POLLIN;
    s.pollfd[TIMER_INDEX].fd = timer_fd;
    s.pollfd[TIMER_INDEX].events = POLLIN;
    s.pollfd[SPAWNEROUT_INDEX].fd = spawnerout_fd;
    s.pollfd[SPAWNEROUT_INDEX].events = POLLIN;

    s.response.size = s.uresponse.size = 0;

    for (;;) {
        if (poll(s.pollfd, s.npollfd, -1) == -1 && errno != EINTR)
            fail(kStatusInternalError, "poll: %s", strerror(errno));

        if (s.pollfd[HYPER_INDEX].revents) {
            do_hyper(&s);
            continue; // new fds were installed, have to re-poll
        }

#if SECCOMPUSERNOTIFY
        if (s.pollfd[SECCOMPUSERNOTIFY_INDEX].revents) {
            // TODO seccomp violations
        }
#endif
        if (s.pollfd[MEMORYEVENTS_INDEX].revents) {
            // TODO memory events (OOM)
        }

        if (s.pollfd[STATUSFIFO_INDEX].revents && do_statusfifo(&s)) break;

        if (s.pollfd[TIMER_INDEX].revents) {
            s.response.size = 0;
            response_append_raw(&s.response, "{\"status\":\"");
            response_append_esc(&s.response, kStatusTimeLimit);
            response_append_raw(&s.response, "\"}\n");
            break;
        }

        if (s.pollfd[SPAWNEROUT_INDEX].revents && do_spawnerout(&s)) break;

        if (do_pipes(&s)) break;
    }

    kill(spawner_pid, SIGKILL); spawner_pid = -1;
    s.exiting = 1; do_pipes(&s);
    response_send(&s.response);
    return EXIT_SUCCESS;
}