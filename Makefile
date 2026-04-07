CXX = clang++
CXXFLAGS = -std=c++23 -O0 -g -Wall -MMD -fPIC `pkg-config --cflags sdl3`
LDFLAGS = `pkg-config --libs sdl3` -lvulkan -lm
OUTDIR = ./_out
EXE = ngen

# OpenUSD
USD_DIR = external/openusd_build
USD_INCLUDE = -I$(USD_DIR)/include
USD_LDFLAGS = -L$(USD_DIR)/lib -Wl,-rpath,$(CURDIR)/$(USD_DIR)/lib \
	-lusd_usd -lusd_usdGeom -lusd_usdShade -lusd_usdLux \
	-lusd_sdf -lusd_pcp -lusd_tf -lusd_vt -lusd_gf -lusd_ar \
	-lusd_arch -lusd_plug -lusd_js -lusd_work -lusd_trace -lusd_ts -lusd_pegtl -lusd_kind

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
	VULKAN_SDK ?= $(HOME)/VulkanSDK/1.4.309.0/macOS
	INCLUDE = -Isrc -Isrc/rhi -Isrc/rhi/vulkan -Isrc/renderer -Isrc/scene -Iexternal/glm -Iexternal/cgltf -Iexternal/stb -Iexternal/imgui -Iexternal/imgui/backends -I$(VULKAN_SDK)/include
	LDINCLUDE = -L$(VULKAN_SDK)/lib -Wl,-rpath,$(VULKAN_SDK)/lib
	GLSLC = $(VULKAN_SDK)/bin/glslc
else ifeq ($(OS),Windows_NT)
	VULKAN_SDK ?= $(VULKAN_SDK)
	INCLUDE = -Isrc -Isrc/rhi -Isrc/rhi/vulkan -Isrc/renderer -Isrc/scene -Iexternal/glm -Iexternal/cgltf -Iexternal/stb -Iexternal/imgui -Iexternal/imgui/backends -I$(VULKAN_SDK)/Include
	LDINCLUDE = -L$(VULKAN_SDK)/Lib
	GLSLC = $(VULKAN_SDK)/Bin/glslc.exe
	EXE = ngen.exe
else
	INCLUDE = -Isrc -Isrc/rhi -Isrc/rhi/vulkan -Isrc/renderer -Isrc/scene -Iexternal/glm -Iexternal/cgltf -Iexternal/stb -Iexternal/imgui -Iexternal/imgui/backends
	LDINCLUDE =
	GLSLC = glslc
endif

SHADERS_SRCS = $(wildcard *.vert **/*.vert) $(wildcard *.frag **/*.frag)
SHADERS_SPV = $(foreach spv, $(SHADERS_SRCS:=.spv), $(spv))

IMGUI_DIR = external/imgui
IMGUI_SRCS = $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp \
             $(IMGUI_DIR)/imgui_widgets.cpp $(IMGUI_DIR)/imgui_demo.cpp \
             $(IMGUI_DIR)/backends/imgui_impl_vulkan.cpp $(IMGUI_DIR)/backends/imgui_impl_sdl3.cpp

SRCS = $(shell find src -name '*.cpp') $(IMGUI_SRCS)
OBJS = $(foreach obj, $(SRCS:.cpp=.o), $(OUTDIR)/$(obj))

USD_SRCS = $(wildcard src/scene/usd*.cpp)
USD_OBJS = $(foreach obj, $(USD_SRCS:.cpp=.o), $(OUTDIR)/$(obj))
NON_USD_OBJS = $(filter-out $(USD_OBJS), $(OBJS))

all: $(NON_USD_OBJS) $(USD_OBJS) | $(SHADERS_SPV)
	$(CXX) $^ -o $(OUTDIR)/$(EXE) $(CXXFLAGS) $(INCLUDE) $(LDINCLUDE) $(LDFLAGS) $(USD_LDFLAGS)

# USD source files: compile with C++20 and USD includes
$(USD_OBJS): $(OUTDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c -std=c++20 -O0 -g -Wall -MMD -fPIC `pkg-config --cflags sdl3` -Wno-deprecated-declarations -o $@ $< $(INCLUDE) $(USD_INCLUDE)

$(NON_USD_OBJS): $(OUTDIR)/%.o: %.cpp
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
