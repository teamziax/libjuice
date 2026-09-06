/* SPDX-License-Identifier: MPL-2.0 */
#include "mux_test_helpers.h"

typedef struct observed {
	atomic_uint calls;
	atomic_uint_fast64_t id;
	uint16_t source_port;
} observed_t;

static void incoming(const juice_mux_pending_request_t *request, void *ptr) {
	observed_t *observed = ptr;
	assert(strcmp(request->binding.address, "127.0.0.1") == 0);
	assert(strcmp(request->binding.local_ufrag, "host") == 0);
	assert(strcmp(request->binding.remote_ufrag, "peer") == 0);
	assert(request->binding.port == observed->source_port);
	atomic_store(&observed->id, request->request_id);
	atomic_fetch_add(&observed->calls, 1);
}

static uint64_t await_callback(observed_t *observed, unsigned int count) {
	for (int i = 0; i < 400 && atomic_load(&observed->calls) < count; ++i) usleep(5000);
	assert(atomic_load(&observed->calls) == count);
	return atomic_load(&observed->id);
}

static void first_request(bool duplicates) {
	uint16_t port = test_port(), source;
	int fd = test_socket(&source);
	observed_t observed = {.source_port = source};
	assert(juice_mux_listen_pending("127.0.0.1", port, NULL, incoming, &observed) == 0);
	test_send(fd, port, "host", "peer", test_password);
	uint64_t id = await_callback(&observed, 1);
	// The normal case sends exactly once until after the first response.
	usleep(30000);
	juice_mux_stats_t stats = test_received(port, 1);
	assert(stats.agents == 0 && stats.mapped_tuples == 0 && stats.pending == 1);
	assert(juice_mux_verify_request("127.0.0.1", port, id, test_password) == 0);
	assert(juice_mux_verify_request("127.0.0.1", port, id, test_password) == JUICE_ERR_NOT_AVAIL);
	juice_agent_t *agent = test_agent(port);
	if (duplicates) {
		for (int i = 0; i < 20; ++i) test_send(fd, port, "host", "peer", test_password);
		stats = test_received(port, 21);
		assert(stats.pending == 1 && stats.duplicates == 20 && stats.notifications == 1);
		assert(stats.mapped_tuples == 0); // A matching configured agent cannot bypass acceptance.
		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		assert(poll(&pfd, 1, 20) == 0);
	}
	assert(juice_mux_attach_request("127.0.0.1", port, id, agent) == 0);
	test_response(fd);
	assert(juice_mux_attach_request("127.0.0.1", port, id, agent) == JUICE_ERR_NOT_AVAIL);
	assert(juice_mux_get_stats("127.0.0.1", port, &stats) == 0);
	assert(stats.pending == 0 && stats.agents == 1 && stats.mapped_tuples == 1);
	uint64_t received = stats.received;
	for (int i = 0; i < 100; ++i) test_send(fd, port, "host", "peer", test_password);
	stats = test_received(port, received + 100);
	assert(stats.notifications == 1 && atomic_load(&observed.calls) == 1);
	// Stopping admission leaves established connections operational.
	assert(juice_mux_listen_pending("127.0.0.1", port, NULL, NULL, NULL) == 0);
	int other = test_socket(&source);
	test_send(other, port, "host", "peer", test_password);
	stats = test_received(port, received + 101);
	assert(stats.mapped_tuples == 1 && stats.notifications == 1);
	close(other);
	juice_destroy(agent);
	assert(juice_mux_get_stats("127.0.0.1", port, &stats) == JUICE_ERR_NOT_AVAIL);
	close(fd);
}

static void invalid_then_valid(void) {
	uint16_t port = test_port(), source;
	int fd = test_socket(&source);
	observed_t observed = {.source_port = source};
	assert(juice_mux_listen_pending("127.0.0.1", port, NULL, incoming, &observed) == 0);
	test_send(fd, port, "host", "peer", "incorrect-password-value");
	uint64_t bad = await_callback(&observed, 1);
	assert(juice_mux_verify_request("127.0.0.1", port, bad, test_password) == JUICE_ERR_FAILED);
	juice_mux_stats_t stats = test_received(port, 1);
	assert(stats.agents == 0 && stats.mapped_tuples == 0 && stats.pending == 0);
	test_send(fd, port, "host", "peer", test_password);
	uint64_t good = await_callback(&observed, 2);
	assert(good != bad);
	assert(juice_mux_verify_request("127.0.0.1", port, good, test_password) == 0);
	assert(juice_mux_reject_request("127.0.0.1", port, good) == 0);
	assert(juice_mux_reject_request("127.0.0.1", port, good) == JUICE_ERR_NOT_AVAIL);
	assert(juice_mux_listen_pending("127.0.0.1", port, NULL, NULL, NULL) == 0);
	close(fd);
}

int main(void) {
	juice_set_log_level(JUICE_LOG_LEVEL_FATAL);
	first_request(false);
	first_request(true);
	invalid_then_valid();
	puts("pending ICE: first-request response without retransmission, one decision, native traffic, integrity PASS");
}
