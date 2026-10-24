CC ?= $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Werror -g
LDFLAGS = -lpthread -lrt

all: aesdsocket

aesdsocket: aesdsocket.c
	$(CC) $(CFLAGS) -o aesdsocket aesdsocket.c $(LDFLAGS)

clean:
	rm -f aesdsocket *.o
