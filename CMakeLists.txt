cmake_minimum_required(VERSION 3.16)
# Nomor versi program. GitHub Actions mengisinya dengan nomor build (-DPROJECT_VER=...).
# Build lokal (PlatformIO) otomatis bernomor 0, jadi OTA menganggap GitHub lebih baru.
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "0")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Repoversiempat)
