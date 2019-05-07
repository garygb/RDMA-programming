CC = gcc
CFLAGS = -Wall -Werror -g -O0
LDFLAGS = -libverbs
APPS = rdma_recv


all: $(APPS)
rdma_recv:rdma_recv.o
	$(CC) rdma_recv.o -o rdma_recv $(LDFLAGS)
*.o:*.c
	${CC} -c $(CFLAGS) $< -o $@
clean:
	rm -f *.o $(APPS)

