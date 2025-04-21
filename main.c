#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define COLUMN 10
#define ROW 8

const char* cells[][3] = {
	{
		"       ",
		"       ",
		"       ",
	},
	{
		"/ ... \\",
		" .   . ",
		"\\ ... /",
	},
	{
		"\\ =#= /",
		" >#@#< ",
		"/ =#= \\",
	},
	{
		"=======",
		"xxxxxxx",
		"=======",
	},
};

struct termios old_terminal_attr;

typedef struct Size {
	uint16_t x;
	uint16_t y;
} Size;

typedef struct Buffer {
	char** ptr;
	Size size;
} Buffer;

typedef enum Page {
	Greeting = 0,
	Creating,
	Join,
	WaitingClient,
	WaitingServer,
	Game,
	Error,
} Page;

typedef enum GreetingSelection {
	GreetingNone = -1,
	GreetingCreate = 0,
	GreetingJoin,
	GreetingExit,
} GreetingSelection;

typedef enum CreatingSelection {
	CreatingTyping = 0,
	CreatingInput,
	CreatingCreate,
	CreatingExit,
} CreatingSelection;

typedef enum JoinSelection {
	JoinTyping = 0,
	JoinInput,
	JoinCreate,
	JoinExit,
} JoinSelection;

typedef struct DynamicString {
	char* ptr;
	size_t len;
	size_t cap;
} DynamicString;

typedef struct Status {
	bool running;
	Page page;
	int sock_fd;
	int map[ROW][COLUMN];
	struct {
		GreetingSelection selection;
	} greeting;
	struct {
		CreatingSelection selection;
		int32_t port;
	} creating;
	struct {
		JoinSelection selection;
		char connect_addr[32]; 
		size_t cursor;
	} join;
} Status;

void free_buffer(Buffer* buf) {
	for (int i = 0; i < buf->size.y; i++) {
		free(buf->ptr[i]);
		buf->ptr[i] = NULL;
	}
	free(buf->ptr);
	buf->ptr = NULL;
}

DynamicString dynamic_string_new() {
	return (DynamicString){
		.ptr = malloc(256 * sizeof(char)),
		.len = 0,
		.cap = 256,
	};
}

void dynamic_string_push(DynamicString* str, char ch) {
	str->len += 1;
	if (str->cap < str->len + 1) {
		str->cap *= 2;
		str->ptr = realloc(str->ptr, str->cap);
	}
	str->ptr[str->len - 1] = ch;
	str->ptr[str->len] = '\0';
}

Buffer main_ui(int status[ROW][COLUMN]) {
	int width = 7;
	int height = 3;
	int full_width = width + 3;
	int full_height = height + 1;

	uint16_t x = full_width * COLUMN + 1;
	uint16_t y = full_height * ROW + 1;

	char** arr = malloc(y * sizeof(char*));
	for (int i = 0; i < y; i++) {
		arr[i] = malloc((x + 1) * sizeof(char));
		if (i % full_height == 0) {
			memset(arr[i], '-', x);
			for (int j = 0; j < x / full_width + 1; j++) {
				arr[i][j * full_width] = '+';
			}
		} else {
			memset(arr[i], ' ', x);
			for (int j = 0; j < x / full_width + 1; j++) {
				arr[i][j * full_width] = '|';
				assert(j * full_width < x);
			}
			// content of a cell
			for (int j = 0; j < x / full_width; j++) {
				// TODO: content
				int type = status[i / full_height][j];
				const char* content = cells[type][i % full_height - 1];
				memcpy(&arr[i][j * full_width + 2], content, width);
			}
		}
		arr[i][x] = '\0';
	}

	return (Buffer){
		.ptr = arr,
		.size = { .x = x, .y = y, },
	};
}

Buffer greeting_options(GreetingSelection selection) {
	char* options[] = {
		"- Start a game",
		"- Join a game ",
		"- Exit        ",
	};

	uint16_t x = strlen(options[0]);
	uint16_t y = sizeof(options)/ sizeof(options[0]);

	char** arr = malloc(y * sizeof(char*));

	for (int i = 0; i < 3; i++) {
		char* color_start = "";
		char* color_end = "";
		if (i == selection) {
			color_start = "\e[7m";
			color_end = "\e[0m";
		}
		char* buf;
		asprintf(&buf, "%s%s%s", color_start, options[i], color_end);
		arr[i] = buf;
	}

	return (Buffer){
		.ptr = arr,
		.size = { .x = x, .y = y, },
	};
}

