# Container images for DeskuBe desktops. The operator,
# github.com/epheo/deskube, only knows these images by name.
CONTAINER_TOOL ?= podman
REGISTRY ?= registry.desku.be/deskube
DESKTOP_IMG ?= $(REGISTRY)/desktop:dev
STEAM_IMG ?= $(REGISTRY)/desktop-steam:dev

.PHONY: help
help: ## Show this help.
	@awk 'BEGIN {FS = ":.*##"} /^[a-zA-Z_-]+:.*?##/ { printf "  %-16s %s\n", $$1, $$2 }' $(MAKEFILE_LIST)

.PHONY: desktop
desktop: ## Build the desktop image.
	$(CONTAINER_TOOL) build -t $(DESKTOP_IMG) desktop

.PHONY: steam
steam: ## Build the Steam image on the desktop image.
	$(CONTAINER_TOOL) build -t $(STEAM_IMG) --build-arg BASE=$(DESKTOP_IMG) steam

.PHONY: moonlight-cli
moonlight-cli: ## Build the headless Moonlight client hack/e2e.sh streams with.
	$(CONTAINER_TOOL) build -t moonlight-cli moonlight-cli

.PHONY: push
push: ## Push the desktop and Steam images.
	$(CONTAINER_TOOL) push $(DESKTOP_IMG)
	$(CONTAINER_TOOL) push $(STEAM_IMG)
