# The engine on this computer, headless (host/host.c): tests and screenshots.
#   make -C host          -> build/host/doom
CC ?= cc
ROOT := ..
OUT := $(ROOT)/build/host
DOOM_SRC := $(wildcard $(ROOT)/firmware/doom/*.c)
FM1_SRC := $(ROOT)/firmware/fm1/lcd_conv.c $(ROOT)/firmware/fm1/fm1_input_map.c
SRC := $(DOOM_SRC) $(FM1_SRC) host.c
OBJ := $(patsubst $(ROOT)/%.c,$(OUT)/obj/%.o,$(filter $(ROOT)/%,$(SRC))) $(OUT)/obj/host/host.o
OPT ?= -O2 -g
CFLAGS := $(OPT) -std=gnu99 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
	-Wno-missing-braces -Wno-pointer-sign -Wno-dangling-else -Wno-parentheses -Wno-tautological-compare \
	-DZ_STATS -DDOOM_FM1=1 -I$(ROOT)/firmware/doom -I$(ROOT)/firmware/fm1 $(EXTRA_CFLAGS)

$(OUT)/doom: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) -lz

$(OUT)/obj/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/obj/host/host.o: host.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OUT)
.PHONY: clean
