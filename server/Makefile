CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Werror -g
LDLIBS ?= -lpthread -lrt

all: aesdsocket

aesdsocket: aesdsocket.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o aesdsocket aesdsocket.c $(LDLIBS)

clean:
	rm -f aesdsocket *.o
