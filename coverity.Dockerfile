FROM knight42/clang

# One dependency per line in alphabetical order.
# This should help avoiding duplicates and make the file easier to update.
RUN apt-get update && apt-get install -y \
    curl \
    git \
    libprocps-dev \
    libssl-dev \
    python-cheetah \
    pkg-config \
    wget \
    && rm -rf /var/lib/apt/lists/*

# Setup coverity
ARG TOKEN

RUN cd /opt \
    && wget https://scan.coverity.com/download/linux64 --post-data "token=${TOKEN}&project=realm%2Frealm-core" -O coverity_tool.tgz \
    && tar zxf coverity_tool.tgz \
    && rm coverity_tool.tgz \
    && mv cov-analysis-linux64-* cov-analysis-linux64 \
    && chmod -R a+w cov-analysis-linux64

ENV PATH "$PATH:/opt/cov-analysis-linux64/bin"