# Container images for DeskuBe desktops. The operator,
# github.com/epheo/deskube, only knows these images by name.
CONTAINER_TOOL ?= podman
REGISTRY ?= ghcr.io/epheo
# A tag of your own keeps parallel builds from replacing each other's images.
TAG ?= main
BASE_IMG ?= $(REGISTRY)/deskube-base:$(TAG)
DESKTOP_IMG ?= $(REGISTRY)/deskube-desktop:$(TAG)
STEAM_IMG ?= $(REGISTRY)/deskube-steam:$(TAG)
MOONLIGHT_CLI_IMG ?= moonlight-cli
# CI adds labels here, such as the source repository ghcr links a package to.
BUILD_FLAGS ?=

.PHONY: help
help: ## Show this help.
	@awk 'BEGIN {FS = ":.*##"} /^[a-zA-Z_-]+:.*?##/ { printf "  %-16s %s\n", $$1, $$2 }' $(MAKEFILE_LIST)

.PHONY: base
base: ## Build the base image: compositor, Sunshine, the shim, nothing to reach.
	$(CONTAINER_TOOL) build $(BUILD_FLAGS) -t $(BASE_IMG) base

.PHONY: desktop
desktop: base ## Build the desktop image: sway with its keys, a terminal, Chromium.
	$(CONTAINER_TOOL) build $(BUILD_FLAGS) -t $(DESKTOP_IMG) --build-arg BASE=$(BASE_IMG) desktop

.PHONY: steam
steam: base ## Build the Steam image on the base image.
	$(CONTAINER_TOOL) build $(BUILD_FLAGS) -t $(STEAM_IMG) --build-arg BASE=$(BASE_IMG) steam

.PHONY: moonlight-cli
moonlight-cli: ## Build the headless Moonlight client hack/e2e.sh streams with.
	$(CONTAINER_TOOL) build $(BUILD_FLAGS) -t $(MOONLIGHT_CLI_IMG) moonlight-cli

.PHONY: push
push: ## Push the base, desktop and Steam images.
	$(CONTAINER_TOOL) push $(BASE_IMG)
	$(CONTAINER_TOOL) push $(DESKTOP_IMG)
	$(CONTAINER_TOOL) push $(STEAM_IMG)
