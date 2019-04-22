CC = gcc
CFLAGS = -Wall -Werror -g -O0
LDFLAGS = -libverbs
APPS = rdma_send


all: $(APPS)
rdma_send:rdma_send.o
	$(CC) rdma_send.o -o rdma_send $(LDFLAGS)
*.o:*.c
	${CC} -c $(CFLAGS) $< -o $@
clean:
	rm -f *.o $(APPS)

