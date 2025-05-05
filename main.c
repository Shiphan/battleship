#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#define COLUMN 10
#define ROW 12

struct termios old_terminal_attr;
int socket_fd = -1;

const char* cells[][3] = {
	{ // empty
		"       ",
		"       ",
		"       ",
	},
	{ // unknown
		"       ",
		"       ",
		"       ",
	},
	{ // hit
		"\\ =#= /",
		" >#@#< ",
		"/ =#= \\",
	},
	{ // miss
		"/ ... \\",
		" .   . ",
		"\\ ... /",
	},
	{ // ship top
		"  ---  ",
		" /   \\ ",
		"|     |",
	},
	{ // ship bottom
		"|     |",
		" \\   / ",
		"  ---  ",
	},
	{ // ship left
		" /-----",
		"|      ",
		" \\-----",
	},
	{ // ship right
		"-----\\ ",
		"      |",
		"-----/ ",
	},
	{ // ship h
		"-------",
		"       ",
		"-------",
	},
	{ // ship v
		"|     |",
		"|     |",
		"|     |",
	},
	{ // ship top destroyed
		"  -x-  ",
		" /x#x\\ ",
		"|  x  |",
	},
	{ // ship bottom destroyed
		"|  x  |",
		" \\x#x/ ",
		"  -x-  ",
	},
	{ // ship left destroyed
		" /-x---",
		"| x#x  ",
		" \\-x---",
	},
	{ // ship right destroyed
		"---x-\\ ",
		"  x#x |",
		"---x-/ ",
	},
	{ // ship h destroyed
		"---x---",
		"  x#x  ",
		"---x---",
	},
	{ // ship v destroyed
		"|  x  |",
		"| x#x |",
		"|  x  |",
	},
};

typedef enum CellState {
	CellEmpty = 0,
	CellUnknown,
	CellHit,
	CellMiss,
	CellShipTop,
	CellShipBottom,
	CellShipLeft,
	CellShipRight,
	CellShipHorizontal,
	CellShipVertical,
	CellShipTopDestroyed,
	CellShipBottomDestroyed,
	CellShipLeftDestroyed,
	CellShipRightDestroyed,
	CellShipHorizontalDestroyed,
	CellShipVerticalDestroyed,
} CellState;

typedef struct Size {
	uint16_t x;
	uint16_t y;
} Size;

typedef struct Vec2 {
	int x;
	int y;
} Vec2;

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

typedef struct GameStatus {
	CellState self_status[ROW][COLUMN];
	CellState enemy_status[ROW][COLUMN];
	Vec2 preparing_cursor;
	Vec2 cursor;
	bool self_preparing;
	bool enemy_preparing;
	bool my_turn;
	int self_hp;
	int enemy_hp;
	int self_max_hp;
	int enemy_max_hp;
} GameStatus;

