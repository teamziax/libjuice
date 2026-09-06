/* SPDX-License-Identifier: MPL-2.0 */
#include "mux_test_helpers.h"

static atomic_uint count;
static atomic_uint_fast64_t last_id;
static void incoming(const juice_mux_pending_request_t *request, void *ptr) {
	(void)ptr;
	atomic_store(&last_id, request->request_id);
	atomic_fetch_add(&count, 1);
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
	test_send(fd, port, "host", "peer", test_password);
	test_send(fd, port, "host", "peer2", test_password);
	test_send(fd, port, "host", "peer3", test_password);
	stats = test_received(port, 3);
	assert(stats.pending == 2 && stats.rejected == 1 && stats.agents == 0);
	for (int i = 0; i < 200 && atomic_load(&count) < 2; ++i) usleep(1000);
	assert(atomic_load(&count) == 2);
	uint64_t expired = atomic_load(&last_id);
	usleep(150000);
	assert(juice_mux_get_stats("127.0.0.1", port, &stats) == 0);
	assert(stats.pending == 0 && stats.rejected == 3);
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
	puts("mux authentication: verify before mapping, bounded attempts, expiry, distinct endpoints, stale IDs PASS");
}
