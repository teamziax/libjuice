/* SPDX-License-Identifier: MPL-2.0 */
#include "mux_test_helpers.h"
#include <pthread.h>
#include "agent.h"

typedef struct listener {
	atomic_bool entered, release, removing, removed;
	uint16_t port;
	uint64_t request_id;
	pthread_mutex_t id_mutex;
} listener_t;

static void suspended(const juice_mux_pending_request_t *request, void *ptr) {
	listener_t *listener = ptr;
	assert(juice_mux_listen_pending("127.0.0.1", listener->port, NULL, NULL, NULL) == JUICE_ERR_INVALID);
	pthread_mutex_lock(&listener->id_mutex);
	listener->request_id = request->request_id;
	pthread_mutex_unlock(&listener->id_mutex);
	atomic_store(&listener->entered, true);
	for (int i = 0; i < 400 && !atomic_load(&listener->release); ++i) usleep(5000);
	assert(atomic_load(&listener->release));
	// Stop has already cancelled this request. This needs the global entry lock:
	// a stop waiting with that lock held would deadlock this callback.
	assert(juice_mux_reject_request("127.0.0.1", listener->port, request->request_id) == JUICE_ERR_NOT_AVAIL);
	assert(strcmp(request->binding.local_ufrag, "host") == 0);
}

static void *remove_listener(void *ptr) {
	listener_t *listener = ptr;
	atomic_store(&listener->removing, true);
	assert(juice_mux_listen_pending("127.0.0.1", listener->port, NULL, NULL, NULL) == 0);
	atomic_store(&listener->removed, true);
	return NULL;
}

static void run_case(bool accepted) {
	listener_t listener = {.port = test_port(), .id_mutex = PTHREAD_MUTEX_INITIALIZER};
	assert(juice_mux_listen_pending("127.0.0.1", listener.port, NULL, suspended, &listener) == 0);
	uint16_t source;
	int fd = test_socket(&source);
	juice_agent_t *agent = accepted ? test_agent(listener.port) : NULL;
	// Queue both datagrams before the mux can read either. The unaccepted second
	// request makes the stop transition observable while the callback is paused.
	if (agent) mutex_lock(&agent->registry->mutex);
	test_send(fd, listener.port, "host", "peer", test_password);
	if (agent) {
		test_send(fd, listener.port, "host", "peer2", test_password);
		mutex_unlock(&agent->registry->mutex);
	}
	for (int i = 0; i < 400 && !atomic_load(&listener.entered); ++i) usleep(5000);
	assert(atomic_load(&listener.entered));
	// Metadata callbacks hold neither the per-registry nor global entry lock.
	juice_mux_stats_t stats = test_received(listener.port, 1);
	assert(stats.pending == (accepted ? 2u : 1u) && stats.agents == (accepted ? 1 : 0));
	if (agent) {
		pthread_mutex_lock(&listener.id_mutex);
		uint64_t request_id = listener.request_id;
		pthread_mutex_unlock(&listener.id_mutex);
		assert(juice_mux_verify_request("127.0.0.1", listener.port, request_id, test_password) == 0);
		assert(juice_mux_attach_request("127.0.0.1", listener.port, request_id, agent) == 0);
	}
	pthread_t remover;
	assert(pthread_create(&remover, NULL, remove_listener, &listener) == 0);
	for (int i = 0; i < 200 && !atomic_load(&listener.removing); ++i) usleep(5000);
	assert(atomic_load(&listener.removing));
	for (int i = 0; i < 200; ++i) {
		assert(juice_mux_get_stats("127.0.0.1", listener.port, &stats) == 0);
		if (stats.pending != (accepted ? 2u : 1u)) break;
		usleep(5000);
	}
	assert(stats.pending == (accepted ? 1u : 0u) && !atomic_load(&listener.removed));
	atomic_store(&listener.release, true);
	assert(pthread_join(remover, NULL) == 0);
	assert(atomic_load(&listener.removed));
	if (agent) { test_response(fd); juice_destroy(agent); }
	assert(juice_mux_get_stats("127.0.0.1", listener.port, &stats) == JUICE_ERR_NOT_AVAIL);
	close(fd);
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	assert(fd >= 0);
	struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(listener.port)};
	assert(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
	assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
	close(fd);
	pthread_mutex_destroy(&listener.id_mutex);
}

int main(void) {
	juice_set_log_level(JUICE_LOG_LEVEL_FATAL);
	run_case(false);
	run_case(true);
	puts("pending ICE lifetime: callback outside locks, cancellation barrier, accepted first packet survives stop PASS");
}
