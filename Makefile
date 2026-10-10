# PShare - Linux/WSL build entry points.
# Built on ps5-native-app-boilerplate's pipeline, with the ps5-homebrew-ui
# OpenGL kit, PacBrew libcurl and the PS5-Lapy-JB-Daemon elevation helper.
# Copyright (C) 2026 PShare contributors
# SPDX-License-Identifier: GPL-3.0-or-later

SHELL := /bin/bash
.DEFAULT_GOAL := app

-include .env

APP_DEFINITIONS ?=
APP_INCLUDE_PATHS ?=
APP_STATIC_ARCHIVES ?=
APP_IMPORT_STUBS ?=
APP_RUNTIME_MODULES ?=
APP_WRAP_SYMBOLS ?=
APP_ROOT_FILES ?=
# Empty selects the pinned ps5-opengl release (tools/fetch-opengl-sdk.sh).
PS5_OPENGL_PREFIX ?=

# PShare always builds against ps5-opengl, libcurl, FFmpeg, libsmb2 and the Lapy helper; user
# APP_* values append.
OPENGL_SDK := .deps/ps5-opengl/current
DAEMON_ELF := build/daemon/pshared.elf
override APP_DEFINITIONS := $(strip GL_GLEXT_PROTOTYPES=1 PSHARE_CONSOLE=1 PSHARE_HAVE_SMB2=1 \
	$(APP_DEFINITIONS))
override APP_INCLUDE_PATHS := $(strip src src/net $(OPENGL_SDK)/include $(APP_INCLUDE_PATHS))
override APP_STATIC_ARCHIVES := $(strip .deps/ps5-opengl/libps5opengl-group.a $(APP_STATIC_ARCHIVES))
override APP_IMPORT_STUBS := $(strip $(OPENGL_SDK)/lib/libSceAgc.so \
	$(OPENGL_SDK)/lib/libSceAgcDriver.so $(APP_IMPORT_STUBS))
# fcntl: curl on sandboxed sockets; pthread_create: room on the stack (src/runtime/libc_extras.c).
override APP_WRAP_SYMBOLS := $(strip fcntl pthread_create $(APP_WRAP_SYMBOLS))
override APP_ROOT_FILES := $(strip $(DAEMON_ELF) $(APP_ROOT_FILES))
# FFmpeg plays the clips (and draws their tiles); libsmb2 writes to PC shares.
override PACBREW_PACKAGES := $(strip libcurl libavformat libavcodec libswscale libswresample \
	libavutil libsmb2 $(PACBREW_PACKAGES))
APP_LAPY_HELPER := 1
PACBREW_INCLUDE_PATHS ?=
PACBREW_STATIC_ARCHIVES ?=
PS5_HOST ?=
FTP_PORT ?= 2121
PS5_FTP_USER ?= anonymous
PS5_FTP_PASSWORD ?=
DEPLOY_DRY_RUN ?= 0
TITLE_ID ?=
APP_NAME ?=
APP_CATEGORY ?= media
CONTENT_SUFFIX ?=
HOST_CC ?= clang
HOST_CXX ?= clang++
HOST_TEST_CXXFLAGS ?= -std=c++20 -O1 -g -Wall -Wextra -Wpedantic -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all
BUILD_JOBS ?= $(shell nproc 2>/dev/null || echo 2)
USE_CCACHE ?= 1
export BUILD_JOBS USE_CCACHE
export HOST_CC HOST_CXX HOST_TEST_CXXFLAGS
export APP_DEFINITIONS APP_INCLUDE_PATHS APP_STATIC_ARCHIVES APP_IMPORT_STUBS APP_RUNTIME_MODULES
export APP_WRAP_SYMBOLS APP_ROOT_FILES APP_LAPY_HELPER PS5_OPENGL_PREFIX
export PACBREW_PACKAGES PACBREW_INCLUDE_PATHS PACBREW_STATIC_ARCHIVES
export PS5_HOST FTP_PORT PS5_FTP_USER PS5_FTP_PASSWORD DEPLOY_DRY_RUN
export TITLE_ID APP_NAME APP_CATEGORY CONTENT_SUFFIX