Buffer creating_options(int32_t port, CreatingSelection selection) {
	char* color_start = "";
	char* color_end = "";
	char* buf;
	if (selection == CreatingTyping) {
		color_start = "\e[7m";
		color_end = "\e[0m";
	}
	if (port == -1) {
		asprintf(&buf, "Port: %s%6s%s", color_start, "", color_end);
	} else {
		asprintf(&buf, "Port: %s%6d%s", color_start, port, color_end);
	}

	uint16_t x = strlen("Port: ") + 6;
	uint16_t y = 3;

	char** arr = malloc(y * sizeof(char*));

	color_start = "";
	color_end = "";
	if (selection == CreatingInput) {
		color_start = "\e[7m";
		color_end = "\e[0m";
	}
	asprintf(&arr[0], "%s%s%s", color_start, buf, color_end);
	free(buf);

	char* options[2] = {
		"- Create",
		"- Cancel",
	};
	char* padding = malloc((x - strlen(options[0])) / 2 + 1);
	memset(padding, ' ', (x - strlen(options[0])) / 2);
	padding[(x - strlen(options[0])) / 2] = '\0';
	char* right = "";
	if ((x - strlen(options[0])) % 2 != 0) {
		right = " ";
	}
	for (int i = 0; i < 2; i++) {
		char* color_start = "";
		char* color_end = "";
		if (i == selection - 2) {
			color_start = "\e[7m";
			color_end = "\e[0m";
		}
		asprintf(&arr[i + 1], "%s%s%s%s%s%s", color_start, padding, options[i], padding, right, color_end);
	}

	return (Buffer){
		.ptr = arr,
		.size = { .x = x, .y = y, },
	};
}

Buffer join_options(char* addr, JoinSelection selection) {
	// 123.123.123.123:12345
	char* color_start = "";
	char* color_end = "";
	char* buf;
	if (selection == JoinTyping) {
		color_start = "\e[7m";
		color_end = "\e[0m";
	}
	asprintf(&buf, "Address: %s%22s%s", color_start, addr, color_end);

	uint16_t x = strlen("Address: ") + 22;
	uint16_t y = 3;

	char** arr = malloc(y * sizeof(char*));

	color_start = "";
	color_end = "";
	if (selection == JoinInput) {
		color_start = "\e[7m";
		color_end = "\e[0m";
	}
	asprintf(&arr[0], "%s%s%s", color_start, buf, color_end);
	free(buf);

	char* options[2] = {
		"- Create",
		"- Cancel",
	};
	char* padding = malloc((x - strlen(options[0])) / 2 + 1);
	memset(padding, ' ', (x - strlen(options[0])) / 2);
	padding[(x - strlen(options[0])) / 2] = '\0';
	char* right = "";
	if ((x - strlen(options[0])) % 2 != 0) {
		right = " ";
	}
	for (int i = 0; i < 2; i++) {
		char* color_start = "";
		char* color_end = "";
		if (i == selection - 2) {
			color_start = "\e[7m";
			color_end = "\e[0m";
		}
		asprintf(&arr[i + 1], "%s%s%s%s%s%s", color_start, padding, options[i], padding, right, color_end);
	}

	return (Buffer){
		.ptr = arr,
		.size = { .x = x, .y = y, },
	};
}

Buffer waiting_client(uint16_t port) {
	uint16_t y = 3;

	char** arr = malloc(y * sizeof(char*));

	arr[0] = strdup("Waiting for connection...");
	arr[2] = strdup("                         ");
	char* old;
	asprintf(&old, "port %d", port); 

	char padding[64];
	memset(padding, ' ', (strlen(arr[0]) - strlen(old)) / 2);
	padding[(strlen(arr[0]) - strlen(old)) / 2] = '\0';
	char* right = "";
	if ((strlen(arr[0]) - strlen(old)) % 2 != 0) {
		right = " ";
	}
	asprintf(&arr[1], "%s%s%s%s", padding, old, padding, right); 
	free(old);

	uint16_t x = strlen(arr[0]);

	return (Buffer){
		.ptr = arr,
		.size = { .x = x, .y = y, },
	};
}

