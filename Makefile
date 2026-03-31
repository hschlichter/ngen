CC = clang
CFLAGS = -std=c17 -O0 -g -Wall -MMD -fPIC `pkg-config --cflags sdl3`
LDFLAGS = `pkg-config --libs sdl3` -lvulkan
OUTDIR = ./_out
EXE = ngen

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
	VULKAN_SDK ?= $(HOME)/VulkanSDK/1.4.309.0/macOS
	INCLUDE = -I. -I$(VULKAN_SDK)/include
	LDINCLUDE = -L$(VULKAN_SDK)/lib -Wl,-rpath,$(VULKAN_SDK)/lib
	GLSLC = $(VULKAN_SDK)/bin/glslc
else ifeq ($(OS),Windows_NT)
	VULKAN_SDK ?= $(VULKAN_SDK)
	INCLUDE = -I. -I$(VULKAN_SDK)/Include
	LDINCLUDE = -L$(VULKAN_SDK)/Lib
	GLSLC = $(VULKAN_SDK)/Bin/glslc.exe
	EXE = ngen.exe
else
	INCLUDE = -I.
	LDINCLUDE =
	GLSLC = glslc
endif

SHADERS_SRCS = $(wildcard *.vert **/*.vert) $(wildcard *.frag **/*.frag)
SHADERS_SPV = $(foreach spv, $(SHADERS_SRCS:=.spv), $(spv))

SRCS = $(wildcard *.c **/*.c)
OBJS = $(foreach obj, $(SRCS:.c=.o), $(OUTDIR)/$(obj))

all: $(OBJS) | $(SHADERS_SPV)
	$(CC) $^ -o $(OUTDIR)/$(EXE) $(CFLAGS) $(INCLUDE) $(LDINCLUDE) $(LDFLAGS)

$(OBJS): $(OUTDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) -o $@ $< $(INCLUDE)

%.spv: %
	$(GLSLC) $< -o $@

-include $(OBJS:%.o=%.d)

clean:
	@rm -rf $(OUTDIR)
	@rm -f shaders/*.spv

print-%  : ; @echo $* = $($*)

.PHONY: all clean
