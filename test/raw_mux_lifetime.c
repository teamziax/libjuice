/* SPDX-License-Identifier: MPL-2.0 */
#include <juice/juice.h>

#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct listener {
	atomic_bool entered;
	atomic_bool release;
	atomic_bool removing;
	atomic_bool removed;
	int port;
} listener_t;

static bool suspended_callback(const void *data, size_t size, const char *address,
                              uint16_t port, void *ptr) {
	listener_t *listener = ptr;
	const unsigned char malformed[] = {0xff, 0x01, 0x02};
	assert(size == sizeof(malformed) && memcmp(data, malformed, size) == 0);
	assert(strcmp(address, "127.0.0.1") == 0 && port > 0);
	atomic_store(&listener->entered, true);
	// Deliberately suspend this test callback to observe the removal barrier.
	// Applications must keep callbacks bounded and never block on other threads.
	for (int i = 0; i < 200 && !atomic_load(&listener->release); ++i)
		usleep(5000);
	assert(atomic_load(&listener->release));
	return false;
}

static void *remove_listener(void *ptr) {
	listener_t *listener = ptr;
	atomic_store(&listener->removing, true);
	assert(juice_mux_listen_raw("127.0.0.1", listener->port, NULL, NULL) == 0);
	atomic_store(&listener->removed, true);
	return NULL;
}

static void parsed(const juice_mux_binding_request_t *request, void *ptr) {
	(void)request;
	(void)ptr;
	assert(!"parsed listener must not replace a raw gate");
}

int main(void) {
	juice_set_log_level(JUICE_LOG_LEVEL_ERROR);
	listener_t listener = {.port = 49183};
	assert(juice_mux_listen_raw("127.0.0.1", listener.port, suspended_callback, &listener) == 0);
	assert(juice_mux_listen("127.0.0.1", listener.port, parsed, NULL) != 0);
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	assert(fd >= 0);
	struct sockaddr_in dst = {.sin_family = AF_INET, .sin_port = htons(listener.port)};
	assert(inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr) == 1);
	const unsigned char malformed[] = {0xff, 0x01, 0x02};
	assert(sendto(fd, malformed, sizeof(malformed), 0, (struct sockaddr *)&dst,
	              sizeof(dst)) == sizeof(malformed));
	for (int i = 0; i < 200 && !atomic_load(&listener.entered); ++i)
		usleep(5000);
	assert(atomic_load(&listener.entered));
	pthread_t remover;
	assert(pthread_create(&remover, NULL, remove_listener, &listener) == 0);
	for (int i = 0; i < 200 && !atomic_load(&listener.removing); ++i)
		usleep(5000);
	assert(atomic_load(&listener.removing));
	usleep(20000);
	assert(!atomic_load(&listener.removed));
	atomic_store(&listener.release, true);
	assert(pthread_join(remover, NULL) == 0);
	assert(atomic_load(&listener.removed));
	juice_mux_stats_t stats;
	assert(juice_mux_get_stats("127.0.0.1", listener.port, &stats) == JUICE_ERR_NOT_AVAIL);
	// The callback context is now safe to release and the UDP endpoint is reusable.
	close(fd);
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	assert(fd >= 0);
	assert(bind(fd, (struct sockaddr *)&dst, sizeof(dst)) == 0);
	close(fd);
	puts("raw-mux lifetime: malformed raw bytes, exclusive listener, removal barrier and release PASS");
	return 0;
}
