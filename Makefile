CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -O2
ASAN_FLAGS = -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined

BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
TARGET = $(BUILD_DIR)/server
BACKEND_TARGET = $(BUILD_DIR)/test_backend
WORKER_NETWORK_OBJS = $(OBJ_DIR)/worker_cgi_io.o $(OBJ_DIR)/worker_http_io.o $(OBJ_DIR)/worker_proxy_io.o $(OBJ_DIR)/worker_file_io.o $(OBJ_DIR)/worker_file_bridge.o
CGI_DIR = cgi-bin
CGI_BINS = $(CGI_DIR)/order
TEST_NAMES = test_parse_port test_recv_header test_send_all test_http_parser test_body_send test_proxy_selection test_proxy_protocol test_proxy_lifecycle test_shared_backend_state test_backend_reservations test_worker_fd_channel test_parent_client_queue test_worker_failure_channel test_process_signal_wait test_connection_completion test_parent_worker_supervisor test_backend_request_lifetime test_distribution_policy test_worker_http_io test_worker_io_state
TEST_BINS = $(addprefix $(BUILD_DIR)/,$(TEST_NAMES))

WORKER_IO_SRCS = src/worker_cgi_io.c src/worker_file_io.c src/worker_file_bridge.c src/worker_http_io.c src/worker_proxy_io.c src/shared_backend_state.c src/backend_reservations.c src/backend_request_lifetime.c src/worker_closed_channel.c src/worker_fd_channel.c src/process_signal_wait.c src/parent_connection_ledger.c
PROCESS_HTTP_SRCS = src/parent_http_loop.c src/parent_worker_supervisor.c src/parent_client_queue.c src/worker_failure_channel.c

HANDLER_SRCS = src/handlers/basic.c src/handlers/static.c src/handlers/greet.c src/handlers/cgi.c src/handlers/cgi_protocol.c src/handlers/cgi_launch.c
PROXY_SRCS = src/proxy.c src/proxy_transfer.c src/proxy_io.c src/proxy_headers.c src/proxy_body.c
SRCS = src/process_http_server.c $(PROCESS_HTTP_SRCS) $(WORKER_IO_SRCS) src/distribution_policy.c src/main.c src/server.c src/socket_io.c src/http.c src/router.c $(PROXY_SRCS) $(HANDLER_SRCS)
LIFECYCLE_TEST_OBJS = $(patsubst src/%.c,$(BUILD_DIR)/test-obj/%.o,$(PROXY_SRCS))
OBJS = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRCS))
HEADERS = src/worker_file_io.h src/worker_http_io.h src/worker_http_internal.h src/parent_http_loop.h src/concurrency_limits.h src/handlers/cgi_protocol.h src/process_http_server.h src/distribution_policy.h src/server.h src/socket_io.h src/http.h src/router.h src/proxy.h src/proxy_internal.h src/handlers/handlers.h
TEST_SRCS = src/distribution_policy.c src/server.c src/socket_io.c src/http.c src/router.c $(PROXY_SRCS) $(HANDLER_SRCS)

.PHONY: all test verify verify-edge clean server cgi backend worker-io smoke-process-http $(TEST_NAMES)

all: $(TARGET) $(CGI_BINS)

$(BUILD_DIR) $(OBJ_DIR) $(CGI_DIR):
	mkdir -p $@

$(TARGET): $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(OBJS)

