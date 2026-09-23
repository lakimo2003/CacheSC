######## Variables ########

CURR_PATH := $(realpath $(dir $(realpath $(firstword $(MAKEFILE_LIST)))))

ifeq ($(INST_PATH),)
    INST_PATH = $(realpath $(dir $(CURR_PATH)))
endif

ifeq ($(OPENSSL_INCL),)
    OPENSSL_INCL = /usr/local/include
endif

ifeq ($(OPENSSL_LIB),)
    OPENSSL_LIB = /usr/local/lib
endif

ifeq ($(ARGON_INCL),)
    ARGON_INCL = /usr/include
endif

ifeq ($(ARGON_LIB),)
    ARGON_LIB = /usr/lib/x86_64-linux-gnu
endif

CFLAGS  += -std=gnu99 -D_ISOC11_SOURCE=1 -O1 -Winline

## CacheSC
CFLAGS  += -I$(INST_PATH)/include
LDFLAGS += -L$(INST_PATH)/lib
LDLIBS  += -lcachesc

## OpenSSL
CFLAGS  += -I$(OPENSSL_INCL)
LDFLAGS += -L$(OPENSSL_LIB)
LDLIBS  += -lcrypto

## Argon2
CFLAGS  += -I$(ARGON_INCL)
LDFLAGS += -L$(ARGON_LIB) -static
LDLIBS  += -largon2 -pthread

CC	:= gcc
OUT := single-eviction openssl-aes-cbc argon2d-attacker argon2d-victim
L2_OUT := l2-attacker l2-victim
L2_SOURCES := ../l2/src/l2_evset.c ../l2/src/l2_pagemap.c ../l2/src/util.c
L2_HEADERS := $(wildcard ../l2/include/*.h)
L2_CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Werror

ifneq ($(NORMALIZE),)
    CFLAGS += -DNORMALIZE=$(NORMALIZE)
endif

######## Targets ########

all: $(OUT) $(L2_OUT)

.PHONY: l2
l2: $(L2_OUT)

rebuild: clean all

clean:
	rm -rf *.o $(OUT) $(L2_OUT)

$(OUT):
	$(CC) $(CFLAGS) -o $@ $(@:=.c) $(LDFLAGS) $(LDLIBS)

$(L2_OUT): %: %.c $(L2_SOURCES) $(L2_HEADERS)
	$(CC) $(L2_CFLAGS) -I../l2/include $< $(L2_SOURCES) -pthread -o $@
