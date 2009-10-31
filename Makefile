# Copyright (c) 2008 Andrés J. Díaz <ajdiaz@connectical.com>
# This is a specific Makefile for C project. Distributed with
# NO WARRANTY as a part of larger project.

# There are the main configuration variables to make the project,
# any other variables in .mk files must be imported automatically.
# The USE and the ENABLED variables setup a lot of different values
# in the application source.
CC       = gcc
INCLUDE  = include
CFLAGS   = -fPIC -O2 -ggdb -Wall -I$(INCLUDE)
SRCDIR   = src
BINS     = libnss_map.so
RM       = rm -f
LD       = ld
LDFLAGS  =
ARFLAGS  = rcs
USE      =
ENABLE   = # ASSERT

-include config.mk

# Dependencies for each binary is setting using a variable called
# as output binary and contain a list of dependencies, for example
# for binary out the dependencies are one and two::
#
#   out := one two

libnss_map.so := libnss_map.o

# --- Do not touch below this line ---

SRCS=$(shell find $(SRCDIR) -name '*.c')
OBJS=$(SRCS:.c=.o)
ALLS=$(OBJS) $(addprefix $(SRCDIR)/,$(BINS))

$(foreach name_,$(wildcard *.mk),$(eval include $(name_)))
$(foreach name_,$(USE),$(eval CFLAGS=$(CFLAGS) -DUSE_$(name_)))
$(foreach name_,$(ENABLE),$(eval CFLAGS=$(CFLAGS) -DENABLE_$(name_)))

all: $(addprefix $(SRCDIR)/,$(BINS))

define auto-dep
ifneq ($(suffix $1),.a)
	ifeq ($(suffix $1),.so)
$$(SRCDIR)/$1: LDFLAGS += -shared
	else
$$(SRCDIR)/$1: CFLAGS += $(LDFLAGS)
	endif
else
$$(SRCDIR)/$1: CFLAGS := $(CFLAGS)
endif
$$(SRCDIR)/$1: $(addprefix $(SRCDIR)/,$($1))
ifneq ($(suffix $1),.a)
ifeq ($(suffix $1),.so)
	@echo  "[0;1m$$(CC)[0;0m  $$@"
	@$(CC) -shared -Wl,-soname,$1 $(LDFLAGS) -o $$@ $$(addprefix $$(SRCDIR)/,$$($1))
else
	@echo  "[0;1m$$(CC)[0;0m  $$@"
	@$(CC) $(LDFLAGS) -o $$@ $$(addprefix $$(SRCDIR)/,$$($1))
endif
else
	@echo  "[0;1m$$(AR)[0;0m  $$@"
	@$(AR) $(ARFLAGS)  $$@ $$(addprefix $$(SRCDIR)/,$$($1))
endif
endef

$(foreach name_,$(BINS),$(eval $(call auto-dep,$(name_))))

.c.o:
	@echo  "[0;1m$(CC)[0;0m  $@"
	@$(CC) $(CFLAGS) -c -o $@ $<

clean:
	@echo  "[0;1m$(RM)[0;0m  $(ALLS)"
	@$(RM) $(ALLS)

tags:
	@which ctags 2>&1 >/dev/null && \
		ctags --recurse=yes $(SRCS) || \
		echo "Sorry :( Tags not available." >&2

