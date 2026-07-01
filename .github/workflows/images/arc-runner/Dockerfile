# ARC (Actions Runner Controller) runner image for scalable runner sets.
# Based on photon-ut-base which contains all build/test dependencies.
#
# Usage:
#   docker build --build-arg RUNNER_VERSION=2.323.0 \
#                --build-arg RUNNER_CONTAINER_HOOKS_VERSION=0.7.0 \
#                -t photon-arc-runner .

ARG BASE_IMAGE=ghcr.io/alibaba/photon-ut-base:latest
FROM ${BASE_IMAGE}

# Runner release versions - update as needed
# https://github.com/actions/runner/releases
ARG RUNNER_VERSION=2.323.0
# https://github.com/actions/runner-container-hooks/releases
ARG RUNNER_CONTAINER_HOOKS_VERSION=0.7.0

# Map Docker platform to runner arch
ARG TARGETARCH
RUN RUNNER_ARCH="" && \
    if [ "${TARGETARCH}" = "amd64" ]; then RUNNER_ARCH="x64"; \
    elif [ "${TARGETARCH}" = "arm64" ]; then RUNNER_ARCH="arm64"; \
    else echo "Unsupported arch: ${TARGETARCH}" && exit 1; fi && \
    echo "RUNNER_ARCH=${RUNNER_ARCH}" > /tmp/runner_arch.env

# ARC environment variables
ENV RUNNER_MANUALLY_TRAP_SIG=1
ENV ACTIONS_RUNNER_PRINT_LOG_TO_STDOUT=1

# Install additional packages needed by the runner
RUN dnf install -y icu libicu dotnet-runtime-8.0 && \
    dnf clean all && rm -rf /var/cache/yum || \
    # Fallback: install only icu if dotnet repo is unavailable
    (dnf install -y icu libicu && dnf clean all && rm -rf /var/cache/yum)

# Create runner user with UID 1001
RUN useradd -m -u 1001 -s /bin/bash runner && \
    echo "runner ALL=(ALL:ALL) NOPASSWD:ALL" >> /etc/sudoers

WORKDIR /home/runner

# Install runner binary
RUN source /tmp/runner_arch.env && \
    curl -fL -o runner.tar.gz \
        "https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/actions-runner-linux-${RUNNER_ARCH}-${RUNNER_VERSION}.tar.gz" && \
    tar xzf runner.tar.gz && \
    rm runner.tar.gz && \
    # Install runner dependencies
    ./bin/installdependencies.sh || true

# Install runner container hooks (for Kubernetes mode)
RUN source /tmp/runner_arch.env && \
    curl -fL -o runner-container-hooks.zip \
        "https://github.com/actions/runner-container-hooks/releases/download/v${RUNNER_CONTAINER_HOOKS_VERSION}/actions-runner-hooks-k8s-${RUNNER_CONTAINER_HOOKS_VERSION}.zip" && \
    unzip runner-container-hooks.zip -d ./k8s && \
    rm runner-container-hooks.zip

# Clean up temp file
RUN rm -f /tmp/runner_arch.env

# Ensure runner user owns the working directory
RUN chown -R runner:runner /home/runner

# Retain LD_LIBRARY_PATH from base image
ENV LD_LIBRARY_PATH=/usr/local/lib:/usr/local/lib64:/usr/lib:/usr/lib64:/lib:/lib64

USER runner

ENTRYPOINT ["/home/runner/run.sh"]
