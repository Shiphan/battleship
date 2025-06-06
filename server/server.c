#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define KEY_LEN 5
#define BUFFER_LEN 256
#define MINIMAL_CAPACITY 16

typedef struct Entry {
	char key[KEY_LEN + 1];
	int wait_sock_fd;
} Entry;

typedef struct EntryVector {
	Entry* ptr;
	size_t len;
	size_t cap;
	pthread_mutex_t mutex;
} EntryVector;

typedef struct WaitThreadInfo {
	int sock_fd;
	EntryVector* entries;
} WaitThreadInfo;

typedef struct WorkThreadInfo {
	int sock1_fd;
	int sock2_fd;
} WorkThreadInfo;

bool streq(const char* a, const char* b) {
	return strcmp(a, b) == 0;
}

bool is_valid_key(char* key) {
	size_t len = strlen(key);
	if (len != KEY_LEN) {
		return false;
	}
	for (int i = 0; i < len; i++) {
		if (key[i] < 'a' || key[i] > 'z') {
			return false;
		}
	}
	return true;
}

void* work_thread(void* raw_info) {
	WorkThreadInfo* info = (WorkThreadInfo*)raw_info;
	int sock1_fd = info->sock1_fd;
	int sock2_fd = info->sock2_fd;

	char* message = "CONNECTED";
	ssize_t written = write(sock1_fd, message, strlen(message));
	if (written == -1) {
		fprintf(stderr, "[ERROR] %s (line: %d)\n", strerror(errno), __LINE__);
	} else if (written != strlen(message)) {
		fprintf(stderr, "[ERROR] not all readed bytes are written to the sock1 (%ld / %ld) (line: %d)\n", written, strlen(message), __LINE__);
	}

	written = write(sock2_fd, message, strlen(message));
	if (written == -1) {
		fprintf(stderr, "[ERROR] %s (line: %d)\n", strerror(errno), __LINE__);
	} else if (written != strlen(message)) {
		fprintf(stderr, "[ERROR] not all readed bytes are written to the sock2 (%ld / %ld) (line: %d)\n", written, strlen(message), __LINE__);
	}

	struct pollfd fds[2] = {
		{ .fd = sock1_fd, .events = POLLIN },
		{ .fd = sock2_fd, .events = POLLIN },
	};
	bool end = false;
	while (!end) {
		int pollled = poll(fds, 2, -1);
		if (pollled == -1) {
			fprintf(stderr, "[ERROR] error on poll %s (line: %d)\n", strerror(errno), __LINE__);
			break;
		}

		for (size_t i = 0; i < 2; i++) {
			if (fds[i].revents == POLLIN) {
				char buf[BUFFER_LEN] = {0};
				ssize_t readed = read(fds[i].fd, buf, sizeof(buf) - 1);
				if (readed == -1) {
					fprintf(stderr, "[ERROR] %s (line: %d)\n", strerror(errno), __LINE__);
					end = true;
					break;
				} else if (readed == 0) {
					printf("[LOG] a socket ended\n");
					end = true;
					break;
				}

				ssize_t written = write(fds[(i + 1) % 2].fd, buf, readed);
				if (written == -1) {
					fprintf(stderr, "[ERROR] %s (line: %d)\n", strerror(errno), __LINE__);
					end = true;
				} else if (written != readed) {
					fprintf(stderr, "[ERROR] not all readed bytes are written to the write_sock (%ld / %ld) (line: %d)\n", written, readed, __LINE__);
					end = true;
				}
			} else if (fds[i].revents == POLLHUP) {
				printf("[LOG] a socket ended\n");
				end = true;
			} else if (fds[i].revents != 0) {
				fprintf(stderr, "[ERROR] poll return event `%d` (line: %d)\n", fds[i].revents, __LINE__);
				end = true;
			}
		}
	}

	close(sock1_fd);
	close(sock2_fd);
	free(raw_info);
	return NULL;
}

