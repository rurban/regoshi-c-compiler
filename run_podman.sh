#!/bin/sh
podman build -t rcc .
podman run --rm -it rcc bash --login -i