Buffer waiting_server(char* addr) {
	uint16_t y = 3;

	char** arr = malloc(y * sizeof(char*));

	arr[0] = strdup("  Waiting for connection...  ");
	arr[2] = strdup("                             ");
	char* old;
	asprintf(&old, "Address %s", addr); 

	char padding[64];
	memset(padding, ' ', (strlen(arr[0]) - strlen(old)) / 2);
	padding[(strlen(arr[0]) - strlen(old)) / 2] = '\0';
	char* right = "";
	if ((strlen(arr[0]) - strlen(old)) % 2 != 0) {
		right = " ";
	}
	asprintf(&arr[1], "%s%s%s%s", padding, old, padding, right); 
	free(old);

	uint16_t x = strlen(arr[0]);

	return (Buffer){
		.ptr = arr,
		.size = { .x = x, .y = y, },
	};
}

Buffer greeting_screen(Buffer options) {
	int width = 7;
	int height = 3;
	int full_width = width + 3;
	int full_height = height + 1;

	uint16_t x = full_width * 8 + 1;
	uint16_t y = full_height * 5 + 1;

	char** arr = malloc(y * sizeof(char*));

	char* top_part[] = {
		"+---------+---------+---------+---------+---------+---------+---------+---------+",
		"|         | / ... \\ | / ... \\ |         |         |         |         | \\ =#= / |",
		"|         |  .   .  |  .   .  |         |         |         |         |  >#@#<  |",
		"|         | \\ ... / | \\ ... / |         |         |         |         | / =#= \\ |",
		"+---------+---------+---------+---------+---------+---------+---------+---------+",
		"| \\ =#= / |        ____        __  __  __          __    _            |         |",
		"|  >#@#<  |       / __ )____ _/ /_/ /_/ /__  _____/ /_  (_)___        |         |",
		"| / =#= \\ |      / __  / __ `/ __/ __/ / _ \\/ ___/ __ \\/ / __ \\       |         |",
		"+---------+     / /_/ / /_/ / /_/ /_/ /  __(__  ) / / / / /_/ /       +---------+",
		"|         |    /_____/\\__,_/\\__/\\__/_/\\___/____/_/ /_/_/ .___/        |         |",
		"|         |                                           /_/             |         |",
		"|         |                                               by Shiphan  |         |",
		"+---------+---------+---------+---------+---------+---------+---------+---------+",
		"| \\ =#= / | \\ =#= / |         |         |         |  /-X--- | ---X--- | ---X-\\  |",
		"|  >#@#<  |  >#@#<  |         |         |         | | X#X   |   X#X   |   X#X | |",
		"| / =#= \\ | / =#= \\ |         |         |         |  \\-X--- | ---X--- | ---X-/  |",
		"+---------+---------+---------+---------+---------+---------+---------+---------+",
	};
	for (int i = 0; i < sizeof(top_part) / sizeof(top_part[0]); i++) {
		arr[i] = strdup(top_part[i]);
	}
	for (int i = 0; i < 3; i++) {
		char* buffer;
		char* other = "|         |         |";
		char padding[64];
		memset(padding, ' ', (39 - options.size.x) / 2);
		padding[(39 - options.size.x) / 2] = '\0';
		char* right = "";
		if ((39 - options.size.x) % 2 != 0) {
			right = " ";
		}
		asprintf(&buffer, "%s%s%s%s%s%s", other, padding, options.ptr[i], padding, right, other);
		
		arr[sizeof(top_part) / sizeof(top_part[0]) + i] = buffer;
	}
	arr[y - 1] = strdup("+---------+---------+---------+---------+---------+---------+---------+---------+");
	
	free_buffer(&options);

	return (Buffer){
		.ptr = arr,
		.size = { .x = x, .y = y, },
	};
}

Buffer error_screen() {
	char** arr = malloc(1 * sizeof(char*));
	asprintf(&arr[0], "Error: %s (%d)", strerror(errno), errno);

	return (Buffer){
		.ptr = arr,
		.size = { .x = strlen(arr[0]), .y = 1, },
	};
}

