// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// A stand-in for KDE Frameworks' org.kde.kwindowsystem QML module: the one property
// ScreenWindows.qml reads, writable so a test can turn "show desktop" on. Only a test's import path
// reaches it.

#include <QObject>
#include <QQmlEngine>

class KWindowSystemStandIn : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(KWindowSystem)
    QML_SINGLETON
    Q_PROPERTY(bool showingDesktop MEMBER m_showingDesktop NOTIFY showingDesktopChanged)

signals:
    void showingDesktopChanged();

private:
    bool m_showingDesktop = false;
};
