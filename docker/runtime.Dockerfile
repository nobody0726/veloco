# Veloco runtime image, pinned to Ubuntu 24.04.
#
# This image is generic: it expects the veloco-httpd binary to be
# provided by the build context or an earlier build stage and copies it
# into a minimal base. The veloco-httpd binary is expected from Task 8 of
# the implementation plan; until then the COPY step fails by design.
#
# Supported platforms: linux/amd64 and linux/arm64
#   docker build --platform linux/amd64 \
#     --build-arg VELOCO_HTTPD_BINARY=build/x86_64/uring/veloco-httpd \
#     -f docker/runtime.Dockerfile -t veloco-httpd:local .
FROM ubuntu:24.04

ARG VELOCO_HTTPD_BINARY=build/x86_64/uring/veloco-httpd

COPY "${VELOCO_HTTPD_BINARY}" /usr/local/bin/veloco-httpd

ENV VELOCO_HTTP_ADDR=0.0.0.0 \
    VELOCO_HTTP_PORT=8080

EXPOSE 8080

ENTRYPOINT ["/usr/local/bin/veloco-httpd"]