char* ui_wrapper(Buffer buf, Size size) {
	if (buf.size.x > size.x || buf.size.y > size.y) {
		free_buffer(&buf);

		char* result;
		int err = asprintf(
			&result,
			"The terminal is too small (%d x %d), and it should at least be %d x %d.\n",
			size.x, size.y, buf.size.x, buf.size.y
		);
		assert(err != -1);

		if (size.y > 0 && strlen(result) <= size.x) {
			char** arr = malloc(1 * sizeof(char*));
			arr[0] = result;
			return ui_wrapper((Buffer){
				.ptr = arr,
				.size = { .x = strlen(result), .y = 1, },
			}, size);
		}

		return result;
	}

	uint16_t top = (size.y - buf.size.y) / 2;
	uint16_t left = (size.x - buf.size.x) / 2;

	char padding = ' ';
	char* padding_s = " ";
	char* y_padding = malloc((size.x + 1) * sizeof(char));
	memset(y_padding, padding, size.x);
	y_padding[size.x] = '\0';

	char* x_padding = malloc((left + 1) * sizeof(char));
	memset(x_padding, padding, left);
	x_padding[left] = '\0';

	char* right_padding = "";
	if ((size.x - buf.size.x) % 2 != 0) {
		right_padding = padding_s;
	}

	char* result = malloc(1 * sizeof(char));
	result[0] = '\0';
	for (int line = 0; line < size.y; line++) {
		char* old_result = result;
		result = NULL;
		// top & bottom padding
		if (line < top || line - top >= buf.size.y) {
			int err = asprintf(&result, "%s\n%s", old_result, y_padding); 
			assert(err != -1);
		} else {
			int err = asprintf(&result, "%s\n%s%s%s%s", old_result, x_padding, buf.ptr[line - top], x_padding, right_padding);
			assert(err != -1);
		}
		free(old_result);
	}
	free(y_padding);

	free_buffer(&buf);

	return result;
}

Size termial_size(void) {
	struct winsize window_size;
	int err = ioctl(STDOUT_FILENO, TIOCGWINSZ, &window_size); 
	assert(err != -1);
	return (Size){
		.x = window_size.ws_col,
		.y = window_size.ws_row,
	};
}

void enter_alter_screen(void) {
	// 7: save cursor position
	// [?47h: alter screen
	// [?25l: hide cursor
	printf("\e7" "\e[?47h" "\e[?25l");

	struct termios attr;
	tcgetattr(STDIN_FILENO, &attr);
	old_terminal_attr = attr;
	attr.c_lflag &= ~(ECHO | ICANON);
	// cfmakeraw(&attr);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &attr);
}

void leave_alter_screen() {
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_terminal_attr);
	printf("\e[2J\e[?25h\e[?47l\e8");
}

void print_ui(Buffer buf) {
	Size size = termial_size();
	char* buffer = ui_wrapper(buf, size);
	// \e[2j: remove old output
	printf("\e[2J%s", buffer);
	fflush(stdout);
	free(buffer);
}

void handle_greeting_key_event(Status* status, int key) {
	GreetingSelection* selection = &status->greeting.selection;
	switch (key) {
		case 'j':
			if (*selection < 0 || *selection > GreetingExit) {
				*selection = GreetingCreate;
			} else if (*selection < GreetingExit) {
				*selection += 1;
			}
			break;
		case 'k':
			if (*selection < 0 || *selection > GreetingExit) {
				*selection = GreetingCreate;
			} else if (*selection > 0){
				*selection -= 1;
			}
			break;
		case '\n':
			switch (*selection) {
				case GreetingNone:
					break;
				case GreetingCreate:
					status->page = Creating;
					break;
				case GreetingJoin:
					status->page = Join;
					break;
				case GreetingExit:
					status->running = false;
					break;
			}
			break;
	}
}

