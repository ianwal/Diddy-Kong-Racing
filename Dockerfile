FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    binutils-mips-linux-gnu \
    build-essential \
    gcc-mips-linux-gnu \
    gdb-multiarch \
    libglib2.0 \
    python3 \
    python3-pip \
    python-is-python3 \
    python3-venv \
    unzip \
    wget \
    libssl-dev \
    vbindiff \
    git \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir /Diddy-Kong-Racing
WORKDIR /Diddy-Kong-Racing

RUN python -m venv /opt/venv
# Enable venv
ENV PATH="/opt/venv/bin:$PATH"

COPY requirements.txt /Diddy-Kong-Racing/
RUN python3 -m pip install -r /Diddy-Kong-Racing/requirements.txt