$(CGI_DIR)/%: cgi-src/%.c | $(CGI_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(OBJ_DIR)/%.o: src/%.c $(HEADERS)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

server: $(TARGET)

cgi: $(CGI_BINS)

$(TEST_NAMES): %: $(BUILD_DIR)/%

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do ./$$t || exit 1; done

$(BUILD_DIR)/test_body_send: tests/unit/test_body_send.c $(TEST_SRCS) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< src/distribution_policy.c src/socket_io.c src/http.c src/router.c $(PROXY_SRCS) $(HANDLER_SRCS)

$(BUILD_DIR)/test_%: tests/unit/test_%.c $(TEST_SRCS) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< $(TEST_SRCS)

verify: $(TARGET) $(CGI_BINS) $(BACKEND_TARGET)
	python3 scripts/verify_iter1.py
	python3 scripts/verify_iter2.py
	python3 scripts/verify_iter3.py --root public --cafe pages/cafe.html --menu pages/menu.html
	python3 scripts/verify_iter3_dynamic.py
	python3 scripts/verify_iter4.py
	python3 scripts/verify_edge_cases.py

clean:
	rm -f $(OBJS) $(WORKER_NETWORK_OBJS) $(LIFECYCLE_TEST_OBJS) $(TARGET) $(CGI_BINS) $(TEST_BINS) $(BACKEND_TARGET) $(BUILD_DIR)/test_process_http_smoke $(BUILD_DIR)/test_worker_file_io $(BUILD_DIR)/test_cgi

backend: $(BACKEND_TARGET)

$(BACKEND_TARGET): tests/backends/server.c src/socket_io.c src/socket_io.h src/http.c src/http.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $< src/socket_io.c src/http.c

$(BUILD_DIR)/test_proxy_protocol: tests/unit/test_proxy_protocol.c $(TEST_SRCS) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< $(TEST_SRCS)

verify-edge: $(TARGET) $(BACKEND_TARGET)
	python3 scripts/verify_edge_cases.py

# Allocation injection uses separate objects; no GNU linker-only --wrap dependency.
$(BUILD_DIR)/test-obj/%.o: src/%.c $(HEADERS)
	mkdir -p $(dir $@)
	$(CC) $(ASAN_FLAGS) -Dmalloc=proxy_test_malloc -Dcalloc=proxy_test_calloc -Dfree=proxy_test_free -c -o $@ $<

$(BUILD_DIR)/test_proxy_lifecycle: tests/unit/test_proxy_lifecycle.c $(TEST_SRCS) $(HEADERS) $(LIFECYCLE_TEST_OBJS) | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< $(filter-out $(PROXY_SRCS),$(TEST_SRCS)) $(LIFECYCLE_TEST_OBJS)

# Shared-state initialization is independent until the worker flow is integrated.
$(BUILD_DIR)/test_shared_backend_state: tests/unit/test_shared_backend_state.c src/shared_backend_state.c src/shared_backend_state.h | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -pthread -o $@ $< src/shared_backend_state.c

$(BUILD_DIR)/test_backend_reservations: tests/unit/test_backend_reservations.c src/backend_reservations.c src/shared_backend_state.c src/shared_backend_state.h | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -pthread -DSHARED_BACKEND_FAULT_TEST -o $@ $< src/backend_reservations.c src/shared_backend_state.c src/distribution_policy.c

$(BUILD_DIR)/test_worker_fd_channel: tests/unit/test_worker_fd_channel.c src/worker_fd_channel.c src/worker_fd_channel.h | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -DWORKER_FD_CHANNEL_TEST -o $@ $< src/worker_fd_channel.c

$(BUILD_DIR)/test_parent_client_queue: tests/unit/test_parent_client_queue.c src/parent_client_queue.c src/parent_client_queue.h src/concurrency_limits.h src/worker_fd_channel.c src/worker_fd_channel.h src/socket_io.h | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< src/parent_client_queue.c src/worker_fd_channel.c

$(BUILD_DIR)/test_worker_failure_channel: tests/unit/test_worker_failure_channel.c src/worker_failure_channel.c src/worker_failure_channel.h src/shared_backend_state.c src/shared_backend_state.h src/backend_reservations.c src/worker_fd_channel.c src/worker_fd_channel.h src/concurrency_limits.h | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -pthread -DWORKER_FAILURE_CHANNEL_TEST -o $@ $< src/worker_failure_channel.c src/shared_backend_state.c src/backend_reservations.c src/worker_fd_channel.c src/distribution_policy.c

$(BUILD_DIR)/test_process_signal_wait: tests/unit/test_process_signal_wait.c src/process_signal_wait.c src/process_signal_wait.h src/worker_failure_channel.c src/worker_failure_channel.h src/shared_backend_state.c src/shared_backend_state.h src/parent_client_queue.c src/parent_client_queue.h | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -pthread -DPROCESS_SIGNAL_WAIT_TEST -o $@ $< src/process_signal_wait.c src/worker_failure_channel.c src/shared_backend_state.c src/parent_client_queue.c

$(BUILD_DIR)/test_connection_completion: tests/unit/test_connection_completion.c src/concurrency_limits.h src/parent_connection_ledger.c src/parent_connection_ledger.h src/worker_closed_channel.c src/worker_closed_channel.h src/worker_fd_channel.c src/worker_fd_channel.h src/parent_client_queue.c src/parent_client_queue.h | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -DWORKER_CLOSED_CHANNEL_TEST -o $@ $< src/parent_connection_ledger.c src/worker_closed_channel.c src/worker_fd_channel.c src/parent_client_queue.c src/distribution_policy.c

$(BUILD_DIR)/test_parent_worker_supervisor: tests/unit/test_parent_worker_supervisor.c src/parent_worker_supervisor.c src/parent_worker_supervisor.h src/concurrency_limits.h src/parent_connection_ledger.c src/parent_connection_ledger.h src/shared_backend_state.c src/shared_backend_state.h src/backend_reservations.c src/worker_fd_channel.c src/worker_fd_channel.h src/parent_client_queue.c src/parent_client_queue.h src/process_signal_wait.c src/process_signal_wait.h src/worker_closed_channel.c src/worker_closed_channel.h src/distribution_policy.c src/distribution_policy.h | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -pthread -DPARENT_WORKER_SUPERVISOR_TEST -DWORKER_FD_CHANNEL_TEST -o $@ $< src/parent_worker_supervisor.c src/parent_connection_ledger.c src/shared_backend_state.c src/backend_reservations.c src/worker_fd_channel.c src/parent_client_queue.c src/process_signal_wait.c src/worker_closed_channel.c src/distribution_policy.c

$(BUILD_DIR)/test_backend_request_lifetime: tests/unit/test_backend_request_lifetime.c src/backend_request_lifetime.c src/backend_request_lifetime.h src/shared_backend_state.c src/shared_backend_state.h src/backend_reservations.c src/parent_connection_ledger.c src/parent_connection_ledger.h src/distribution_policy.c src/distribution_policy.h | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -pthread -o $@ $< src/backend_request_lifetime.c src/shared_backend_state.c src/backend_reservations.c src/parent_connection_ledger.c src/distribution_policy.c

$(BUILD_DIR)/test_distribution_policy: tests/unit/test_distribution_policy.c src/distribution_policy.c src/distribution_policy.h src/backend_request_lifetime.c src/backend_request_lifetime.h src/shared_backend_state.c src/shared_backend_state.h src/backend_reservations.c src/parent_connection_ledger.c src/parent_connection_ledger.h src/worker_closed_channel.c src/worker_closed_channel.h src/worker_fd_channel.c src/worker_fd_channel.h | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -pthread -DSHARED_BACKEND_FAULT_TEST -o $@ $< src/distribution_policy.c src/backend_request_lifetime.c src/shared_backend_state.c src/backend_reservations.c src/parent_connection_ledger.c src/worker_closed_channel.c src/worker_fd_channel.c

$(BUILD_DIR)/test_worker_http_io: tests/unit/test_worker_http_io.c $(WORKER_IO_SRCS) $(TEST_SRCS) $(HEADERS) src/worker_http_internal.h src/worker_http_io.h src/concurrency_limits.h src/shared_backend_state.h src/worker_fd_channel.h src/worker_closed_channel.h src/backend_request_lifetime.h $(BACKEND_TARGET) | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -pthread -o $@ $< $(WORKER_IO_SRCS) $(TEST_SRCS)

$(BUILD_DIR)/test_worker_io_state: tests/unit/test_worker_io_state.c $(WORKER_IO_SRCS) $(TEST_SRCS) $(HEADERS) src/worker_http_internal.h src/worker_http_io.h src/concurrency_limits.h src/shared_backend_state.h src/worker_fd_channel.h src/worker_closed_channel.h src/backend_request_lifetime.h | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -pthread -o $@ $< $(WORKER_IO_SRCS) $(TEST_SRCS)

# Compile the worker network components without changing the sequential entry.
worker-io: $(WORKER_NETWORK_OBJS)
$(WORKER_NETWORK_OBJS): src/worker_file_io.h src/worker_http_io.h src/worker_http_internal.h src/concurrency_limits.h src/backend_request_lifetime.h src/worker_fd_channel.h src/worker_closed_channel.h src/shared_backend_state.h

$(BUILD_DIR)/test_process_http_smoke: tests/unit/test_process_http_smoke.c $(PROCESS_HTTP_SRCS) $(WORKER_IO_SRCS) $(TEST_SRCS) $(HEADERS) src/parent_http_loop.h src/parent_worker_supervisor.h src/worker_http_internal.h src/worker_http_io.h $(BACKEND_TARGET) src/worker_file_io.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $< $(sort $(PROCESS_HTTP_SRCS) $(WORKER_IO_SRCS) $(TEST_SRCS))
smoke-process-http: $(BUILD_DIR)/test_process_http_smoke
	./$(BUILD_DIR)/test_process_http_smoke

# Directly relevant minimal file-helper check; not the broad sanitizer suite.
$(BUILD_DIR)/test_worker_file_io: tests/unit/test_worker_file_io.c src/worker_file_io.c src/worker_file_io.h $(TEST_SRCS) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $< src/worker_file_io.c $(TEST_SRCS)
smoke-file-io: $(BUILD_DIR)/test_worker_file_io
	./$(BUILD_DIR)/test_worker_file_io
.PHONY: smoke-file-io

$(BUILD_DIR)/test_cgi: tests/backends/cgi.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $<
smoke-process-server: all $(BACKEND_TARGET) $(BUILD_DIR)/test_cgi
	python3 scripts/smoke_process_server.py
.PHONY: smoke-process-server
