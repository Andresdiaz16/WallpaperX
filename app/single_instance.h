// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QByteArray>
#include <QDBusConnection>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <optional>

namespace wallpaperx {

// The desktop file's id, which is what org.freedesktop.Application names an app by.
inline constexpr const char* kApplicationBusName = "com.wallpaperx.app";
inline constexpr const char* kApplicationObjectPath = "/com/wallpaperx/app";

// The running app, as org.freedesktop.Application: a second launch hands it the files it was
// given and exits, rather than opening a second window over the same library.
class ApplicationService : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Application")

public:
    // `paths` empty: only come forward. `activationToken`: what the launcher gave the second
    // instance to raise a window with, or empty.
    using Handler = std::function<void(const QStringList& paths, const QByteArray& activationToken)>;

    using QObject::QObject;
    void setHandler(Handler handler) { m_handler = std::move(handler); }

public slots:
    void Activate(const QVariantMap& platformData);
    void Open(const QStringList& uris, const QVariantMap& platformData);
    // The app has no actions; it comes forward.
    void ActivateAction(const QString& name, const QVariantList& parameter, const QVariantMap& platformData);

private:
    Handler m_handler;
};

// A path as a file URI and back, byte for byte: a D-Bus string must be UTF-8, a path need not be.
QString fileUri(const QByteArray& path);
// Nothing for a URI that is not a file on this computer (localPathOfFileUrl).
std::optional<QString> pathOfFileUri(const QString& uri);

struct Claim {
    enum class Result {
        // This process holds the name and serves `service`.
        First,
        // Another instance holds it and took the files; this one should exit.
        HandedOver,
        // It could not be reached; `problem` says why.
        HandOverFailed,
        // No session bus, or the name cannot be had: run as though alone.
        Alone,
    };
    Result result = Result::Alone;
    QString problem;
};

// Serves `service` on `bus` under kApplicationBusName, or, when another instance already does,
// hands it `paths` (absolute, raw bytes).
Claim claimOrHandOver(QDBusConnection bus, ApplicationService& service, const QList<QByteArray>& paths);

} // namespace wallpaperx
