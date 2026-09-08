/* SPDX-License-Identifier: MPL-2.0 */
#include "mux_test_helpers.h"

static atomic_uint count;
static atomic_uint_fast64_t last_id;
static void incoming(const juice_mux_pending_request_t *request, void *ptr) {
	(void)ptr;
	atomic_store(&last_id, request->request_id);
	atomic_fetch_add(&count, 1);
}

static uint64_t backlog_ids[64];
static atomic_uint backlog_count;
static void backlog(const juice_mux_pending_request_t *request, void *ptr) {
	(void)ptr;
	unsigned int index = atomic_load(&backlog_count);
	assert(index < 64);
	backlog_ids[index] = request->request_id;
	atomic_store(&backlog_count, index + 1);
}
static void reordered_cancellation(void) {
	uint16_t port = test_port(), source;
	int fd = test_socket(&source);
	juice_mux_pending_config_t config = {32, 5000};
	assert(juice_mux_listen_pending("127.0.0.1", port, &config, backlog, NULL) == 0);
	for (unsigned int i = 0; i < 64; ++i) {
		if (i == 32) {
			// Free the queue in reverse order, including non-head expiry/notification entries.
			for (int j = 31; j >= 0; --j)
				assert(juice_mux_reject_request("127.0.0.1", port, backlog_ids[j]) == 0);
		}
		char remote[24];
		snprintf(remote, sizeof(remote), "peer%u", i);
		test_send(fd, port, "host", remote, test_password);
		for (int j = 0; j < 400 && atomic_load(&backlog_count) <= i; ++j) usleep(1000);
		assert(atomic_load(&backlog_count) == i + 1);
	}
	for (int i = 63; i >= 32; --i) {
		assert(juice_mux_verify_request("127.0.0.1", port, backlog_ids[i], test_password) == 0);
		assert(juice_mux_reject_request("127.0.0.1", port, backlog_ids[i]) == 0);
	}
	juice_mux_stats_t stats;
	assert(juice_mux_get_stats("127.0.0.1", port, &stats) == 0 && stats.pending == 0);
	assert(juice_mux_listen_pending("127.0.0.1", port, NULL, NULL, NULL) == 0);
	close(fd);
}

int main(void) {
	juice_set_log_level(JUICE_LOG_LEVEL_FATAL);
	uint16_t port = test_port(), source;
	int fd = test_socket(&source);
	// An existing legacy agent must authenticate before mapping a new source.
	juice_agent_t *agent = test_agent(port);
	test_send(fd, port, "host", "peer", "incorrect-password-value");
	juice_mux_stats_t stats = test_received(port, 1);
	assert(stats.agents == 1 && stats.mapped_tuples == 0);
	test_send(fd, port, "host", "peer", test_password);
	test_response(fd);
	stats = test_received(port, 2);
	assert(stats.mapped_tuples == 1);
	juice_destroy(agent);
	close(fd);

	port = test_port();
	fd = test_socket(&source);
	juice_mux_pending_config_t config = {2, 100};
	assert(juice_mux_listen_pending("127.0.0.1", port, &config, incoming, NULL) == 0);
	assert(juice_mux_listen_pending("127.0.0.1", port, &config, incoming, NULL) != 0);
	assert(juice_mux_listen_pending("127.0.0.2", port, &config, incoming, NULL) == 0);
	assert(juice_mux_listen_pending("127.0.0.2", port, NULL, NULL, NULL) == 0);
	// Reject invalid ICE characters, including non-UTF8 bytes, before a binding
	// can copy metadata into a runtime string such as a JNI modified-UTF8 string.
	test_send(fd, port, "ho-st", "peer", test_password);
	test_send(fd, port, "host", "pe\xff" "r", test_password);
	stats = test_received(port, 2);
	assert(stats.pending == 0 && stats.notifications == 0 && stats.rejected == 2);
	test_send(fd, port, "host", "peer", test_password);
	test_send(fd, port, "host", "peer2", test_password);
	test_send(fd, port, "host", "peer3", test_password);
	stats = test_received(port, 5);
	assert(stats.pending == 2 && stats.rejected == 3 && stats.agents == 0);
	for (int i = 0; i < 200 && atomic_load(&count) < 2; ++i) usleep(1000);
	assert(atomic_load(&count) == 2);
	uint64_t expired = atomic_load(&last_id);
	usleep(150000);
	assert(juice_mux_get_stats("127.0.0.1", port, &stats) == 0);
	assert(stats.pending == 0 && stats.rejected == 5);
	assert(juice_mux_verify_request("127.0.0.1", port, expired, test_password) == JUICE_ERR_NOT_AVAIL);
	assert(juice_mux_listen_pending("127.0.0.1", port, NULL, NULL, NULL) == 0);
	assert(juice_mux_listen_pending("127.0.0.1", port, NULL, incoming, NULL) == 0);
	test_send(fd, port, "host", "peer", test_password);
	for (int i = 0; i < 200 && atomic_load(&count) < 3; ++i) usleep(1000);
	assert(atomic_load(&count) == 3 && atomic_load(&last_id) != expired);
	assert(juice_mux_verify_request("127.0.0.1", port, expired, test_password) == JUICE_ERR_NOT_AVAIL);
	assert(juice_mux_reject_request("127.0.0.1", port, atomic_load(&last_id)) == 0);
	assert(juice_mux_listen_pending("127.0.0.1", port, NULL, NULL, NULL) == 0);
	close(fd);
	reordered_cancellation();
	puts("mux authentication: verify before mapping, bounded attempts, expiry, distinct endpoints, stale IDs PASS");
}
