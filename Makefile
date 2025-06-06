main: main.c
	cc -O3 -o main main.c
run: main
	./main
debug: main.c
	cc -g -Og -o main main.c
