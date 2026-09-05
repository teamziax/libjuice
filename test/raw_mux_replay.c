/* SPDX-License-Identifier: MPL-2.0 */
/* Loopback-only deferred first-request regression. */
#include <juice/juice.h>
#include "stun.h"

#include <arpa/inet.h>
#include <assert.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char password[] = "012345678901234567890123456789";

typedef struct ingress {
	atomic_int calls;
	atomic_bool ready;
	unsigned char first[2048];
	size_t size;
	uint16_t source_port;
} ingress_t;

static bool defer(const void *data, size_t size, const char *address, uint16_t port, void *ptr) {
	ingress_t *ingress = ptr;
	assert(strcmp(address, "127.0.0.1") == 0);
	assert(size <= sizeof(ingress->first));
	if (atomic_load(&ingress->calls) == 0) {
		memcpy(ingress->first, data, size);
		ingress->size = size;
		ingress->source_port = port;
	}
	atomic_fetch_add(&ingress->calls, 1);
	return atomic_load(&ingress->ready);
}

static int bind_socket(uint16_t *port) {
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	assert(fd >= 0);
	struct sockaddr_in address = {.sin_family = AF_INET};
	assert(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
	assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
	socklen_t size = sizeof(address);
	assert(getsockname(fd, (struct sockaddr *)&address, &size) == 0);
	*port = ntohs(address.sin_port);
	return fd;
}

static void run_case(bool valid_integrity) {
	uint16_t port, source_port;
	int reservation = bind_socket(&port);
	close(reservation);
	int fd = bind_socket(&source_port);
	ingress_t ingress = {0};
	assert(juice_mux_listen_raw("127.0.0.1", port, defer, &ingress) == 0);

	stun_message_t request = {0};
	request.msg_class = STUN_CLASS_REQUEST;
	request.msg_method = STUN_METHOD_BINDING;
	request.priority = 123456;
	request.ice_controlling = 123456789;
	memcpy(request.transaction_id, "firstrequest", STUN_TRANSACTION_ID_SIZE);
	strcpy(request.credentials.username, "host:peer");
	unsigned char datagram[2048];
	int size = _juice_stun_write(datagram, sizeof(datagram), &request,
	                           valid_integrity ? password : "incorrect-password-value");
	assert(size > 0);
	struct sockaddr_in destination = {.sin_family = AF_INET, .sin_port = htons(port)};
	assert(inet_pton(AF_INET, "127.0.0.1", &destination.sin_addr) == 1);
	// The client transmits exactly once. All subsequent processing uses the saved request.
	assert(sendto(fd, datagram, size, 0, (struct sockaddr *)&destination,
	              sizeof(destination)) == size);
	for (int i = 0; i < 200 && atomic_load(&ingress.calls) == 0; ++i)
		usleep(5000);
	assert(atomic_load(&ingress.calls) == 1);
	assert(ingress.source_port == source_port);
	juice_mux_stats_t stats;
	assert(juice_mux_get_stats("127.0.0.1", port, &stats) == 0);
	assert(stats.agents == 0 && stats.mapped_tuples == 0 && stats.rejected == 1);

	// Peer construction happens on this owning thread, after the callback returns.
	juice_config_t config = {0};
	config.concurrency_mode = JUICE_CONCURRENCY_MODE_MUX;
	config.bind_address = "127.0.0.1";
	config.local_port_range_begin = config.local_port_range_end = port;
	juice_agent_t *agent = juice_create(&config);
	assert(agent);
	assert(juice_set_local_ice_attributes(agent, "host", password) == 0);
	assert(juice_set_remote_description(agent,
	    "a=ice-ufrag:peer\r\na=ice-pwd:012345678901234567890123456789\r\n") == 0);
	assert(juice_gather_candidates(agent) == 0);
	atomic_store(&ingress.ready, true);
	assert(juice_mux_replay("127.0.0.1", port, "127.0.0.1", source_port,
	                        ingress.first, ingress.size) == 0);
	// The queue must own its copy by the time replay returns.
	memset(ingress.first, 0xff, sizeof(ingress.first));

	bool success_response = false;
	for (int i = 0; i < 10; ++i) {
		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		int ready = poll(&pfd, 1, 50);
		assert(ready >= 0);
		if (ready == 0)
			continue;
		unsigned char response[2048];
		ssize_t received = recv(fd, response, sizeof(response), 0);
		assert(received > 0);
		stun_message_t message;
		assert(_juice_stun_read(response, received, &message) > 0);
		if (message.msg_class == STUN_CLASS_RESP_SUCCESS &&
		    memcmp(message.transaction_id, request.transaction_id, STUN_TRANSACTION_ID_SIZE) == 0) {
			assert(_juice_stun_check_integrity(response, received, &message, password));
			success_response = true;
			break;
		}
	}
	assert(success_response == valid_integrity);
	assert(atomic_load(&ingress.calls) == 2);
	assert(juice_mux_get_stats("127.0.0.1", port, &stats) == 0);
	assert(stats.received == 2 && stats.rejected == 1 && stats.agents == 1);
	juice_destroy(agent);
	assert(juice_mux_get_stats("127.0.0.1", port, &stats) == 0);
	assert(stats.agents == 0 && stats.mapped_tuples == 0);
	assert(juice_mux_listen_raw("127.0.0.1", port, NULL, NULL) == 0);
	assert(juice_mux_get_stats("127.0.0.1", port, &stats) == JUICE_ERR_NOT_AVAIL);
	close(fd);
}

int main(void) {
	juice_set_log_level(JUICE_LOG_LEVEL_ERROR);
	run_case(true);
	run_case(false);
	puts("raw-mux replay: first-request response, owned bytes, current guard and ICE integrity PASS");
	return 0;
}
