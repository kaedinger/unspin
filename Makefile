##############################################################
# Unspin - build system
# Produces the .txz package that the .plg installs
##############################################################

PLUGIN   := unspin
PLG_FILE := unspin.plg
VERSION  := $(shell date +%Y.%m.%d)
PKG_DIR  := pkg
BUILD    := build/$(PLUGIN)-$(VERSION)

CXX      := g++
CXXFLAGS := -O2 -Wall -std=c++17

.PHONY: all build-daemon package clean update-plg update-plg-hash install-local

all: build-daemon package

build-daemon:
	@echo "→ Compiling unspind"
	@mkdir -p usr/local/sbin
	@$(CXX) $(CXXFLAGS) -o usr/local/sbin/unspind src/unspind.cpp
	@chmod +x usr/local/sbin/unspind
	@echo "✓ unspind compiled"

package:
	@echo "→ Building $(PLUGIN)-$(VERSION)"
	@mkdir -p $(BUILD)

	# Copy installable files
	@rsync -a usr/ $(BUILD)/usr/
	@rsync -a etc/ $(BUILD)/etc/
	@rsync -a install/ $(BUILD)/install/

	# Ensure executables have correct permissions
	@chmod +x $(BUILD)/usr/local/sbin/unspind
	@chmod +x $(BUILD)/etc/rc.d/rc.unspin

	# Create the TXZ
	@mkdir -p $(PKG_DIR)
	@cd $(BUILD) && \
	  tar --create \
	      --xz \
	      --file=../../$(PKG_DIR)/$(PLUGIN)-$(VERSION)-x86_64-1.txz \
	      install/ usr/ etc/

	@echo "✓ Package: $(PKG_DIR)/$(PLUGIN)-$(VERSION)-x86_64-1.txz"

	@$(MAKE) --no-print-directory update-plg-hash VERSION=$(VERSION)

# Update the version string in the .plg so it matches
update-plg:
	@sed -i "s|<!ENTITY version.*|<!ENTITY version   \"$(VERSION)\">|" $(PLG_FILE)
	@echo "✓ Updated .plg version to $(VERSION)"

# Compute the sha256 of the just-built TXZ and inject it into the .plg's <SHA256>
# tag. Called at the end of `package`.
update-plg-hash:
	@HASH=$$(sha256sum $(PKG_DIR)/$(PLUGIN)-$(VERSION)-x86_64-1.txz | cut -d' ' -f1); \
	sed -i "s|<SHA256>.*</SHA256>|<SHA256>$$HASH</SHA256>|" $(PLG_FILE); \
	echo "✓ Updated .plg SHA256 to $$HASH"

clean:
	@rm -rf build/
	@echo "✓ Cleaned build artifacts"

# Quick local test - install directly on a running Unraid system
# Usage: make install-local HOST=tower
install-local:
	scp $(PKG_DIR)/$(PLUGIN)-$(VERSION)-x86_64-1.txz root@$(HOST):/tmp/
	ssh root@$(HOST) "installpkg /tmp/$(PLUGIN)-$(VERSION)-x86_64-1.txz"
