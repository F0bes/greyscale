EE_BIN = greyscale.elf
EE_OBJS = greyscale.o texture.o
EE_LIBS = -lkernel -lgraph -ldma -ldraw

EE_CFLAGS = -I$(shell pwd) -Werror

all: $(EE_BIN)

texture.s:
	bin2s 16bit.data texture.s texture

clean:
	rm -f $(EE_OBJS) $(EE_BIN) texture.s

run: $(EE_BIN)
	ps2client execee host:$(EE_BIN)

wsl: $(EE_BIN)
	$(PCSX2) -elf "$(shell wslpath -w $(shell pwd))/$(EE_BIN)"

emu: $(EE_BIN)
	$(PCSX2) -elf "$(shell pwd)/$(EE_BIN)"

reset:
	ps2client reset
	ps2client netdump

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