typedef struct Status {
	bool running;
	Page page;
	int sock_fd;
	GameStatus game;
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

bool streq(const char* a, const char* b) {
	return strcmp(a, b) == 0;
}

bool string_has_prefix(const char* str, const char* prefix) {
	return strncmp(str, prefix, strlen(prefix)) == 0;
}

bool cell_is_ship_not_destroyed(CellState target) {
	return target >= CellShipTop && target <= CellShipVertical;
}

bool cell_is_ship_destroyed(CellState target) {
	return target >= CellShipTopDestroyed && target <= CellShipVerticalDestroyed;
}

Buffer grid(CellState status[ROW][COLUMN], Vec2 cursor) {
	int width = 7;
	int height = 3;
	int full_width = width + 3;
	int full_height = height + 1;

	uint16_t x = full_width * COLUMN + 1;
	uint16_t y = full_height * ROW + 1;

	char** arr = malloc(y * sizeof(char*));
	for (int i = 0; i < y; i++) {
		if (i % full_height == 0) {
			arr[i] = malloc((x + 1) * sizeof(char));
			memset(arr[i], '-', x);
			for (int j = 0; j < x / full_width + 1; j++) {
				arr[i][j * full_width] = '+';
			}
			arr[i][x] = '\0';
		} else {
			arr[i] = strdup("");
			for (int j = 0; j < x / full_width; j++) {
				char* new;
				char* color_start = "";
				char* color_end = "";
				if (cursor.x == j && cursor.y == i / full_height) {
					color_start = "\e[100m";
					color_end = "\e[0m";
				}
				int x_index = j;
				int type = status[i / full_height][x_index];
				const char* content = cells[type][i % full_height - 1];
				asprintf(&new, "%s| %s%s%s ", arr[i], color_start, content, color_end);
				free(arr[i]);
				arr[i] = new;
			}
			size_t len = strlen(arr[i]);
			arr[i] = realloc(arr[i], len + 2);
			arr[i][len] = '|';
			arr[i][len + 1] = '\0';
		}
	}

	return (Buffer){
		.ptr = arr,
		.size = { .x = x, .y = y, },
	};
}

Buffer game_ui(GameStatus* status) {
	Vec2 left_cursor = { .x = -1, .y = -1, };
	Vec2 right_cursor = { .x = -1, .y = -1, };
	if (status->self_preparing) {
		right_cursor = status->cursor;
	} else {
		left_cursor = status->cursor;
	}
	Buffer left = grid(status->enemy_status, left_cursor);
	Buffer right = grid(status->self_status, right_cursor);
	assert(left.size.y == right.size.y);

	uint16_t top_bar_y = 3;

	uint16_t y = left.size.y + top_bar_y;

	char* gap = "  ~~  ";
	char** arr = malloc(y * sizeof(char*));

	for (int i = 0; i < left.size.y; i++) {
		asprintf(&arr[i + top_bar_y], "%s%s%s", left.ptr[i], gap, right.ptr[i]);
	}
	free_buffer(&left);
	free_buffer(&right);

	uint16_t x = left.size.x + strlen(gap) + right.size.x;

	asprintf(&arr[0], "%*s", x, "");
	asprintf(&arr[2], "%*s", x, "");

	assert(x % 2 == 0);
	if (status->self_preparing || status->enemy_preparing) {
		int bar_len = x / 2 - 3 - 7;
		char* left_hp_bar = malloc(bar_len + 1);
		memset(left_hp_bar, '\\', bar_len);
		left_hp_bar[bar_len] = '\0';

		char* right_hp_bar = malloc(bar_len + 1);
		memset(right_hp_bar, '/', bar_len);
		right_hp_bar[bar_len] = '\0';

		char* left_preparing = "   ";
		if (status->enemy_preparing) {
			left_preparing = "xxx";
		}
		char* right_preparing = "   ";
		if (status->self_preparing) {
			right_preparing = "xxx";
		}

		asprintf(&arr[1], "?? %s  %s <> %s  %s ??", left_hp_bar, left_preparing, right_preparing, right_hp_bar); 

		free(left_hp_bar);
		free(right_hp_bar);
	} else {
		int bar_len = x / 2 - 3 - 7;
		char* left_hp_bar = malloc(bar_len + 1);
		memset(left_hp_bar, '\\', bar_len);
		memset(left_hp_bar, '.', bar_len - (int)(bar_len * ((double)status->enemy_hp / status->enemy_max_hp)));
		left_hp_bar[bar_len] = '\0';

		char* right_hp_bar = malloc(bar_len + 1);
		memset(right_hp_bar, '.', bar_len);
		memset(right_hp_bar, '/', (int)(bar_len * ((double)status->self_hp / status->self_max_hp)));
		right_hp_bar[bar_len] = '\0';

		char* turn;
		if (status->my_turn) {
			turn = "      <> >>>  ";
		} else {
			turn = "  <<< <>      ";
		}

		asprintf(&arr[1], "%-3d%s%s%s%3d", status->enemy_hp, left_hp_bar, turn, right_hp_bar, status->self_hp); 

		free(left_hp_bar);
		free(right_hp_bar);
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
	char* padding;
	asprintf(&padding, "%*s", (int)(x - strlen(options[0])) / 2, "");
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
	free(padding);

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
		"- Join  ",
		"- Cancel",
	};
	char* padding;
	asprintf(&padding, "%*s", (int)(x - strlen(options[0])) / 2, "");
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
	free(padding);

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

	char* padding;
	asprintf(&padding, "%*s", (int)(strlen(arr[0]) - strlen(old)) / 2, "");
	char* right = "";
	if ((strlen(arr[0]) - strlen(old)) % 2 != 0) {
		right = " ";
	}
	asprintf(&arr[1], "%s%s%s%s", padding, old, padding, right); 
	free(old);
	free(padding);

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

	char* padding;
	asprintf(&padding, "%*s", (int)(strlen(arr[0]) - strlen(old)) / 2, "");
	char* right = "";
	if ((strlen(arr[0]) - strlen(old)) % 2 != 0) {
		right = " ";
	}
	asprintf(&arr[1], "%s%s%s%s", padding, old, padding, right); 
	free(old);
	free(padding);

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

	char* other = "|         |         |";
	char* padding;
	asprintf(&padding, "%*s", (39 - options.size.x) / 2, "");
	char* right = "";
	if ((39 - options.size.x) % 2 != 0) {
		right = " ";
	}
	for (int i = 0; i < 3; i++) {
		char* buffer;
	asprintf(&buffer, "%s%s%s%s%s%s", other, padding, options.ptr[i], padding, right, other);

		arr[sizeof(top_part) / sizeof(top_part[0]) + i] = buffer;
	}
	free(padding);

	arr[y - 1] = strdup("+---------+---------+---------+---------+---------+---------+---------+---------+");
	
	free_buffer(&options);

	return (Buffer){
		.ptr = arr,
		.size = { .x = x, .y = y, },
	};
}

Buffer error_screen(void) {
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
	free(x_padding);
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

void leave_alter_screen(void) {
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
		case 'j': case 's':
			if (*selection < 0 || *selection > GreetingExit) {
				*selection = GreetingCreate;
			} else if (*selection < GreetingExit) {
				*selection += 1;
			}
			break;
		case 'k': case 'w':
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
		case 'j': case 's':
			if (*selection < 0 || *selection > CreatingExit) {
				*selection = CreatingInput;
			} else if (*selection < CreatingExit) {
				*selection += 1;
			}
			break;
		case 'k': case 'w':
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
		case 'j': case 's':
			if (*selection < 0 || *selection > JoinExit) {
				*selection = JoinInput;
			} else if (*selection < JoinExit) {
				*selection += 1;
			}
			break;
		case 'k': case 'w':
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

void handle_preparing_key_event(Status* status, int key) {
	switch (key) {
		case 'j': case 's':
			if (status->game.cursor.y < ROW - 1) {
				status->game.cursor.y += 1;
			}
			break;
		case 'k': case 'w':
			if (status->game.cursor.y > 0) {
				status->game.cursor.y -= 1;
			}
			break;
		case 'h': case 'a':
			if (status->game.cursor.x > 0) {
				status->game.cursor.x -= 1;
			}
			break;
		case 'l': case 'd':
			if (status->game.cursor.x < COLUMN - 1) {
				status->game.cursor.x += 1;
			}
			break;
		case '\n':
			if (status->game.preparing_cursor.x == -1 && status->game.preparing_cursor.y == -1) {
				if (cell_is_ship_not_destroyed(status->game.self_status[status->game.cursor.y][status->game.cursor.x])) {
					// TODO: remove this ship
				} else {
					status->game.preparing_cursor = status->game.cursor;
				}
			} else if (status->game.preparing_cursor.x == status->game.cursor.x && status->game.preparing_cursor.y != status->game.cursor.y) {
				int x = status->game.cursor.x;
				int y_min;
				int y_max;
				if (status->game.preparing_cursor.y > status->game.cursor.y) {
					y_max = status->game.preparing_cursor.y;
					y_min = status->game.cursor.y;
				} else if (status->game.preparing_cursor.y < status->game.cursor.y) {
					y_max = status->game.cursor.y;
					y_min = status->game.preparing_cursor.y;
				}
				for (int y = y_min; y <= y_max; y++) {
					if (cell_is_ship_not_destroyed(status->game.self_status[y][x])) {
						return;
					}
				}
				for (int y = y_min; y <= y_max; y++) {
					if (y == y_min) {
						status->game.self_status[y][x] = CellShipTop;
					} else if (y == y_max) {
						status->game.self_status[y][x] = CellShipBottom;
					} else {
						status->game.self_status[y][x] = CellShipVertical;
					}
				}
				status->game.preparing_cursor = (Vec2){ .x = -1, .y = -1, };
			} else if (status->game.preparing_cursor.x != status->game.cursor.x && status->game.preparing_cursor.y == status->game.cursor.y) {
				int x_min;
				int x_max;
				int y = status->game.cursor.y;
				if (status->game.preparing_cursor.x > status->game.cursor.x) {
					x_max = status->game.preparing_cursor.x;
					x_min = status->game.cursor.x;
				} else if (status->game.preparing_cursor.x < status->game.cursor.x) {
					x_max = status->game.cursor.x;
					x_min = status->game.preparing_cursor.x;
				}
				for (int x = x_min; x <= x_max; x++) {
					if (cell_is_ship_not_destroyed(status->game.self_status[y][x])) {
						return;
					}
				}
				for (int x = x_min; x <= x_max; x++) {
					if (x == x_min) {
						status->game.self_status[y][x] = CellShipLeft;
					} else if (x == x_max) {
						status->game.self_status[y][x] = CellShipRight;
					} else {
						status->game.self_status[y][x] = CellShipHorizontal;
					}
				}
				status->game.preparing_cursor = (Vec2){ .x = -1, .y = -1, };
			}
			break;
		case '\e':
			status->game.preparing_cursor = (Vec2){ .x = -1, .y = -1, };
			break;
		case ' ':
			status->game.self_max_hp = 0;
			for (int y = 0; y < ROW; y++) {
				for (int x = 0; x < COLUMN; x++) {
					if (cell_is_ship_not_destroyed(status->game.self_status[y][x])) {
						status->game.self_max_hp += 1;
					}
				}
			}
			if (status->game.self_max_hp == 0) {
				break;
			}
			status->game.cursor = (Vec2){ .x = COLUMN - 1, .y = 0, };
			status->game.preparing_cursor = (Vec2){ .x = -1, .y = -1, };
			status->game.self_preparing = false;
			status->game.self_hp = status->game.self_max_hp;
			char* buf;
			asprintf(&buf, "READY %d\n", status->game.self_max_hp);
			write(status->sock_fd, buf, strlen(buf));
			free(buf);
			break;
	}
}

void handle_game_key_event(Status* status, int key) {
	switch (key) {
		case 'j': case 's':
			if (status->game.cursor.y < ROW - 1) {
				status->game.cursor.y += 1;
			}
			break;
		case 'k': case 'w':
			if (status->game.cursor.y > 0) {
				status->game.cursor.y -= 1;
			}
			break;
		case 'h': case 'a':
			if (status->game.cursor.x > 0) {
				status->game.cursor.x -= 1;
			}
			break;
		case 'l': case 'd':
			if (status->game.cursor.x < COLUMN - 1) {
				status->game.cursor.x += 1;
			}
			break;
		case '\n': {
			if (status->game.my_turn) {
				status->game.my_turn = false;
				char* buf;
				asprintf(&buf, "FIRE %d,%d\n", status->game.cursor.x, status->game.cursor.y);
				write(status->sock_fd, buf, strlen(buf));
				free(buf);
			}
			break;
		}
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
				if (status->game.self_preparing || status->game.enemy_preparing) {
					handle_preparing_key_event(status, key);
				} else {
					handle_game_key_event(status, key);
				}
				break;
			case Error:
				break;
		}
	}
}

char* handle_fire(CellState self_cells[ROW][COLUMN], Vec2 position, int* self_hp) {
	CellState target = self_cells[position.y][position.x];
	if (cell_is_ship_not_destroyed(target)) {
		*self_hp -= 1;
		self_cells[position.y][position.x] = target + (CellShipTopDestroyed - CellShipTop);
	} else if (target == CellEmpty) {
		self_cells[position.y][position.x] = CellMiss;
		char* message;
		asprintf(&message, "MISS %d,%d\n", position.x, position.y);
		return message;
	} else {
		return strdup("IGNORE\n");
	}

	Vec2 start_position = position;
	char* message;
	bool destroyed = false;
	switch (self_cells[position.y][position.x]) {
		case CellShipTopDestroyed:
			while (true) {
				if (self_cells[position.y][position.x] == CellShipBottomDestroyed) {
					destroyed = true;
					break;
				} else if (cell_is_ship_not_destroyed(self_cells[position.y][position.x])) {
					destroyed = false;
					break;
				} else if (cell_is_ship_destroyed(self_cells[position.y][position.x])) {
					position.y++;
				} else {
					abort();
				}
			}
			if (destroyed) {
				asprintf(&message, "DESTROYED v,%d,%d,%d\n", start_position.x, start_position.y, position.y);
			}
			break;
		case CellShipBottomDestroyed:
			while (true) {
				if (self_cells[position.y][position.x] == CellShipTopDestroyed) {
					destroyed = true;
					break;
				} else if (cell_is_ship_not_destroyed(self_cells[position.y][position.x])) {
					destroyed = false;
					break;
				} else if (cell_is_ship_destroyed(self_cells[position.y][position.x])) {
					position.y--;
				} else {
					abort();
				}
			}
			if (destroyed) {
				asprintf(&message, "DESTROYED v,%d,%d,%d\n", start_position.x, position.y, start_position.y);
			}
			break;
		case CellShipLeftDestroyed:
			while (true) {
				if (self_cells[position.y][position.x] == CellShipRightDestroyed) {
					destroyed = true;
					break;
				} else if (cell_is_ship_not_destroyed(self_cells[position.y][position.x])) {
					destroyed = false;
					break;
				} else if (cell_is_ship_destroyed(self_cells[position.y][position.x])) {
					position.x++;
				} else {
					abort();
				}
			}
			if (destroyed) {
				asprintf(&message, "DESTROYED h,%d,%d,%d\n", start_position.x, position.x, start_position.y);
			}
			break;
		case CellShipRightDestroyed:
			while (true) {
				if (self_cells[position.y][position.x] == CellShipLeftDestroyed) {
					destroyed = true;
					break;
				} else if (cell_is_ship_not_destroyed(self_cells[position.y][position.x])) {
					destroyed = false;
					break;
				} else if (cell_is_ship_destroyed(self_cells[position.y][position.x])) {
					position.x--;
				} else {
					abort();
				}
			}
			if (destroyed) {
				asprintf(&message, "DESTROYED h,%d,%d,%d\n", position.x, start_position.x, start_position.y);
			}
			break;
		case CellShipHorizontalDestroyed: {
			int left = start_position.x;
			int right = start_position.x;
			while (true) {
				if (self_cells[position.y][left] == CellShipLeftDestroyed) {
					destroyed = true;
					break;
				} else if (cell_is_ship_not_destroyed(self_cells[position.y][left])) {
					destroyed = false;
					break;
				} else if (cell_is_ship_destroyed(self_cells[position.y][left])) {
					left--;
				} else {
					abort();
				}
			}
			if (!destroyed) {
				break;
			}
			while (true) {
				if (self_cells[position.y][right] == CellShipRightDestroyed) {
					destroyed = true;
					break;
				} else if (cell_is_ship_not_destroyed(self_cells[position.y][right])) {
					destroyed = false;
					break;
				} else if (cell_is_ship_destroyed(self_cells[position.y][right])) {
					right++;
				} else {
					abort();
				}
			}
			if (destroyed) {
				asprintf(&message, "DESTROYED h,%d,%d,%d\n", left, right, start_position.y);
			}
			break;
		}
		case CellShipVerticalDestroyed: {
			int top = start_position.y;
			int bottom = start_position.y;
			while (true) {
				if (self_cells[top][position.x] == CellShipTopDestroyed) {
					destroyed = true;
					break;
				} else if (cell_is_ship_not_destroyed(self_cells[top][position.x])) {
					destroyed = false;
					break;
				} else if (cell_is_ship_destroyed(self_cells[top][position.x])) {
					top--;
				} else {
					abort();
				}
			}
			if (!destroyed) {
				break;
			}
			while (true) {
				if (self_cells[bottom][position.x] == CellShipBottomDestroyed) {
					destroyed = true;
					break;
				} else if (cell_is_ship_not_destroyed(self_cells[bottom][position.x])) {
					destroyed = false;
					break;
				} else if (cell_is_ship_destroyed(self_cells[bottom][position.x])) {
					bottom++;
				} else {
					abort();
				}
			}
			if (destroyed) {
				asprintf(&message, "DESTROYED v,%d,%d,%d\n", start_position.x, top, bottom);
			}
			break;
		}
		default:
			abort();
	}
	if (!destroyed) {
		asprintf(&message, "HIT %d,%d\n", start_position.x, start_position.y);
	}
	return message;
}

void handle_game_action(Status* status) {
	struct pollfd fds = { .fd = status->sock_fd, .events = POLLIN, };
	while (poll(&fds, 1, 0) > 0) {
		char buf[256] = {0};
		int readed = read(status->sock_fd, buf, sizeof(buf) - 1);
		if (readed == -1) {
			status->page = Error;
			break;
		} else if (readed == 0) {
			status->running = false;
			break;
		}

		char* save;
		char* method = strtok_r(buf, " ", &save);
		char* parms = strtok_r(NULL, "\n", &save);
		assert(method != NULL);
		if (string_has_prefix(buf, "FIRE")) {
			assert(parms != NULL);
			char* save;
			char* x_str = strtok_r(parms, ",", &save);
			char* y_str = strtok_r(NULL, "", &save);

			Vec2 position;
			char* end;
			position.x = COLUMN - strtoul(x_str, &end, 10) - 1;
			assert(end != x_str);
			position.y = strtoul(y_str, &end, 10);
			assert(end != y_str);

			if (!status->game.my_turn) {
				status->game.my_turn = true;
				char* message = handle_fire(status->game.self_status, position, &status->game.self_hp);
				write(status->sock_fd, message, strlen(message));
				free(message);
			}
		} else if (string_has_prefix(buf, "HIT")) {
			assert(parms != NULL);
			char* save;
			char* x_str = strtok_r(parms, ",", &save);
			char* y_str = strtok_r(NULL, "", &save);

			Vec2 position;
			char* end;
			position.x = COLUMN - strtoul(x_str, &end, 10) - 1;
			assert(end != x_str);
			position.y = strtoul(y_str, &end, 10);
			assert(end != y_str);

			status->game.enemy_hp -= 1;
			status->game.enemy_status[position.y][position.x] = CellHit;
		} else if (string_has_prefix(buf, "MISS")) {
			assert(parms != NULL);
			char* save;
			char* x_str = strtok_r(parms, ",", &save);
			char* y_str = strtok_r(NULL, "", &save);

			Vec2 position;
			char* end;
			position.x = COLUMN - strtoul(x_str, &end, 10) - 1;
			assert(end != x_str);
			position.y = strtoul(y_str, &end, 10);
			assert(end != y_str);

			status->game.enemy_status[position.y][position.x] = CellMiss;
		} else if (string_has_prefix(buf, "DESTROYED")) {
			assert(parms != NULL);
			char* save;
			char* direction = strtok_r(parms, ",", &save);
			char* a_str = strtok_r(NULL, ",", &save);
			char* b_str = strtok_r(NULL, ",", &save);
			char* c_str = strtok_r(NULL, "", &save);
			assert(direction!= NULL);
			assert(a_str != NULL);
			assert(b_str != NULL);
			assert(c_str != NULL);
			char* end;
			int a = strtoul(a_str, &end, 10);
			assert(end != a_str);
			int b = strtoul(b_str, &end, 10);
			assert(end != b_str);
			int c = strtoul(c_str, &end, 10);
			assert(end != c_str);
			if (streq(direction, "v")) {
				int x = COLUMN - a - 1;
				int y1 = b;
				int y2 = c;
				for (int y = y1; y <= y2; y++) {
					if (y == y1) {
						status->game.enemy_status[y][x] = CellShipTopDestroyed;
					} else if (y == y2) {
						status->game.enemy_status[y][x] = CellShipBottomDestroyed;
					} else {
						status->game.enemy_status[y][x] = CellShipVerticalDestroyed;
					}

				}
			} else if (streq(direction, "h")){
				int x1 = COLUMN - a - 1;
				int x2 = COLUMN - b - 1;
				int y = c;
				for (int x = x2; x <= x1; x++) {
					if (x == x2) {
						status->game.enemy_status[y][x] = CellShipLeftDestroyed;
					} else if (x == x1) {
						status->game.enemy_status[y][x] = CellShipRightDestroyed;
					} else {
						status->game.enemy_status[y][x] = CellShipHorizontalDestroyed;
					}
				}
			} else {
				assert(streq(direction, "v") || streq(direction, "h"));
			}
			status->game.enemy_hp -= 1;
		} else if (string_has_prefix(buf, "READY")) {
			assert(parms != NULL);
			char* end;
			status->game.enemy_max_hp = strtoul(parms, &end, 10);
			assert(end != parms);
			status->game.enemy_hp = status->game.enemy_max_hp; 
			status->game.enemy_preparing = false;
		} else if (string_has_prefix(buf, "IGNORE")) {
			assert(parms == NULL);
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
				close(status->sock_fd);
				status->sock_fd = accepted_fd;
				socket_fd = accepted_fd;
				status->game.my_turn = true;
			} else if (errno != EAGAIN && errno != EWOULDBLOCK) {
				status->page = Error;
			}
			break;
		}
		case WaitingServer: {
			char buf[32] = {0};
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
				uint64_t tmp_port = strtoul(addr_port, &end, 10);
				assert(end != addr_port);
				assert(tmp_port <= UINT16_MAX);
				port = tmp_port;
			}

			struct sockaddr_in addr;
			addr.sin_family = AF_INET;
			addr.sin_port = htons(port);
			if (streq(addr_ip, "localhost")) {
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
				status->game.my_turn = false;
			} else if (errno != EAGAIN && errno != EALREADY && errno != EINPROGRESS) {
				status->page = Error;
			}
			break;
		}
		case Game:
			handle_game_action(status);
			break;
	}
}

void ctrl_c_handler(int sig) {
	assert(sig == SIGINT);
	leave_alter_screen();
	if (socket_fd != -1) {
		close(socket_fd);
	}
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

	Status status = {
		.running = true,
		.page = Greeting,
		.sock_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0),
		.game = {
			.cursor = { .x = 0, .y = 0, },
			.preparing_cursor  = { .x = -1, .y = -1, },
			.self_status = {0},
			.self_preparing = true,
			.self_hp = 0,
			.self_max_hp = 0,
			.enemy_status = {0},
			.enemy_preparing = true,
			.enemy_hp = 0,
			.enemy_max_hp = 0,
		},
		.greeting = {
			.selection = GreetingNone,
		},
		.creating = {
			.selection = CreatingTyping,
			.port = -1,
		},
		.join = {
			.selection = JoinTyping,
			.connect_addr = {0},
			.cursor = 0,
		},
	};
	socket_fd = status.sock_fd;

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
				print_ui(game_ui(&status.game));
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
