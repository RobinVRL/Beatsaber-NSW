.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO must be set)
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET := BeatSaberQuestLoader
BUILD := build
SOURCES := source
INCLUDES := source
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := -g -Wall -Wextra -O2 -ffunction-sections $(ARCH) $(INCLUDE) -D__SWITCH__
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS := -g $(ARCH)
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS := -lGLESv2 -lEGL -lglapi -ldrm_nouveau
BS_MEDIA_FFMPEG ?= 1
ifeq ($(BS_MEDIA_FFMPEG),1)
CFLAGS += -DBS_MEDIA_FFMPEG
LIBS += -lavformat -lavcodec -lswscale -lswresample -lavutil -ldav1d -lbz2
endif
LIBS += -lz -lnx -lm
LIBDIRS := $(PORTLIBS) $(LIBNX)
# NVK's libvulkan.a embeds its own newer nv50_ir compiler. Linked with the
# GLES Mesa driver Unity uses, the duplicated C++ symbols corrupt the GL
# shader compiler. Only link it for explicit Vulkan experiments.
ifeq ($(BS_ENABLE_NVK),1)
LIBS := $(NVK_SDK)/lib/libvulkan.a $(LIBS)
endif
APP_TITLE := Beat Saber Quest Loader
APP_AUTHOR := Homebrew engine study
APP_VERSION := 0.1.0

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
export LD := $(CXX)
export OFILES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export NROFLAGS += --icon=$(DEVKITPRO)/libnx/default_icon.jpg --nacp=$(CURDIR)/$(TARGET).nacp

.PHONY: all clean $(BUILD)
all: $(BUILD)
$(BUILD):
	@mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	@rm -rf $(BUILD) $(TARGET).elf $(TARGET).nro $(TARGET).nacp
else
.PHONY: all
all: $(OUTPUT).nro
ifeq ($(BS_ENABLE_NVK),1)
vulkan_bridge.o: CFLAGS += -DBS_USE_NVK -I$(NVK_SDK)/include -I$(NVK_SDK)/Vulkan-Headers-1.4.305/include
endif
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)
	@echo linking $@
	@$(LD) $(LDFLAGS) $(OFILES) $(LIBPATHS) $(LIBS) -o $@
%.o: %.c
	@echo compiling $<
	@$(CC) $(CFLAGS) -MMD -MP -MF $*.d -c $< -o $@
-include $(DEPSDIR)/*.d
.PHONY: force_vulkan_bridge
vulkan_bridge.o: force_vulkan_bridge
endif
