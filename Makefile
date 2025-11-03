CC := g++-9
CUDA_VER ?= 12.8
ARCH ?= 120
CUDA_PATH ?= /usr/local/cuda-$(CUDA_VER)
NVCC ?= $(CUDA_PATH)/bin/nvcc

CCFLAGS := -O3 -I$(CUDA_PATH)/include

# Allow comma-separated ARCH list (e.g., 89,120)
comma := ,
ARCH_LIST := $(subst $(comma), ,$(ARCH))

# One -gencode per arch
GEN_CODES := $(foreach A,$(ARCH_LIST),-gencode=arch=compute_$(A),code=compute_$(A))
NVCCFLAGS := -O3 $(GEN_CODES) -ccbin=/usr/bin/$(CC)

LDFLAGS := -L$(CUDA_PATH)/lib64 -lcudart -pthread -lcurl

CPU_SRC := RCKangaroo.cpp GpuKang.cpp Ec.cpp utils.cpp
GPU_SRC := RCGpuCore.cu

CPP_OBJECTS := $(CPU_SRC:.cpp=.o)
CU_OBJECTS := $(GPU_SRC:.cu=.o)

CUDA_VER_SAFE := $(subst .,-,$(CUDA_VER))
# Name like rckangaroo_cuda-12-4_sm89_sm120 for ARCH=89,120
TARGET := rckangaroo_cuda-$(CUDA_VER_SAFE)_sm$(subst $(comma),_sm,$(ARCH))

all: $(TARGET)

$(TARGET): $(CPP_OBJECTS) $(CU_OBJECTS)
	$(CC) $(CCFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CC) $(CCFLAGS) -c $< -o $@

%.o: %.cu
	$(NVCC) $(NVCCFLAGS) -c $< -o $@

clean:
	rm -f $(CPP_OBJECTS) $(CU_OBJECTS)
