CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Werror -g
USE_AESD_CHAR_DEVICE ?= 1
DEFS = -DUSE_AESD_CHAR_DEVICE=$(USE_AESD_CHAR_DEVICE)
LDLIBS ?= -lpthread -lrt

all: aesdsocket

aesdsocket: aesdsocket.c
	$(CC) $(CFLAGS) $(DEFS) $(LDFLAGS) -o aesdsocket aesdsocket.c $(LDLIBS)

clean:
	rm -f aesdsocket *.o
