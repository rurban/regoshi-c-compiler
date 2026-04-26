# podman/docker build -t rcc .
# podman/docker run --rm -it rcc bash --login -i

FROM ubuntu:latest
RUN apt-get update && \
    apt-get install -y --no-install-recommends build-essential gcc less sudo
ENV TEST_USER tester
ENV WORK_DIR "/work"
WORKDIR "${WORK_DIR}"
RUN useradd "${TEST_USER}"
# Enable sudo without password for convenience.
RUN echo "${TEST_USER} ALL = NOPASSWD: ALL" >> /etc/sudoers
COPY . .
RUN chown -R "${TEST_USER}:${TEST_USER}" "${WORK_DIR}"
USER "${TEST_USER}"