void* wait_thread(void* raw_info) {
	WaitThreadInfo* info = (WaitThreadInfo*)raw_info;
	EntryVector* entries = info->entries;

	printf("[LOG] wait for a key\n");
	char buf[1024] = {0};
	ssize_t readed = read(info->sock_fd, buf, sizeof(buf) - 1);
	if (readed == -1) {
		fprintf(stderr, "[ERROR] %s (line: %d)\n", strerror(errno), __LINE__);
	}
	if (is_valid_key(buf)) {
		printf("[LOG] new key: `%s`\n", buf);
		int err = pthread_mutex_lock(&entries->mutex);
		assert(err == 0);

		bool found = false;
		size_t index = 0;
		while (!found && index < entries->len) {
			if (streq(buf, entries->ptr[index].key)) {
				found = true;
			} else {
				index += 1;
			}
		}
		if (found) {
			int sock1_fd = entries->ptr[index].wait_sock_fd;
			int sock2_fd = info->sock_fd;

			entries->len -= 1;
			for (size_t i = index; i < entries->len; i ++) {
				entries->ptr[i] = entries->ptr[i + 1];
			}

			if (entries->len < entries->cap / 4 && entries->cap / 2 >= MINIMAL_CAPACITY) {
				entries->cap /= 2;
				Entry* new_ptr = realloc(entries->ptr, entries->cap * sizeof(Entry));
				assert(new_ptr != NULL);
				entries->ptr = new_ptr;
			}

			int err = pthread_mutex_unlock(&entries->mutex);
			assert(err == 0);

			WorkThreadInfo* work_info = malloc(sizeof(WorkThreadInfo));
			*work_info = (WorkThreadInfo){
				.sock1_fd = sock1_fd,
				.sock2_fd = sock2_fd,
			};

			pthread_t thread;
			err = pthread_create(&thread, NULL, work_thread, work_info);
			if (err != 0) {
				fprintf(stderr, "[ERROR] %s (line: %d)\n", strerror(err), __LINE__);
			}
			err = pthread_detach(thread);
			if (err != 0) {
				fprintf(stderr, "[ERROR] %s (line: %d)\n", strerror(err), __LINE__);
			}
		} else {
			assert(entries->len <= entries->cap);
			if (entries->len == entries->cap) {
				entries->cap *= 2;
				Entry* new_ptr = realloc(entries->ptr, entries->cap * sizeof(Entry));
				assert(new_ptr != NULL);
				entries->ptr = new_ptr;
			}
			entries->ptr[entries->len] = (Entry){
				.key = {0},
				.wait_sock_fd = info->sock_fd,
			};
			strncpy(entries->ptr[entries->len].key, buf, KEY_LEN);
			entries->len += 1;

			int err = pthread_mutex_unlock(&entries->mutex);
			assert(err == 0);
		}
	} else {
		printf("[LOG] invalid key format\n");
		char* message = "error: invalid connection";
		ssize_t written = write(info->sock_fd, message, strlen(message));
		if (written == -1) {
			fprintf(stderr, "[ERROR] %s (line: %d)\n", strerror(errno), __LINE__);
		} else if (written != strlen(message)) {
			fprintf(stderr, "[ERROR] not all readed bytes are written to the write_sock (%ld / %ld) (line: %d)\n", written, strlen(message), __LINE__);
		}
		close(info->sock_fd);
	}

	free(raw_info);
	return NULL;
}

int main(int argc, char** argv) {
	if (argc != 2) {
		printf("usage: %s <port>\n", argv[0]);
		return 0;
	}

	uint16_t port;
	{
		char* end;
		uint64_t tmp_port = strtoul(argv[1], &end, 10);
		if (end == argv[1]) {
			fprintf(stderr, "[ERROR] the port `%s` is not a number (line: %d)\n", argv[1], __LINE__);
			return 1;
		}
		if (tmp_port > UINT16_MAX) {
			fprintf(stderr, "[ERROR] the port `%s` is too big for a port (line: %d)\n", argv[1], __LINE__);
			return 1;
		}
		port = tmp_port;
	}

	int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (sock_fd == -1) {
		fprintf(stderr, "[ERROR] %s (line: %d)\n", strerror(errno), __LINE__);
		return errno;
	}

	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	int err = bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr));
	if (err == -1) {
		fprintf(stderr, "[ERROR] %s (line: %d)\n", strerror(errno), __LINE__);
		return errno;
	}

	err = listen(sock_fd, SOMAXCONN);
	if (err == -1) {
		return errno;
	}

	EntryVector entries = {
		.ptr = malloc(MINIMAL_CAPACITY * sizeof(Entry)),
		.len = 0,
		.cap = MINIMAL_CAPACITY,
		.mutex = PTHREAD_MUTEX_INITIALIZER,
	};

	while (true) {
		int accepted_fd = accept(sock_fd, NULL, NULL);
		if (accepted_fd == -1) {
			fprintf(stderr, "[ERROR] %s (line: %d)\n", strerror(errno), __LINE__);
			return errno;
		}
		printf("[LOG] a new connection\n");
		
		WaitThreadInfo* info = malloc(sizeof(WaitThreadInfo));
		*info = (WaitThreadInfo){
			.sock_fd = accepted_fd,
			.entries = &entries,
		};
		pthread_t thread;
		int err = pthread_create(&thread, NULL, wait_thread, info);
		if (err != 0) {
			fprintf(stderr, "[ERROR] %s (line: %d)\n", strerror(err), __LINE__);
			return err;
		}
		err = pthread_detach(thread);
		if (err != 0) {
			fprintf(stderr, "[ERROR] %s (line: %d)\n", strerror(err), __LINE__);
			return err;
		}
	}

	return 0;
}