RUNTIME := runtime/libc.prx
RUNTIME_INPUTS := tools/rebuild-libc.sh tools/build-host-tools.sh tools/ninja-build.sh \
	$(wildcard tooling/native/*.cpp tooling/native/*.hpp) \
	$(wildcard tooling/native/runtime/*.txt)

.PHONY: all app build daemon init doctor test test-unit test-elevation test-integration libc deps \
	opengl pacbrew pacbrew-list preview art assets-check format format-check lint check \
	deploy undeploy clean distclean help

all: app
build: app

init:
	@printf '%s\n' '==> [init] Configuring the application identity in sce_sys/param.json'
	@bash tools/init-project.sh sce_sys/param.json

doctor:
	@printf '%s\n' '==> [doctor] Checking the Linux/WSL host without changing it'
	@bash tools/doctor.sh

deps:
	@printf '%s\n' '==> [deps] Fetching declared native dependencies'
	@bash tools/setup-native-dependencies.sh
	@bash tools/setup-pacbrew-dependencies.sh --environment
	@bash tools/prepare-opengl.sh

opengl:
	@printf '%s\n' '==> [opengl] Preparing the ps5-opengl SDK link group'
	@bash tools/prepare-opengl.sh

pacbrew:
	@bash tools/setup-pacbrew-dependencies.sh --all

pacbrew-list:
	@bash tools/setup-pacbrew-dependencies.sh --list

libc:
	@printf '%s\n' '==> [libc] Rebuilding and verifying the clean-room runtime'
	@bash tools/rebuild-libc.sh

$(RUNTIME): $(RUNTIME_INPUTS)
	@printf '%s\n' '==> [libc] Generating the missing or outdated runtime'
	@bash tools/rebuild-libc.sh

# The background uploader: a payload for the console's ELF loader (port 9021).
daemon:
	@printf '%s\n' '==> [daemon] Building pshared.elf for the payload loader'
	@bash tools/build-daemon.sh "$(DAEMON_ELF)"

app: $(RUNTIME) opengl daemon
	@printf '%s\n' '==> [app] Compiling, linking, signing, and assembling the PShare folder'
	@bash tools/build.sh Folder
	@cp -f "$(DAEMON_ELF)" dist/pshared.elf
	@printf 'Standalone daemon: %s\n' "$(CURDIR)/dist/pshared.elf"

# The interface rendered on this PC (Mesa, no console): PNGs in build/preview.
preview:
	@printf '%s\n' '==> [preview] Rendering the PShare screens on the PC'
	@bash tools/host-preview.sh build/preview

# Rebuild the icon, backgrounds and splash from art/ (Python 3 with Pillow).
art:
	@printf '%s\n' '==> [art] Rendering the icon, backgrounds and splash'
	@python3 tools/make-art.py

assets-check:
	@printf '%s\n' '==> [assets] Validating icon, backgrounds, and selection audio'
	@bash tools/validate-assets.sh

test: test-unit test-elevation test-integration

test-unit:
	@printf '%s\n' '==> [test-unit] Config, media, upload-request and history tests'
	@bash tools/build-tests.sh
	@build/tests/pshare_tests

test-elevation:
	@bash tools/setup-native-dependencies.sh >/dev/null
	@mkdir -p build/tests
	@$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -Isrc \
		-idirafter .deps/native/ps5-payload-sdk/target/include \
		tests/test_elevation.cpp -o build/tests/test_elevation
	@build/tests/test_elevation
	@printf '%s\n' 'Resident/one-shot Lapy client, exchange, and proof checks passed.'

test-integration:
	@python3 -m unittest discover -s tests -p "test_*.py"

format:
	@bash tools/run_clang_format.sh

format-check:
	@bash tools/run_clang_format.sh --check

lint: format-check

check: lint test app

deploy:
	@printf '%s\n' '==> [deploy] Building and publishing the app folder over FTP'
	@bash tools/deploy.sh

undeploy:
	@bash tools/deploy.sh undeploy

clean:
	@printf '%s\n' '==> [clean] Removing generated build outputs'
	@rm -rf -- build dist
	@rm -f -- $(RUNTIME)

distclean: clean
	@rm -rf -- .deps

help:
	@printf '%s\n' \
	  'make                 Build dist/<TITLE_ID>/ (the app), its ZIP, and dist/pshared.elf' \
	  'make daemon          Build only the background uploader payload' \
	  'make preview         Render the PShare screens on this PC into build/preview' \
	  'make art             Re-render the icon, backgrounds, splash and README banner' \
	  'make test            Host unit tests (sanitizers on), the Lapy client tests, tooling tests' \
	  'make lint            clang-format check' \
	  'make deps            Fetch the payload SDK, PacBrew, and ps5-opengl into .deps/' \
	  'make init TITLE_ID=PPSA12345 APP_NAME="PShare"  Change the app identity' \
	  'make deploy PS5_HOST=<address>  FTP-deploy the app folder (optional, dev only)' \
	  'make clean / distclean'
