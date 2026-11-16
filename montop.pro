QT += core gui qml quick quickcontrols2

CONFIG += c++17 release
TARGET = montop
TEMPLATE = app

# The one place the version is set; a release also sets pkgver in the PKGBUILDs.
# It reaches the code through a generated header rather than a define, so
# changing it rebuilds what uses it.
VERSION = 0.5.1
VERSION_HEADER = "$${LITERAL_HASH}pragma once" "$${LITERAL_HASH}define MONTOP_VERSION \"$$VERSION\""
write_file($$OUT_PWD/version.h, VERSION_HEADER)
INCLUDEPATH += $$OUT_PWD $$PWD/src

HEADERS += \
    src/apps.h \
    src/cli.h \
    src/gpu.h \
    src/procfs.h \
    src/run.h \
    src/monitor.h \
    src/processmodel.h \
    src/qmlsingletons.h \
    src/sparkline.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/apps.cpp \
    src/cli.cpp \
    src/gpu.cpp \
    src/run.cpp \
    src/procfs.cpp \
    src/monitor.cpp \
    src/processmodel.cpp \
    src/sparkline.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc

# The C++ types QML uses (QML_ELEMENT) form the module Montop. Its type
# description and qmldir land in build/qml/Montop, where qmllint finds them:
# bin/lint.
CONFIG += qmltypes
QML_IMPORT_NAME = Montop
QML_IMPORT_MAJOR_VERSION = 1
QMLTYPES_FILENAME = $$OUT_PWD/qml/Montop/montop.qmltypes
# qmltypes.prf looks for Qt's metatypes by build type
# (qt6core_release_metatypes.json); Arch installs them without one. Without
# them qmltyperegistrar can't tell qmllint that qreal is a number.
for(module, $$list(core gui qml quick)) {
    metatypes = $$[QT_INSTALL_ARCHDATA]/metatypes/qt6$${module}_metatypes.json
    exists($$metatypes): QML_FOREIGN_METATYPES += $$metatypes
}
QMLDIR = "module Montop" "typeinfo montop.qmltypes" "depends QtQuick"
write_file($$OUT_PWD/qml/Montop/qmldir, QMLDIR)
