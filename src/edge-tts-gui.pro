QT += core gui websockets multimedia core5compat

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
# DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    communicate.cpp \
    main.cpp \
    dialog.cpp

HEADERS += \
    communicate.h \
    dialog.h

FORMS += \
    dialog.ui

RC_ICONS = favicon.ico

VERSION = 0.7.0

win32: LIBS += -luser32

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

DISTFILES += \
    voice_list.tsv

RESOURCES += \
    resources.qrc
