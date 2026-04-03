CXX = clang++
CXXFLAGS = -std=c++23 -O0 -g -Wall -MMD -fPIC `pkg-config --cflags sdl3`
LDFLAGS = `pkg-config --libs sdl3` -lvulkan -lm
OUTDIR = ./_out
EXE = ngen

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
	VULKAN_SDK ?= $(HOME)/VulkanSDK/1.4.309.0/macOS
	INCLUDE = -Isrc -Isrc/rhi -Isrc/rhi/vulkan -Isrc/renderer -Isrc/scene -Iexternal/glm -Iexternal/cgltf -Iexternal/stb -I$(VULKAN_SDK)/include
	LDINCLUDE = -L$(VULKAN_SDK)/lib -Wl,-rpath,$(VULKAN_SDK)/lib
	GLSLC = $(VULKAN_SDK)/bin/glslc
else ifeq ($(OS),Windows_NT)
	VULKAN_SDK ?= $(VULKAN_SDK)
	INCLUDE = -Isrc -Isrc/rhi -Isrc/rhi/vulkan -Isrc/renderer -Isrc/scene -Iexternal/glm -Iexternal/cgltf -Iexternal/stb -I$(VULKAN_SDK)/Include
	LDINCLUDE = -L$(VULKAN_SDK)/Lib
	GLSLC = $(VULKAN_SDK)/Bin/glslc.exe
	EXE = ngen.exe
else
	INCLUDE = -Isrc -Isrc/rhi -Isrc/rhi/vulkan -Isrc/renderer -Isrc/scene -Iexternal/glm -Iexternal/cgltf -Iexternal/stb
	LDINCLUDE =
	GLSLC = glslc
endif

SHADERS_SRCS = $(wildcard *.vert **/*.vert) $(wildcard *.frag **/*.frag)
SHADERS_SPV = $(foreach spv, $(SHADERS_SRCS:=.spv), $(spv))

SRCS = $(shell find src -name '*.cpp')
OBJS = $(foreach obj, $(SRCS:.cpp=.o), $(OUTDIR)/$(obj))

all: $(OBJS) | $(SHADERS_SPV)
	$(CXX) $^ -o $(OUTDIR)/$(EXE) $(CXXFLAGS) $(INCLUDE) $(LDINCLUDE) $(LDFLAGS)

$(OBJS): $(OUTDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -o $@ $< $(INCLUDE)

%.spv: %
	$(GLSLC) $< -o $@

-include $(OBJS:%.o=%.d)

tidy:
	clang-tidy $(SRCS) -- $(CXXFLAGS) $(INCLUDE)

format:
	clang-format -i $(SRCS) $(shell find src -name '*.h')

clean:
	@rm -rf $(OUTDIR)
	@rm -f shaders/*.spv

print-%  : ; @echo $* = $($*)

.PHONY: all clean format tidy