void handle_creating_key_event(Status* status, int key) {
	if (status->creating.selection == CreatingTyping) {
		if (key >= '0' && key <= '9') {
			if (status->creating.port == -1) {
				status->creating.port = 0;
			}
			int32_t new_port = status->creating.port * 10 + (key - '0');
			if (new_port <= UINT16_MAX) {
				status->creating.port = new_port;
			}
		} else if (key == '\x7f') {
			int32_t new_port = status->creating.port / 10;
			if (new_port == 0) {
				status->creating.port = -1;
			} else {
				status->creating.port = new_port;
			}
		} else if (key == '\e' || key == '\n') {
			// status->creating.port = key;
			status->creating.selection = CreatingInput;
		} else {
			// status->creating.port = key;
		}
		return;
	}
	CreatingSelection* selection = &status->creating.selection;
	switch (key) {
		case 'j':
			if (*selection < 0 || *selection > CreatingExit) {
				*selection = CreatingInput;
			} else if (*selection < CreatingExit) {
				*selection += 1;
			}
			break;
		case 'k':
			if (*selection < 0 || *selection > CreatingExit) {
				*selection = CreatingInput;
			} else if (*selection > 1){
				*selection -= 1;
			}
			break;
		case '\n':
			switch (*selection) {
				case CreatingTyping:
					break;
				case CreatingInput:
					status->creating.selection = CreatingTyping;
					break;
				case CreatingCreate: {
					struct sockaddr_in addr;
					addr.sin_family = AF_INET;
					addr.sin_port = htons(status->creating.port);
					addr.sin_addr.s_addr = htonl(INADDR_ANY);

					int err = bind(status->sock_fd, (struct sockaddr*)&addr, sizeof(addr));
					if (err < 0) {
						status->page = Error;
						break;
					}
					err = listen(status->sock_fd, 1);
					if (err < 0) {
						status->page = Error;
						break;
					}
					
					status->page = WaitingClient;
					break;
				}
				case CreatingExit:
					status->page = Greeting;
					break;
			}
			break;
		case 'i': case 'a':
			if (*selection == CreatingInput) {
				status->creating.selection = CreatingTyping;
			}
			break;
	}
}

void handle_join_key_event(Status* status, int key) {
	if (status->join.selection == JoinTyping) {
		if ((key >= '0' && key <= '9') || (key >= 'a' && key <= 'z') || key == '.' || key == ':') {
			if (status->join.cursor > 21) {
				return;
			}
			status->join.connect_addr[status->join.cursor] = key;
			status->join.cursor += 1;
		} else if (key == '\x7f') {
			if (status->join.cursor > 0) {
				status->join.cursor -= 1;
				status->join.connect_addr[status->join.cursor] = '\0';
			}
		} else if (key == '\e' || key == '\n') {
			status->join.selection = JoinInput;
		}
		return;
	}
	JoinSelection* selection = &status->join.selection;
	switch (key) {
		case 'j':
			if (*selection < 0 || *selection > JoinExit) {
				*selection = JoinInput;
			} else if (*selection < JoinExit) {
				*selection += 1;
			}
			break;
		case 'k':
			if (*selection < 0 || *selection > JoinExit) {
				*selection = JoinInput;
			} else if (*selection > 1){
				*selection -= 1;
			}
			break;
		case '\n':
			switch (*selection) {
				case JoinTyping:
					break;
				case JoinInput:
					status->join.selection = JoinTyping;
					break;
				case JoinCreate: {
					status->page = WaitingServer;
					break;
				}
				case JoinExit:
					status->page = Greeting;
					break;
			}
			break;
		case 'i': case 'a':
			if (*selection == JoinInput) {
				status->join.selection = JoinTyping;
			}
			break;
	}
}

void handle_key_event(Status* status) {
	struct pollfd fds = { .fd = STDIN_FILENO, .events = POLLIN };
	while (poll(&fds, 1, 0) > 0) {
		int key = getchar();
		switch (status->page) {
			case Greeting:
				handle_greeting_key_event(status, key);
				break;
			case Creating:
				handle_creating_key_event(status, key);
				break;
			case Join:
				handle_join_key_event(status, key);
				break;
			case WaitingClient:
				break;
			case WaitingServer:
				break;
			case Game:
				break;
			case Error:
				break;
		}
	}
}

