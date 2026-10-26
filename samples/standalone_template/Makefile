# Makefile variant of the template (for projects not using CMake).
# Needs VkVita installed into $VITASDK (cmake --install).
TITLE_ID = MYGAME001
TARGET   = my_vulkan_game
OBJS     = main.o

PREFIX   = arm-vita-eabi
CC       = $(PREFIX)-gcc
PKGCFG   = PKG_CONFIG_PATH=$(VITASDK)/arm-vita-eabi/lib/pkgconfig pkg-config
CFLAGS   = -O2 -Wall $(shell $(PKGCFG) --cflags vkvita)
LIBS     = $(shell $(PKGCFG) --libs vkvita) -lSceCtrl_stub

all: $(TARGET).vpk

%.vpk: eboot.bin
	vita-mksfoex -s TITLE_ID=$(TITLE_ID) "$(TARGET)" param.sfo
	vita-pack-vpk -s param.sfo -b eboot.bin $@

eboot.bin: $(TARGET).velf
	vita-make-fself -s $< $@

%.velf: %.elf
	vita-elf-create $< $@

$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $^ $(LIBS) -o $@

clean:
	rm -f $(TARGET).velf $(TARGET).elf $(TARGET).vpk eboot.bin param.sfo $(OBJS)
