// SPDX-License-Identifier: GPL-2.0-or-later

// Entry point for tests/qml_windows/tst_screen_windows.qml, which drives the package's
// ScreenWindows.qml -- how the plugin works out whether a window on its screen is focused or
// maximized -- against stand-ins for Plasma's task manager and KWindowSystem modules
// (tests/qml_standins/), put first on the import path here. Offscreen: no compositor, no Plasma.

#include <QtQuickTest/quicktest.h>

#include <QObject>
#include <QQmlEngine>
#include <QString>

namespace {

class Setup : public QObject {
    Q_OBJECT

public slots:
    // Prepended, so the stand-ins win over whatever the host has installed.
    void qmlEngineAvailable(QQmlEngine* engine) { engine->addImportPath(QStringLiteral(WALLPAPERX_STANDIN_QML_PATH)); }
};

}  // namespace

QUICK_TEST_MAIN_WITH_SETUP(wallpaperx_qml_screen_windows, Setup)

#include "qml_screen_windows_test.moc"
