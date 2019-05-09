#pragma once
#include "jstr/jstr.h"
#include <stdbool.h>
#include <unistd.h>
#include <limits.h>

void log_error(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

extern int response_fd;

void fail(const char *status, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 2, 3)));

extern const char kStatusExited[];         // = "exited"
extern const char kStatusKilled[];         // = "killed"
extern const char kStatusMemoryLimit[];    // = "memoryLimit"
extern const char kStatusPidsLimit[];      // = "pidsLimit"
extern const char kStatusTimeLimit[];      // = "timeLimit"
extern const char kStatusFileLimit[];      // = "fileLimit"
extern const char kStatusInternalError[];  // = "internalError"
extern const char kStatusRequestInvalid[]; // = "requestInvalid"
extern const char kStatusResponseTooBig[]; // = "responseTooBig"

struct sandals_request {
    const char *host_name;
    const char *domain_name;
    const char *user;
    const char *group;
    const char *chroot;
    const jstr_token_t *mounts;
    const char *cgroup_root;
    const jstr_token_t *cgroup_config;
    const char *seccomp_policy;
    int va_randomize; // address space randomisation
    const char **cmd;
    const char **env;
    const char *work_dir;
    struct timespec time_limit;
    const char *stdstreams_file;
    long stdstreams_limit;
    const jstr_token_t *pipes;
};

extern const char kMountsKey[];       // = "mounts"
extern const char kCgroupConfigKey[]; // = "cgroupConfig"
extern const char kPipesKey[];        // = "pipes"

void *match_key(const char *str, ...) __attribute__((sentinel));

void request_recv(struct sandals_request *request);

struct sandals_response {
    size_t size;
    char buf[PIPE_BUF];
    char overflow[8];
};

void response_append_raw(struct sandals_response *response, const char *str);
void response_append_esc(struct sandals_response *response, const char *str);
void response_append_int(struct sandals_response *response, int value);
void response_send(const struct sandals_response *response);

int open_checked(const char *path, int flags, mode_t mode);
void write_checked(int fd, const void *buf, size_t size, const char *path);
void close_stray_fds_except(int fd);

void configure_net(const struct sandals_request *request);

void do_mounts(const struct sandals_request *request);

struct map_user_and_group_ctx {
    int procselfuidmap_fd;
    int procselfgidmap_fd;
};

void map_user_and_group_begin(struct map_user_and_group_ctx *ctx);

void map_user_and_group_complete(
    const struct sandals_request *request,
    struct map_user_and_group_ctx *ctx);

struct sandals_pipe {
    const char *file;
    const char *fifo;
    bool stdout;
    bool stderr;
    long limit;
};

int pipe_count(const struct sandals_request *request);

void pipe_foreach(
    const struct sandals_request *request, void(*fn)(), void *userdata);

struct cgroup_ctx {
    int cgroupprocs_fd; // "cgroup.procs"
    int memoryevents_fd; // "memory.events"
    int pidsevents_fd; // "pids.events"
};

void create_cgroup(
    const struct sandals_request *request, struct cgroup_ctx *ctx);

extern pid_t spawner_pid;

int supervisor(
    const struct sandals_request *request,
    const struct cgroup_ctx *cgroup_ctx,
    int spawnerout_fd);

int spawner(const struct sandals_request *request);
