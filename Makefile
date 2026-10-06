# Container images for DeskuBe desktops. The operator,
# github.com/epheo/deskube, only knows these images by name.
CONTAINER_TOOL ?= podman
REGISTRY ?= registry.desku.be/deskube
BASE_IMG ?= $(REGISTRY)/base:dev
DESKTOP_IMG ?= $(REGISTRY)/desktop:dev
STEAM_IMG ?= $(REGISTRY)/desktop-steam:dev

.PHONY: help
help: ## Show this help.
	@awk 'BEGIN {FS = ":.*##"} /^[a-zA-Z_-]+:.*?##/ { printf "  %-16s %s\n", $$1, $$2 }' $(MAKEFILE_LIST)

.PHONY: base
base: ## Build the base image: compositor, Sunshine, the shim, nothing to reach.
	$(CONTAINER_TOOL) build -t $(BASE_IMG) base

.PHONY: desktop
desktop: base ## Build the desktop image: sway with its keys, a terminal, Chromium.
	$(CONTAINER_TOOL) build -t $(DESKTOP_IMG) --build-arg BASE=$(BASE_IMG) desktop

.PHONY: steam
steam: base ## Build the Steam image on the base image.
	$(CONTAINER_TOOL) build -t $(STEAM_IMG) --build-arg BASE=$(BASE_IMG) steam

.PHONY: moonlight-cli
moonlight-cli: ## Build the headless Moonlight client hack/e2e.sh streams with.
	$(CONTAINER_TOOL) build -t moonlight-cli moonlight-cli

.PHONY: push
push: ## Push the base, desktop and Steam images.
	$(CONTAINER_TOOL) push $(BASE_IMG)
	$(CONTAINER_TOOL) push $(DESKTOP_IMG)
	$(CONTAINER_TOOL) push $(STEAM_IMG)
