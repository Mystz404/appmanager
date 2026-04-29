QT       += core gui network

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17

win32: LIBS += -lversion
VERSION = 1.0.3.20
win32: RC_ICONS += resources/appmanager.ico

DEFINES += APP_VERSION=\\\"$${VERSION}\\\"
DESTDIR = AppManager
OBJECTS_DIR = obj_build
MOC_DIR = moc_build
UI_DIR = ui_build
RCC_DIR = rcc_build
# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    appmanagerservice.cpp \
    docbrowserpage.cpp \
    logindialog.cpp \
    main.cpp \
    mainwindow.cpp \
    versionutils.cpp

HEADERS += \
    appmanagerservice.h \
    apptypes.h \
    docclienttypes.h \
    docbrowserpage.h \
    logindialog.h \
    mainwindow.h \
    refreshbuttonutils.h \
    versionutils.h

FORMS += \
    mainwindow.ui

RESOURCES += \
    appmanager.qrc

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
