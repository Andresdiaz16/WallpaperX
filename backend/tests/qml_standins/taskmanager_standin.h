// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// A stand-in for the parts of Plasma's org.kde.taskmanager QML module that ScreenWindows.qml uses,
// so a test can load that file with no compositor and no Plasma session.
//
// It does not model Plasma's filtering. The TasksModel serves exactly the rows a test gives it and
// records what the file asked it to filter by; whether Plasma's model then filters correctly is
// Plasma's to grade. Only a test's import path reaches this module -- it is never installed.

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QQmlEngine>
#include <QRect>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <vector>

class AbstractTasksModel : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("roles only")

public:
    enum AdditionalRoles { IsActive = Qt::UserRole + 1, IsMaximized, IsFullScreen, ScreenGeometry };
    Q_ENUM(AdditionalRoles)
};

class TasksModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(GroupMode groupMode MEMBER m_groupMode NOTIFY configChanged)
    Q_PROPERTY(bool filterByScreen MEMBER m_filterByScreen NOTIFY configChanged)
    Q_PROPERTY(QRect screenGeometry MEMBER m_screenGeometry NOTIFY configChanged)
    Q_PROPERTY(bool filterByVirtualDesktop MEMBER m_filterByVirtualDesktop NOTIFY configChanged)
    // Present, as on Plasma 6.7 and later, where ScreenWindows.qml switches to it.
    Q_PROPERTY(bool filterByCurrentVirtualDesktop MEMBER m_filterByCurrentVirtualDesktop NOTIFY configChanged)
    Q_PROPERTY(QVariant virtualDesktop MEMBER m_virtualDesktop NOTIFY configChanged)
    Q_PROPERTY(bool filterByActivity MEMBER m_filterByActivity NOTIFY configChanged)
    Q_PROPERTY(QString activity MEMBER m_activity NOTIFY configChanged)
    Q_PROPERTY(bool filterMinimized MEMBER m_filterMinimized NOTIFY configChanged)

public:
    enum GroupMode { GroupDisabled = 0, GroupApplications };
    Q_ENUM(GroupMode)

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return static_cast<int>(m_windows.size()); }

    // The test's side. A window is a map of `active`, `maximized`, `fullScreen` (booleans) and
    // `geometry` (a rect; an empty one is a window the model cannot place yet). Each call emits the
    // one signal its name says, so a test can tell which of the file's handlers recounted.
    Q_INVOKABLE void resetWindows(const QVariantList& windows);         // modelReset
    Q_INVOKABLE void appendWindow(const QVariantMap& window);           // rowsInserted
    Q_INVOKABLE void removeWindow(int row);                             // rowsRemoved
    Q_INVOKABLE void changeWindow(int row, const QVariantMap& window);  // dataChanged, any role
    // dataChanged naming `roles`, as Plasma's model names the roles a change touched.
    Q_INVOKABLE void changeWindowRoles(int row, const QVariantMap& window, const QList<int>& roles);
    // Changes a row and then announces a layout change rather than a data change, as a filter that
    // re-sorts after a window moved does.
    Q_INVOKABLE void changeWindowByLayout(int row, const QVariantMap& window);  // layoutChanged

signals:
    void countChanged();
    void configChanged();

private:
    struct Window {
        bool active = false;
        bool maximized = false;
        bool fullScreen = false;
        QRect geometry;
    };
    static Window windowFrom(const QVariantMap& map);

    std::vector<Window> m_windows;
    // Plasma's own default, so a file that forgets to ask for no grouping is seen grouping.
    GroupMode m_groupMode = GroupApplications;
    bool m_filterByScreen = false;
    QRect m_screenGeometry;
    bool m_filterByVirtualDesktop = false;
    bool m_filterByCurrentVirtualDesktop = false;
    QVariant m_virtualDesktop;
    bool m_filterByActivity = false;
    QString m_activity;
    bool m_filterMinimized = false;
};

class VirtualDesktopInfo : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariant currentDesktop MEMBER m_currentDesktop NOTIFY currentDesktopChanged)

signals:
    void currentDesktopChanged();

private:
    QVariant m_currentDesktop = QStringLiteral("standin-desktop-1");
};

class ActivityInfo : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString currentActivity MEMBER m_currentActivity NOTIFY currentActivityChanged)

signals:
    void currentActivityChanged();

private:
    QString m_currentActivity = QStringLiteral("standin-activity-1");
};
