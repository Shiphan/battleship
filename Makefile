main: main.c
	cc -o main main.c
run: main
	./main
debug: main.c
	cc -g -O0 -o main main.c
