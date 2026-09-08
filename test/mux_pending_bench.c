/* SPDX-License-Identifier: MPL-2.0 */
// Synthetic queue-maintenance benchmark; excludes packet reception and polling.
#include "conn_mux.c"
#include <stdio.h>
#include <time.h>

static void noop(const juice_mux_pending_request_t *r, void *p) { (void)r; (void)p; }
static double nanoseconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + t.tv_nsec;
}
int main(void) {
    juice_set_log_level(JUICE_LOG_LEVEL_NONE);
    for (unsigned n = 0; n <= 4096; n = n == 0 ? 256 : n * 4) {
        registry_impl_t impl = {0};
        conn_registry_t registry = {0};
        registry.impl = &impl;
        mutex_init(&registry.mutex, 0);
        mutex_init(&impl.callback_mutex, 0);
        juice_mux_pending_config_t config = {4096, 30000};
        if (conn_mux_listen_pending(&registry, &config, noop, NULL) != 0) {
            fprintf(stderr, "Unable to initialize pending-request benchmark\n");
            return 1;
        }
        addr_record_t src = {0};
        src.len = sizeof(struct sockaddr_in);
        ((struct sockaddr_in *)&src.addr)->sin_family = AF_INET;
        ((struct sockaddr_in *)&src.addr)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        for (unsigned i = 0; i < n; ++i) {
            char local[32];
            snprintf(local, sizeof(local), "local%08u", i);
            defer_request(&impl, &src, local, "remote", "a", 1);
        }
        while (impl.notifications < n) dispatch_pending(&registry);
        struct pollfd pfd = {0};
        timestamp_t next;
        const unsigned iterations = 20000;
        double start = nanoseconds();
        for (unsigned i = 0; i < iterations; ++i) {
            conn_mux_prepare(&registry, &pfd, &next);
            conn_mux_process(&registry, &pfd);
        }
        double elapsed = nanoseconds() - start;
        printf("pending=%u bytes_per_request=%zu native_request_bytes=%zu ns_per_empty_iteration=%.1f\n",
            n, sizeof(pending_request_t), n * sizeof(pending_request_t), elapsed / iterations);
        clear_pending(&impl);
        free(impl.pending_buckets);
#ifdef BENCH_INDEXED_PENDING
        free(impl.pending_ids);
#endif
        mutex_destroy(&impl.callback_mutex);
        mutex_destroy(&registry.mutex);
    }
}
