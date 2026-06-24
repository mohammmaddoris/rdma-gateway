APP = rdma_gateway
CC = gcc
PKGCONF = pkg-config

CFLAGS += -O3 -g -Wall -Wextra -I./include
CFLAGS += -march=native -mavx2 -msse4.2
CFLAGS += $(shell $(PKGCONF) --cflags libdpdk)
LDFLAGS += $(shell $(PKGCONF) --libs libdpdk)
LDFLAGS += -lrt -lpthread -lm

SRCS = src/main.c \
       src/log.c \
       src/crc32c.c \
       src/qp_ctx.c \
       src/arq.c \
       src/jitter_buffer.c \
       src/congestion.c \
       src/wan_tunnel.c \
       src/stats.c \
       src/processor.c \
       src/peer_manager.c

OBJS = $(SRCS:.c=.o)

all: $(APP)

$(APP): $(OBJS)
	$(CC) $(OBJS) -o $(APP) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f src/*.o $(APP)

.PHONY: all clean
