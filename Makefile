CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -g -std=c11 -D_GNU_SOURCE
LDFLAGS = -lpthread
 
OBJS = chatd.o client.o proto.o
 
.PHONY: all clean
 
all: chatd test_client
 
chatd: $(OBJS)
	$(CC) $(CFLAGS) -o chatd $(OBJS) $(LDFLAGS)
 
test_client: test_client.c
	$(CC) $(CFLAGS) -o test_client test_client.c
 
chatd.o:  chatd.c  proto.h client.h
client.o: client.c proto.h client.h
proto.o:  proto.c  proto.h
 
clean:
	rm -f chatd test_client $(OBJS)