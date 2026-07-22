cmake_minimum_required(VERSION 3.16)
# App version shown in the boot log ("App version:"). Without this, ESP-IDF
# falls back to `git describe`, which advertises the ancient
# pre-m4-provisioning milestone tag. Keep in sync with FW_VERSION in
# main/defaults.h when bumping releases.
set(PROJECT_VER "0.7.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Project name = public repo name; also names the app binary
# (build/picpak-tesserae-client.bin).
project(picpak-tesserae-client)
