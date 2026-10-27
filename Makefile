# Native Windows build (TDM-GCC / MinGW-w64 + SDL2)
CC      = gcc
SDL     = thirdparty/sdl2
MIK     = thirdparty/libmikmod
CFLAGS  = -O2 -g -fno-strict-aliasing -DSDL_MAIN_HANDLED -I$(SDL)/include/SDL2 -Isrc -Igen -I$(MIK)/include -Ithirdparty
GENFLAGS= -O1 -fno-strict-aliasing -w -Isrc -Igen
MIKFLAGS= -O2 -w -DHAVE_CONFIG_H -DMIKMOD_BUILD -DMIKMOD_STATIC -I$(MIK) -I$(MIK)/include
LDFLAGS = -L$(SDL)/lib -lSDL2 -lm
BUILD   = build

MIK_SRC = $(MIK)/mmio/mmalloc.c $(MIK)/mmio/mmerror.c $(MIK)/mmio/mmio.c \
          $(MIK)/playercode/mdriver.c $(MIK)/playercode/mloader.c $(MIK)/playercode/mlutil.c \
          $(MIK)/playercode/mplayer.c $(MIK)/playercode/munitrk.c $(MIK)/playercode/mwav.c \
          $(MIK)/playercode/npertab.c $(MIK)/playercode/sloader.c $(MIK)/playercode/virtch.c \
          $(MIK)/playercode/virtch2.c $(MIK)/playercode/virtch_common.c $(MIK)/playercode/mdulaw.c \
          $(MIK)/loaders/load_uni.c $(MIK)/drivers/drv_nos.c $(MIK)/posix/strcasecmp.c \
          $(MIK)/depackers/mmcmp.c $(MIK)/depackers/pp20.c $(MIK)/depackers/s404.c $(MIK)/depackers/xpk.c
GEN_SRC = $(wildcard gen/rec_*.c)
SRC     = $(wildcard src/*.c)
OBJ     = $(patsubst gen/%.c,$(BUILD)/gen/%.o,$(GEN_SRC)) $(patsubst src/%.c,$(BUILD)/src/%.o,$(SRC)) \
          $(patsubst $(MIK)/%.c,$(BUILD)/mik/%.o,$(MIK_SRC))

# stop make from trying to regenerate makefiles via built-in rules
.DEFAULT_GOAL := all
Makefile Makefile.web: ;

all: $(BUILD)/uplink_port.exe

$(BUILD)/uplink_port.exe: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)
	cp $(SDL)/bin/SDL2.dll $(BUILD)/

$(BUILD)/gen/%.o: gen/%.c src/recomp.h gen/rec_funcs.h
	@mkdir -p $(BUILD)/gen
	$(CC) $(GENFLAGS) -c $< -o $@

$(BUILD)/src/%.o: src/%.c src/recomp.h src/host.h src/gl.h src/port_config.h
	@mkdir -p $(BUILD)/src
	$(CC) $(CFLAGS) -Wall -Wno-unused-function -Wno-misleading-indentation -c $< -o $@

$(BUILD)/mik/%.o: $(MIK)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(MIKFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)
