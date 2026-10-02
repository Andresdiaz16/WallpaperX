// SPDX-License-Identifier: GPL-2.0-or-later

#include "taskmanager_standin.h"

int TasksModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : count(); }

QVariant TasksModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) return {};
    const Window& window = m_windows[static_cast<size_t>(index.row())];
    switch (role) {
        case AbstractTasksModel::IsActive:
            return window.active;
        case AbstractTasksModel::IsMaximized:
            return window.maximized;
        case AbstractTasksModel::IsFullScreen:
            return window.fullScreen;
        case AbstractTasksModel::ScreenGeometry:
            return window.geometry;
        default:
            return {};
    }
}

QHash<int, QByteArray> TasksModel::roleNames() const {
    return {{AbstractTasksModel::IsActive, "IsActive"},
            {AbstractTasksModel::IsMaximized, "IsMaximized"},
            {AbstractTasksModel::IsFullScreen, "IsFullScreen"},
            {AbstractTasksModel::ScreenGeometry, "ScreenGeometry"}};
}

TasksModel::Window TasksModel::windowFrom(const QVariantMap& map) {
    Window window;
    window.active = map.value(QStringLiteral("active")).toBool();
    window.maximized = map.value(QStringLiteral("maximized")).toBool();
    window.fullScreen = map.value(QStringLiteral("fullScreen")).toBool();
    // Placed somewhere unless the test says otherwise: most rows are about the other roles.
    window.geometry = map.contains(QStringLiteral("geometry")) ? map.value(QStringLiteral("geometry")).toRectF().toRect()
                                                               : QRect(0, 0, 640, 480);
    return window;
}

void TasksModel::resetWindows(const QVariantList& windows) {
    beginResetModel();
    m_windows.clear();
    for (const QVariant& window : windows) m_windows.push_back(windowFrom(window.toMap()));
    endResetModel();
    emit countChanged();
}

void TasksModel::appendWindow(const QVariantMap& window) {
    beginInsertRows(QModelIndex(), count(), count());
    m_windows.push_back(windowFrom(window));
    endInsertRows();
    emit countChanged();
}

void TasksModel::removeWindow(int row) {
    if (row < 0 || row >= count()) return;
    beginRemoveRows(QModelIndex(), row, row);
    m_windows.erase(m_windows.begin() + row);
    endRemoveRows();
    emit countChanged();
}

void TasksModel::changeWindow(int row, const QVariantMap& window) {
    if (row < 0 || row >= count()) return;
    m_windows[static_cast<size_t>(row)] = windowFrom(window);
    emit dataChanged(index(row, 0), index(row, 0));
}

void TasksModel::changeWindowRoles(int row, const QVariantMap& window, const QList<int>& roles) {
    if (row < 0 || row >= count()) return;
    m_windows[static_cast<size_t>(row)] = windowFrom(window);
    emit dataChanged(index(row, 0), index(row, 0), roles);
}

void TasksModel::changeWindowByLayout(int row, const QVariantMap& window) {
    if (row < 0 || row >= count()) return;
    emit layoutAboutToBeChanged();
    m_windows[static_cast<size_t>(row)] = windowFrom(window);
    emit layoutChanged();
}
