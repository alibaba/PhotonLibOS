# ARC runner for the photon-arc-runner-fstack scale set.
# Run fstack compilation directly in the runner, without a job container.
ARG BASE_IMAGE=ghcr.io/alibaba/photon-ut-fstack:latest
FROM ${BASE_IMAGE}

ARG RUNNER_VERSION=2.338.0
ARG TARGETARCH

# The existing fstack build image is published for linux/amd64 only.
RUN test "${TARGETARCH}" = "amd64"

ENV RUNNER_MANUALLY_TRAP_SIG=1
ENV ACTIONS_RUNNER_PRINT_LOG_TO_STDOUT=1

# Bullseye security packages have moved out of the active Debian mirror.
RUN sed -i 's|http://deb.debian.org/debian-security|https://archive.debian.org/debian-security|g' /etc/apt/sources.list && \
    rm -rf /var/lib/apt/lists/* && \
    apt-get update && \
    apt-get install -y --no-install-recommends ca-certificates curl sudo && \
    rm -rf /var/lib/apt/lists/*

RUN useradd -m -u 1001 -s /bin/bash runner && \
    echo "runner ALL=(ALL:ALL) NOPASSWD:ALL" > /etc/sudoers.d/runner && \
    chmod 0440 /etc/sudoers.d/runner

WORKDIR /home/runner

RUN curl -fL --retry 3 -o runner.tar.gz \
        "https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/actions-runner-linux-x64-${RUNNER_VERSION}.tar.gz" && \
    tar xzf runner.tar.gz && \
    rm runner.tar.gz && \
    ./bin/installdependencies.sh && \
    ./bin/Runner.Listener --version && \
    rm -rf /var/lib/apt/lists/* && \
    chown -R runner:runner /home/runner

ENV LD_LIBRARY_PATH=/usr/local/lib:/usr/local/lib64

USER runner

ENTRYPOINT ["/home/runner/run.sh"]
