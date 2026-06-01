.PHONY: all clean install

EXTENSION = docinspect
MODULE_big = pg_$(EXTENSION)

DATA = sql/docinspect--0.1-0.sql

# Try libbson-static-1.0 first, fall back to libbson-1.0
LIBBSON_CFLAGS := $(shell pkg-config --cflags libbson-static-1.0 2>/dev/null || \
                          pkg-config --cflags libbson-1.0 2>/dev/null || echo "")
LIBBSON_LIBS := $(shell pkg-config --static --libs libbson-static-1.0 2>/dev/null || \
                        pkg-config --libs libbson-1.0 2>/dev/null || echo "")

PG_CPPFLAGS = $(LIBBSON_CFLAGS) -Iinclude
PG_LDFLAGS = $(LIBBSON_LIBS)

# -Wno-declaration-after-statement: libbson headers use C99 mixed declarations
# This must override PGXS's CFLAGS which adds -Wdeclaration-after-statement
PG_CFLAGS = -std=gnu99 -Wall -Wextra -Wno-unused-parameter \
            -Wno-declaration-after-statement $(LIBBSON_CFLAGS)

SOURCES = $(wildcard src/*.c)
OBJS = $(patsubst %.c,%.o,$(SOURCES))

DEBUG ?= no
ifeq ($(DEBUG),yes)
  PG_CPPFLAGS += -ggdb -O0 -g
  PG_CFLAGS += -ggdb -O0 -g
  override CFLAGS += -O0
endif

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Override after PGXS include to suppress the warning from libbson headers
override CFLAGS += -Wno-declaration-after-statement
