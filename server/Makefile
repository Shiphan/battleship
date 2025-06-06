server: server.c
	cc -o server server.c
server-static: server.c
	cc -static -o server server.c
run: server
	./server
debug: server.c
	cc -g -O0 -o server server.c
