DRIVER_NAME   = DualAudioDriver
DRIVER_SRC    = DualAudioDriver/DualAudioDriver.c
DRIVER_PLIST  = DualAudioDriver/Info.plist
DRIVER_BUNDLE = build/$(DRIVER_NAME).driver

APP_BUNDLE    = build/DualAudio.app
APP_PLIST     = DualAudio/Sources/DualAudio/Info.plist

HAL_PLUGIN_DIR = /Library/Audio/Plug-Ins/HAL

CC      = clang
CFLAGS  = -std=c11 -O2 -Wall -Wextra \
          -arch arm64 -arch x86_64 \
          -framework CoreAudio \
          -framework CoreFoundation \
          -framework AudioToolbox

.PHONY: all driver app icon pkg release release-pkg notarize staple install uninstall clean check-root check-release-vars check-notary-profile

all: driver app

# ── Driver ────────────────────────────────────────────────────────────────────

driver: $(DRIVER_BUNDLE)/Contents/MacOS/$(DRIVER_NAME)

$(DRIVER_BUNDLE)/Contents/MacOS/$(DRIVER_NAME): \
        Makefile \
        $(DRIVER_SRC) \
        $(DRIVER_PLIST) \
        DualAudioDriver/DualAudioDriver.h \
        AudioBridge/include/DualAudioRingBuffer.h
	@mkdir -p $(DRIVER_BUNDLE)/Contents/MacOS
	$(CC) $(CFLAGS) \
	    -bundle \
	    -I DualAudioDriver \
	    -I AudioBridge/include \
	    -o $@ \
	    $(DRIVER_SRC)
	@cp $(DRIVER_PLIST) $(DRIVER_BUNDLE)/Contents/Info.plist
	codesign --force --sign - $(DRIVER_BUNDLE)
	@echo "Built $(DRIVER_BUNDLE)"

# ── App (.app bundle) ─────────────────────────────────────────────────────────

app: $(APP_BUNDLE)/Contents/MacOS/DualAudio

