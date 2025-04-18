#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <signal.h>

#define COLUMN 6
#define ROW 4

typedef struct Size {
	uint16_t x;
	uint16_t y;
} Size;

typedef struct Buffer {
	char** ptr;
	Size size;
} Buffer;

Buffer main_ui(void) {
	uint16_t x = 10 * COLUMN + 1;
	uint16_t y = 4 * ROW + 1;

	char** arr = malloc((y + 1) * sizeof(char*));
	for (int i = 0; i < y; i++) {
		arr[i] = malloc((x + 1) * sizeof(char));
		if (i % 4 == 0) {
			memset(arr[i], '-', x);
			for (int j = 0; j < x / 10 + 1; j++) {
				arr[i][j * 10] = '+';
			}
		} else {
			memset(arr[i], ' ', x);
			for (int j = 0; j < x / 10 + 1; j++) {
				arr[i][j * 10] = '|';
				assert(j*10 < x);
			}
		}
		arr[i][x] = '\0';
	}

	return (Buffer){
		.ptr = arr,
		.size = { .x = x, .y = y, },
	};
}

char* ui_wrapper(Buffer buf, Size size) {
	if (buf.size.x > size.x || buf.size.y > size.y) {
		for (int i = 0; i < buf.size.y; i++) {
			free(buf.ptr[i]);
		}
		free(buf.ptr);
		buf.ptr = NULL;

		char* result;
		int err = asprintf(
			&result,
			"The terminal is too small (%d x %d), and it should at least be %d x %d.\n",
			size.x, size.y, buf.size.x, buf.size.y
		);
		assert(err != -1);
		return result;
	}

	// TODO: wrap the ui
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
}

void leave_alter_screen(void) {
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
	bool running = true;
	while (running) {
		print_ui(main_ui());
		usleep(1000*1000/60);
	}
	leave_alter_screen();

	return 0;
}
