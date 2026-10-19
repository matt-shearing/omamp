QT += core
QT -= gui
CONFIG += c++17 console release
TARGET = omamp_bench
TEMPLATE = app

include(../../src/core/core.pri)

SOURCES += bench.cpp