$(APP_BUNDLE)/Contents/MacOS/DualAudio: \
        Makefile \
        $(APP_PLIST) \
        $(wildcard DualAudio/Sources/DualAudio/*.swift) \
        $(wildcard AudioBridge/*.c) \
        $(wildcard AudioBridge/include/*.h) \
        scripts/AppIcon.icns
	@rm -rf $(APP_BUNDLE)
	@mkdir -p $(APP_BUNDLE)/Contents/MacOS .build/release-universal
	swift build --configuration release --arch arm64 2>&1
	swift build --configuration release --arch x86_64 2>&1
	lipo -create \
	    .build/arm64-apple-macosx/release/DualAudio \
	    .build/x86_64-apple-macosx/release/DualAudio \
	    -output .build/release-universal/DualAudio
	@cp .build/release-universal/DualAudio $(APP_BUNDLE)/Contents/MacOS/DualAudio
	@cp $(APP_PLIST) $(APP_BUNDLE)/Contents/Info.plist
	@mkdir -p $(APP_BUNDLE)/Contents/Resources
	@cp scripts/AppIcon.icns $(APP_BUNDLE)/Contents/Resources/AppIcon.icns
	@echo "Built $(APP_BUNDLE)"

# ── App icon ──────────────────────────────────────────────────────────────────

ICONSET = scripts/AppIcon.iconset

icon: scripts/AppIcon.icns

scripts/AppIcon.icns: scripts/icon_base.png
	@rm -rf $(ICONSET) && mkdir -p $(ICONSET)
	@sips -z 16   16   scripts/icon_base.png --out $(ICONSET)/icon_16x16.png      >/dev/null
	@sips -z 32   32   scripts/icon_base.png --out $(ICONSET)/icon_16x16@2x.png   >/dev/null
	@sips -z 32   32   scripts/icon_base.png --out $(ICONSET)/icon_32x32.png      >/dev/null
	@sips -z 64   64   scripts/icon_base.png --out $(ICONSET)/icon_32x32@2x.png   >/dev/null
	@sips -z 128  128  scripts/icon_base.png --out $(ICONSET)/icon_128x128.png    >/dev/null
	@sips -z 256  256  scripts/icon_base.png --out $(ICONSET)/icon_128x128@2x.png >/dev/null
	@sips -z 256  256  scripts/icon_base.png --out $(ICONSET)/icon_256x256.png    >/dev/null
	@sips -z 512  512  scripts/icon_base.png --out $(ICONSET)/icon_256x256@2x.png >/dev/null
	@sips -z 512  512  scripts/icon_base.png --out $(ICONSET)/icon_512x512.png    >/dev/null
	@sips -z 1024 1024 scripts/icon_base.png --out $(ICONSET)/icon_512x512@2x.png >/dev/null
	iconutil --convert icns $(ICONSET) --output scripts/AppIcon.icns
	@rm -rf $(ICONSET)
	@echo "Built scripts/AppIcon.icns"

# ── PKG installer ─────────────────────────────────────────────────────────────

PKG_OUT  = build/DualAudio.pkg
PKG_WORK = build/pkg-work
PKG_VERSION = 1.0

APP_PKG_ID    = com.dualaudio.DualAudio
DRIVER_PKG_ID = com.dualaudio.DualAudioDriver

# Release signing inputs. Set these on the make command line.
DEVELOPER_ID_APP ?=
DEVELOPER_ID_INSTALLER ?=
NOTARY_PROFILE ?= DualAudio

pkg: all
	@rm -rf $(PKG_WORK) && mkdir -p $(PKG_WORK)
	@echo "Staging app…"
	@mkdir -p "$(PKG_WORK)/app-root/Applications"
	@cp -R "$(APP_BUNDLE)" "$(PKG_WORK)/app-root/Applications/"
	@codesign --force --sign - "$(PKG_WORK)/app-root/Applications/DualAudio.app"
	@echo "Staging driver…"
	@mkdir -p "$(PKG_WORK)/driver-root/Library/Audio/Plug-Ins/HAL"
	@cp -R "$(DRIVER_BUNDLE)" "$(PKG_WORK)/driver-root/Library/Audio/Plug-Ins/HAL/"
	@mkdir -p "$(PKG_WORK)/driver-scripts"
	@printf '#!/bin/sh\nlaunchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || killall coreaudiod 2>/dev/null || true\nexit 0\n' \
		> "$(PKG_WORK)/driver-scripts/postinstall"
	@chmod +x "$(PKG_WORK)/driver-scripts/postinstall"
	@echo "Building component packages…"
	@printf '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0">\n<array>\n  <dict>\n    <key>BundleIsRelocatable</key><false/>\n    <key>BundleIsVersionChecked</key><true/>\n    <key>BundleHasStrictIdentifier</key><true/>\n    <key>BundleOverwriteAction</key><string>upgrade</string>\n    <key>RootRelativeBundlePath</key><string>Applications/DualAudio.app</string>\n  </dict>\n</array>\n</plist>\n' \
		> "$(PKG_WORK)/app-components.plist"
	pkgbuild --root "$(PKG_WORK)/app-root" \
	         --component-plist "$(PKG_WORK)/app-components.plist" \
	         --install-location / \
	         --identifier $(APP_PKG_ID) \
	         --version $(PKG_VERSION) \
	         "$(PKG_WORK)/app.pkg"
	pkgbuild --root "$(PKG_WORK)/driver-root" \
	         --scripts "$(PKG_WORK)/driver-scripts" \
	         --identifier $(DRIVER_PKG_ID) \
	         --version $(PKG_VERSION) \
	         "$(PKG_WORK)/driver.pkg"
	@echo "Building installer…"
	productbuild --distribution scripts/distribution.xml \
	             --package-path "$(PKG_WORK)" \
	             "$(PKG_OUT)"
	@rm -rf "$(PKG_WORK)"
	@echo "Built $(PKG_OUT) — share this file directly."

# ── Direct distribution release ───────────────────────────────────────────────

release: staple

release-pkg: check-release-vars all
	@rm -rf $(PKG_WORK) && mkdir -p $(PKG_WORK)
	@echo "Staging app..."
	@mkdir -p "$(PKG_WORK)/app-root/Applications"
	@cp -R "$(APP_BUNDLE)" "$(PKG_WORK)/app-root/Applications/"
	codesign --force --timestamp --options runtime \
	    --sign "$(DEVELOPER_ID_APP)" \
	    "$(PKG_WORK)/app-root/Applications/DualAudio.app"
	codesign --verify --strict --verbose=2 \
	    "$(PKG_WORK)/app-root/Applications/DualAudio.app"
	@echo "Staging driver..."
	@mkdir -p "$(PKG_WORK)/driver-root$(HAL_PLUGIN_DIR)"
	@cp -R "$(DRIVER_BUNDLE)" "$(PKG_WORK)/driver-root$(HAL_PLUGIN_DIR)/"
	codesign --force --timestamp --options runtime \
	    --sign "$(DEVELOPER_ID_APP)" \
	    "$(PKG_WORK)/driver-root$(HAL_PLUGIN_DIR)/$(DRIVER_NAME).driver"
	codesign --verify --strict --verbose=2 \
	    "$(PKG_WORK)/driver-root$(HAL_PLUGIN_DIR)/$(DRIVER_NAME).driver"
	@mkdir -p "$(PKG_WORK)/driver-scripts"
	@printf '#!/bin/sh\nlaunchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || killall coreaudiod 2>/dev/null || true\nexit 0\n' \
		> "$(PKG_WORK)/driver-scripts/postinstall"
	@chmod +x "$(PKG_WORK)/driver-scripts/postinstall"
	@echo "Building component packages..."
	@printf '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0">\n<array>\n  <dict>\n    <key>BundleIsRelocatable</key><false/>\n    <key>BundleIsVersionChecked</key><true/>\n    <key>BundleHasStrictIdentifier</key><true/>\n    <key>BundleOverwriteAction</key><string>upgrade</string>\n    <key>RootRelativeBundlePath</key><string>Applications/DualAudio.app</string>\n  </dict>\n</array>\n</plist>\n' \
		> "$(PKG_WORK)/app-components.plist"
	pkgbuild --root "$(PKG_WORK)/app-root" \
	         --component-plist "$(PKG_WORK)/app-components.plist" \
	         --install-location / \
	         --identifier $(APP_PKG_ID) \
	         --version $(PKG_VERSION) \
	         "$(PKG_WORK)/app.pkg"
	pkgbuild --root "$(PKG_WORK)/driver-root" \
	         --scripts "$(PKG_WORK)/driver-scripts" \
	         --identifier $(DRIVER_PKG_ID) \
	         --version $(PKG_VERSION) \
	         "$(PKG_WORK)/driver.pkg"
	@echo "Building signed installer..."
	productbuild --sign "$(DEVELOPER_ID_INSTALLER)" \
	             --timestamp \
	             --distribution scripts/distribution.xml \
	             --package-path "$(PKG_WORK)" \
	             "$(PKG_OUT)"
	pkgutil --check-signature "$(PKG_OUT)"
	@rm -rf "$(PKG_WORK)"
	@echo "Built signed release package: $(PKG_OUT)"

notarize: check-notary-profile release-pkg
	xcrun notarytool submit "$(PKG_OUT)" \
	    --keychain-profile "$(NOTARY_PROFILE)" \
	    --wait

staple: notarize
	xcrun stapler staple "$(PKG_OUT)"
	xcrun stapler validate "$(PKG_OUT)"
	spctl -a -vv -t install "$(PKG_OUT)"
	@echo "Release package is ready: $(PKG_OUT)"

# ── Install / uninstall ───────────────────────────────────────────────────────

install: driver check-root
	@echo "Installing $(DRIVER_NAME).driver → $(HAL_PLUGIN_DIR)/"
	cp -R $(DRIVER_BUNDLE) $(HAL_PLUGIN_DIR)/
	@echo "Restarting coreaudiod…"
	launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || \
	    killall coreaudiod 2>/dev/null || true
	@build_owner="$${SUDO_USER:-$$(logname 2>/dev/null || id -un)}"; \
	build_group="$$(id -gn "$$build_owner")"; \
	chown -R "$$build_owner:$$build_group" build .build 2>/dev/null || true
	@echo "Done. Open System Settings → Sound → Output to verify 'Dual Audio' appears."

uninstall: check-root
	@echo "Removing $(HAL_PLUGIN_DIR)/$(DRIVER_NAME).driver"
	rm -rf "$(HAL_PLUGIN_DIR)/$(DRIVER_NAME).driver"
	launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || \
	    killall coreaudiod 2>/dev/null || true
	@echo "Uninstalled."

check-root:
	@if [ "$$(id -u)" -ne 0 ]; then \
	    echo "ERROR: install/uninstall require sudo.  Run: sudo make install"; \
	    exit 1; \
	fi

check-release-vars:
	@if [ -z "$(DEVELOPER_ID_APP)" ]; then \
	    echo "ERROR: set DEVELOPER_ID_APP='Developer ID Application: Your Name (TEAMID)'"; \
	    exit 1; \
	fi
	@if [ -z "$(DEVELOPER_ID_INSTALLER)" ]; then \
	    echo "ERROR: set DEVELOPER_ID_INSTALLER='Developer ID Installer: Your Name (TEAMID)'"; \
	    exit 1; \
	fi

check-notary-profile:
	@if [ -z "$(NOTARY_PROFILE)" ]; then \
	    echo "ERROR: set NOTARY_PROFILE to a notarytool keychain profile name"; \
	    exit 1; \
	fi

# ── Clean ─────────────────────────────────────────────────────────────────────

clean:
	rm -rf build/ .build/ scripts/AppIcon.icns scripts/AppIcon.iconset/ $(PKG_WORK)
