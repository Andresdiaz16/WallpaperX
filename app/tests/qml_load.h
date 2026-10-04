// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Test-only: why a QML test's component gave no object, reported against whoever owns the cause.

#include "tests/test_report.h"

#include <QList>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QRegularExpression>
#include <QStringList>
#include <QVariantMap>

#include <memory>

namespace wallpaperx::apptest {

// Every item below `item` that inherits `className`, depth first in tree order, shown or not: a
// caller that wants only visible ones filters, so each file's visibility rule stays in sight.
inline QList<QQuickItem*> allInheriting(QQuickItem* item, const char* className) {
    QList<QQuickItem*> out;
    for (QQuickItem* child : item->childItems()) {
        if (child->inherits(className)) {
            out.append(child);
        }
        out += allInheriting(child, className);
    }
    return out;
}

// A Qt module missing from this install is the environment's fault and an initial property the
// file does not have is the test's; both are harness faults. Anything else is a failure of `file`,
// the production QML compiled unchanged into the test's module. Only Qt's own modules count as the
// environment, so a WallpaperX import the file gets wrong still blames the file. A Qt that rewords
// the module message falls back to blaming the file: red either way, never a pass.
//
// Qt names no unknown initial property, only the required one it left unset, so the type is asked:
// beginCreate() makes an instance without enforcing required properties, whose meta-object lists
// the properties the file declares.
inline void reportLoadFailure(const QQmlComponent& component, const QVariantMap& given, const QString& file) {
    static const QRegularExpression missingModule(
        QString::fromLatin1(R"re(module "(Qt[\w.]*)" (?:version \S+ )?(?:is not installed|plugin "[^"]*" not found))re"));
    const std::string errors = component.errorString().trimmed().toStdString();
    QStringList modules;
    for (const QQmlError& error : component.errors()) {
        if (const QRegularExpressionMatch match = missingModule.match(error.description()); match.hasMatch()) {
            modules.append(match.captured(1));
        }
    }
    QStringList unknown;
    // A fresh component: a failed creation leaves `component` in its Error state.
    QQmlComponent type(component.engine(), component.url());
    if (modules.isEmpty() && type.isReady()) {
        if (std::unique_ptr<QObject> probe{type.beginCreate(component.engine()->rootContext())}) {
            for (auto key = given.keyBegin(); key != given.keyEnd(); ++key) {
                if (probe->metaObject()->indexOfProperty(key->toUtf8().constData()) < 0) {
                    unknown.append(*key);
                }
            }
            type.completeCreate();
        }
    }
    if (!modules.isEmpty()) {
        testing::harnessFailure(file.toStdString() + " needs " + modules.join(QStringLiteral(", ")).toStdString() +
                                ", which this Qt install lacks: " + errors);
    } else if (!unknown.isEmpty()) {
        testing::harnessFailure("the test passed " + unknown.join(QStringLiteral(", ")).toStdString() + ", which " + file.toStdString() +
                                " does not have: " + errors);
    } else {
        testing::check(false, (file.toStdString() + " did not load: " + errors).c_str());
    }
}

} // namespace wallpaperx::apptest
