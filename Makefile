CC = clang
VULKAN_SDK ?= $(HOME)/VulkanSDK/1.4.309.0/macOS
CFLAGS = -std=c17 -O0 -g -Wall -MMD -fPIC `pkg-config --cflags sdl3`
LDFLAGS = `pkg-config --libs sdl3` -lvulkan
INCLUDE = -I. -I$(VULKAN_SDK)/include
OUTDIR = ./out
LDINCLUDE = -L$(VULKAN_SDK)/lib -Wl,-rpath,$(VULKAN_SDK)/lib

SRCS = $(wildcard *.c **/*.c)
OBJS = $(foreach obj, $(SRCS:.c=.o), $(OUTDIR)/$(obj))

# Run with DYLD_LIBRARY_PATH=$HOME/VulkanSDK/1.4.309.0/macOS/lib ./build/ngen
# With rpath, it's not needed
.PHONY: all
all: $(OBJS)
	$(CC) $^ -o $(OUTDIR)/ngen $(CFLAGS) $(INCLUDE) $(LDINCLUDE) $(LDFLAGS)

$(OBJS): $(OUTDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) -o $@ $< $(INCLUDE)

-include $(OBJS:%.o=%.d)

.PHONY: clean
clean:
	@rm -rf $(OUTDIR)

print-%  : ; @echo $* = $($*)
