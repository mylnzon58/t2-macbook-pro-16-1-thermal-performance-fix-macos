# Makefile — MacBook Pro 16,1 (A2141) Asymmetric HWP & Thermal Driver (macOS)
# Author: Juan José Monzón (@mylnzon58)

SDK_PATH    := $(shell xcrun --show-sdk-path)
KERNEL_INC  := $(SDK_PATH)/System/Library/Frameworks/Kernel.framework/Headers
BUILD_DIR   := build
KEXT_BUNDLE := $(BUILD_DIR)/A2141AsymmetricHWP.kext
KEXT_BIN    := $(KEXT_BUNDLE)/Contents/MacOS/A2141AsymmetricHWP
CLI_BIN     := $(BUILD_DIR)/a2141-ctl

CXX         := clang++
CC          := clang
KEXT_MIN    := -mmacosx-version-min=26.2
CLI_MIN     := -mmacosx-version-min=12.0

KEXT_CXXFLAGS := -arch x86_64 $(KEXT_MIN) -mkernel -nostdinc \
                 -isystem "$(KERNEL_INC)" -Iinclude \
                 -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
                 -Wall -Wextra -Werror -O2 -fno-builtin -fno-common -fno-rtti -fno-exceptions

KEXT_LDFLAGS  := -arch x86_64 $(KEXT_MIN) -Wl,-kext -nostdlib -lkmodc++ -lkmod -lcc_kext

CLI_CFLAGS    := -arch x86_64 $(CLI_MIN) -mavx2 -mfma -Iinclude -Wall -Wextra -Werror -O3 -pthread
CLI_LDFLAGS   := -arch x86_64 $(CLI_MIN) -framework IOKit -framework CoreFoundation -lpthread

.PHONY: all i9 kext cli verify clean

all: kext cli verify

i9:
	@$(MAKE) BUILD_DIR=build-i9 \
		KEXT_CXXFLAGS="$(KEXT_CXXFLAGS) -DA2141_MODEL_I9=1" \
		CLI_CFLAGS="$(CLI_CFLAGS) -DA2141_MODEL_I9=1" all

kext: $(KEXT_BIN)

$(KEXT_BIN): kext/A2141AsymmetricHWP.cpp kext/A2141AsymmetricHWP.hpp kext/Info.plist include/a2141_shared.h resources/DriverIcon.icns
	@mkdir -p $(KEXT_BUNDLE)/Contents/MacOS $(KEXT_BUNDLE)/Contents/Resources
	@cp kext/Info.plist $(KEXT_BUNDLE)/Contents/Info.plist
	@cp resources/DriverIcon.icns $(KEXT_BUNDLE)/Contents/Resources/DriverIcon.icns
	$(CXX) $(KEXT_CXXFLAGS) -c kext/A2141AsymmetricHWP.cpp -o $(BUILD_DIR)/A2141AsymmetricHWP.o
	$(CXX) $(KEXT_LDFLAGS) $(BUILD_DIR)/A2141AsymmetricHWP.o -o $(KEXT_BIN)

cli: $(CLI_BIN)

$(CLI_BIN): cli/a2141_ctl.c cli/smc_client.c cli/smc_client.h include/a2141_shared.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CLI_CFLAGS) cli/a2141_ctl.c cli/smc_client.c -o $(CLI_BIN) $(CLI_LDFLAGS)

verify: $(KEXT_BIN) $(CLI_BIN)
	@echo "[+] Verificando binario Ring-0 y simbolos contra /System/Library/Kernels/kernel..."
	@file $(KEXT_BIN)
	@file $(CLI_BIN)
	@rm -rf electron-app/out 2>/dev/null || true

clean:
	rm -rf $(BUILD_DIR) build-i9 electron-app/out
