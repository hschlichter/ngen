CC = clang
VULKAN_SDK ?= $(HOME)/VulkanSDK/1.4.309.0/macOS
CFLAGS = -std=c17 -O0 -g -Wall -MMD -fPIC `pkg-config --cflags sdl3` -I$(VULKAN_SDK)/include
LDFLAGS = `pkg-config --libs sdl3` -L$(VULKAN_SDK)/lib -lvulkan
INCLUDE = -I.
BUILDDIR = ./build
LDINCLUDE = 

SRCS = $(wildcard *.c **/*.c)
OBJS = $(foreach obj, $(SRCS:.c=.o), $(BUILDDIR)/$(obj))

# Run with DYLD_LIBRARY_PATH=$HOME/VulkanSDK/1.4.309.0/macOS/lib ./build/ngen
.PHONY: all
all: $(OBJS)
	$(CC) $^ -o $(BUILDDIR)/ngen $(CFLAGS) $(INCLUDE) $(LDINCLUDE) $(LDFLAGS)

$(OBJS): $(BUILDDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) -o $@ $< $(INCLUDE)

-include $(OBJS:%.o=%.d)

.PHONY: clean
clean:
	@rm -rf $(BUILDDIR)

print-%  : ; @echo $* = $($*)
