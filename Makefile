# This Makefile is for building the AppImage wrapper on Linux systems.

# Copyright (C) 2026 imngkhang
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, version 3 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.fsf.org/licenses/>.

# Here we will check the arch of the system
UNAME_M := $(shell uname -m)

# Set the default arch
ifeq ($(UNAME_M),aarch64)
    DEFAULT_ARCH := aarch64
else ifeq ($(UNAME_M),arm64)
    DEFAULT_ARCH := aarch64
else ifeq ($(UNAME_M),x86_64)
    DEFAULT_ARCH := x86_64
else
    $(error Unsupported architecture: $(UNAME_M). Only x86_64 and aarch64 are supported)
endif

# make targets
.PHONY: all x86_64 aarch64 clean depends help

# Build with default arch
all: $(DEFAULT_ARCH)

# Build for specific architectures
x86_64:
	@chmod +x buildscripts/classin_build_amd64.sh
	@./buildscripts/classin_build_amd64.sh

aarch64:
	@chmod +x buildscripts/classin_build_arm64.sh
	@./buildscripts/classin_build_arm64.sh

# Build for all architectures
build-all: x86_64 aarch64

# Check dependencies
depends:
	@for cmd in jq wget ar tar sha256sum stat; do \
		if ! command -v $$cmd >/dev/null 2>&1; then \
			echo "Missing: $$cmd"; \
		else \
			echo "OK: $$cmd"; \
		fi \
	done

# Clean up build artifacts
clean:
	@echo "Cleaning up build artifacts..."
	@rm -rf dist/ *.AppDir/ appimagetool *.deb
	@echo "Done!"

# Show help message
help:
	@echo "Build Commands:"
	@echo "  make           - Build for current architecture ($(DEFAULT_ARCH)), default"
	@echo "  make x86_64    - Build a x86_64 AppImage"
	@echo "  make aarch64   - Build an aarch64 AppImage"
	@echo "  make build-all - Build all architectures"
	@echo "  make clean     - Remove build and temporary files"
	@echo "  make depends   - Check dependencies for building"