void handle_actions(Status* status) {
	switch (status->page) {
		case Greeting:
		case Creating:
		case Join:
		case Error:
			break;
		case WaitingClient: {
			struct sockaddr_in addr;
			uint addr_len = sizeof(addr);
			int accepted_fd = accept(status->sock_fd, (struct sockaddr*)&addr, &addr_len);
			if (accepted_fd != -1) {
				status->page = Game;
				char* buf = "hi from server\n";
				write(accepted_fd, buf, strlen(buf));
			} else if (errno != EAGAIN && errno != EWOULDBLOCK) {
				status->page = Error;
			}
			break;
		}
		case WaitingServer: {
			char buf[32];
			strcpy(buf, status->join.connect_addr);
			char* addr_ip;
			char* addr_port;
			{
				char* save;
				addr_ip = strtok_r(buf, ":", &save);
				addr_port = strtok_r(NULL, ":", &save);
				assert(addr_ip == buf);
				assert(addr_port != NULL);
				assert(addr_port != buf);
			}
			uint16_t port;
			{
				char* end;
				ulong tmp_port = strtoul(addr_port, &end, 10);
				assert(end != addr_port);
				assert(tmp_port <= UINT16_MAX);
				port = tmp_port;
			}

			struct sockaddr_in addr;
			addr.sin_family = AF_INET;
			addr.sin_port = htons(port);
			if (strcmp(addr_ip, "localhost") == 0) {
				addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			} else {
				in_addr_t tmp_addr = inet_addr(addr_ip);
				if (tmp_addr == -1) {
					status->page = Error;
					break;
				}
				addr.sin_addr.s_addr = tmp_addr;
			}

			int err = connect(status->sock_fd, (struct sockaddr*)&addr, sizeof(addr));
			if (err == 0) {
				status->page = Game;
				char* buf = "hi from client\n";
				write(status->sock_fd, buf, strlen(buf));
			} else if (errno != EAGAIN && errno != EALREADY && errno != EINPROGRESS) {
				status->page = Error;
			}
			break;
		}
		case Game:
			// TODO: handle
			break;
	}
}

void ctrl_c_handler(int sig) {
	assert(sig == SIGINT);
	leave_alter_screen();
	exit(0);
}

void ctrl_c(void) {
	struct sigaction sa;

	sa.sa_handler = ctrl_c_handler;
	int err = sigemptyset(&sa.sa_mask);
	assert(err != -1);
	sa.sa_flags = 0;
	err = sigaction(SIGINT, &sa, NULL);
	assert(err != -1);
}

int main(int argc, char** argv) {
	ctrl_c();
	enter_alter_screen();

	// bool running;
	// Page page;
	// int sock_fd;
	// int map[ROW][COLUMN];
	// struct {
	// 	GreetingSelection selection;
	// } greeting;
	// struct {
	// 	CreatingSelection selection;
	// 	int32_t creating_port;
	// 	char* connect_addr; 
	// } creating;
	// 111.111.111.111:12345
	Status status = {
		.running = true,
		.page = Greeting,
		.sock_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0),
		.map = {0},
		.greeting = {
			.selection = GreetingNone,
		},
		.creating = {
			.selection = CreatingTyping,
			.port = -1,
		},
		.join = {
			.selection = JoinTyping,
			// .connect_addr = 
			.cursor = 0,
		},
	};

	status.map[1][2] = 2;
	status.map[0][3] = 1;

	while (status.running) {
		handle_key_event(&status);

		handle_actions(&status);

		switch (status.page) {
			case Greeting:
				print_ui(greeting_screen(greeting_options(status.greeting.selection)));
				break;
			case Creating:
				print_ui(greeting_screen(creating_options(status.creating.port, status.creating.selection)));
				break;
			case Join:
				print_ui(greeting_screen(join_options(status.join.connect_addr, status.join.selection)));
				break;
			case WaitingClient:
				print_ui(greeting_screen(waiting_client(status.creating.port)));
				break;
			case WaitingServer:
				print_ui(greeting_screen(waiting_server(status.join.connect_addr)));
				break;
			case Game:
				print_ui(main_ui(status.map));
				break;
			case Error:
				print_ui(error_screen());
				break;
		}
		usleep(1000*1000/60);
	}

	leave_alter_screen();
	close(status.sock_fd);
	return 0;
}
