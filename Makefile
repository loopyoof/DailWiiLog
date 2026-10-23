#---------------------------------------------------------------------------------
# Daily Log - build with devkitPPC + libogc (+ GRRLIB, freetype, libpng, libjpeg, zlib, libvorbisidec)
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif
include $(DEVKITPPC)/wii_rules

TARGET   := boot
BUILD    := build
SOURCES  := source
DATA     :=
INCLUDES := source

CFLAGS   = -g -O2 -Wall $(MACHDEP) $(INCLUDE)
CXXFLAGS = $(CFLAGS)
LDFLAGS  = -g $(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS     := -lgrrlib -lfreetype -lpng -ljpeg -lz -lfat -lwiiuse -lbte -lasnd -logc -lvorbisidec -lm
LIBDIRS  := $(PORTLIBS)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT   := $(CURDIR)/$(TARGET)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)
CFILES          := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export LD       := $(CC)
export OFILES   := $(CFILES:.c=.o)
export INCLUDE  := $(foreach dir,$(INCLUDES), -iquote $(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD) -I$(LIBOGC_INC)
export LIBPATHS := -L$(LIBOGC_LIB) $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean install
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).dol

# copy the build into the SD folder layout
install: $(BUILD)
	cp $(OUTPUT).elf $(OUTPUT).dol sdcard/apps/dailylog/

else
DEPENDS := $(OFILES:.o=.d)
$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
-include $(DEPENDS)
endif
