main: main.c
	gcc -o main main.c
run: main
	./main
debug: main.c
	gcc -g -O0 -o main main.c
