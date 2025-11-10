FROM nvidia/cuda:12.4.0-devel-ubuntu22.04
RUN apt update && DEBIAN_FRONTEND=noninteractive apt upgrade -y && \
    apt install -y nvtop make g++-9 libcurl4-openssl-dev tmux git nano && \
	for v in 12-0 12-2; do apt install -y cuda-compiler-$v cuda-nvcc-$v cuda-libraries-$v cuda-libraries-dev-$v cuda-cudart-$v; done && \
    apt autoremove -y && apt clean && rm -rf /var/lib/apt/lists/*