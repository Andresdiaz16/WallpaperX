// SPDX-License-Identifier: GPL-2.0-or-later

#include "daemon_client.h"

#include "dbus_reply.h"

#include <QDBusError>
#include <QDBusMessage>
#include <QDBusServiceWatcher>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

namespace wallpaperx {

namespace {

constexpr const char* kObjectPath = "/io/wallpaperx/Daemon";
constexpr const char* kInterface = "io.wallpaperx.Daemon";

QString notRunning()
{
    return QStringLiteral("The WallpaperX service (wallpaperx-daemon) is not running.");
}

QString describe(const QDBusError& error, const QString& method)
{
    if (isUnreachable(error)) {
        return notRunning();
    }
    if (isNoAnswer(error)) {
        return QStringLiteral("The WallpaperX service did not answer.");
    }
    switch (error.type()) {
    case QDBusError::Disconnected:
        return QStringLiteral("There is no session bus to reach the WallpaperX service on.");
    case QDBusError::UnknownMethod:
        return QStringLiteral("The WallpaperX service is older than this app: it does not know %1.").arg(method);
    case QDBusError::UnknownObject:
    case QDBusError::UnknownInterface:
        return QStringLiteral("Something other than the WallpaperX service holds its name on the bus.");
    case QDBusError::InvalidArgs:
        return QStringLiteral("The WallpaperX service refused it: %1").arg(clamped(error.message()));
    default:
        return QStringLiteral("The WallpaperX service could not do it: %1").arg(clamped(error.message()));
    }
}

std::optional<int> monitorIdOf(const QJsonValue& value)
{
    const double id = value.toDouble(-1);
    if (!value.isDouble() || id < 0 || id >= kMonitorIds || id != std::trunc(id)) {
        return std::nullopt;
    }
    return int(id);
}

std::optional<int> monitorIdOf(const QString& key)
{
    bool ok = false;
    const int id = key.toInt(&ok);
    return ok && id >= 0 && id < kMonitorIds && key == QString::number(id) ? std::optional<int>(id) : std::nullopt;
}

// A setting the daemon has just accepted proves it reports frame-rate settings.
FrameRates& written(std::optional<FrameRates>& rates)
{
    return rates ? *rates : rates.emplace();
}

bool isFrameRateChoice(int rate)
{
    return std::ranges::find(kFrameRates, rate) != std::end(kFrameRates);
}

// Nothing for a rate this app does not know, as a newer daemon may report.
std::optional<int> frameRateOf(const QJsonValue& value)
{
    const int rate = value.toInt(0);
    return isFrameRateChoice(rate) ? std::optional<int>(rate) : std::nullopt;
}

std::expected<DaemonStatus, QString> statusOf(const QDBusMessage& reply)
{
    const QVariantList args = reply.arguments();
    if (args.size() != 1 || args.front().metaType().id() != QMetaType::QString) {
        return std::unexpected(QStringLiteral("The WallpaperX service answered GetStatus with something other than text."));
    }
    return parseDaemonStatus(args.front().toString().toUtf8());
}

} // namespace

bool isPauseMode(const QString& mode)
{
    return std::ranges::any_of(kPauseModes, [&mode](const char* name) { return mode == QLatin1StringView(name); });
}

std::expected<DaemonStatus, QString> parseDaemonStatus(const QByteArray& json)
{
    const auto refuse = [](const char* what) {
        return std::unexpected(QStringLiteral("The WallpaperX service's status %1.").arg(QLatin1StringView(what)));
    };
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return refuse("is not a JSON object");
    }
    const QJsonObject root = doc.object();
    DaemonStatus status;

    // Keyed by the bytes each screen was named with; `outputs` is an escaped display form.
    const QJsonValue outputs = root.value(QStringLiteral("outputs_literal"));
    if (!outputs.isObject()) {
        return refuse("has no screen names; the service is older than this app");
    }
    const QJsonObject names = outputs.toObject();
    for (auto it = names.begin(); it != names.end(); ++it) {
        const auto id = monitorIdOf(it.value());
        if (!id) {
            return refuse("names a screen with a monitor id out of range");
        }
        status.outputs.insert(it.key(), *id);
    }

    for (const char* field : {"wallpapers", "pause"}) {
        const QJsonValue value = root.value(QLatin1StringView(field));
        if (value.isUndefined()) {
            continue;
        }
        if (!value.isObject()) {
            return refuse("has a list of monitors that is not an object");
        }
        const QJsonObject monitors = value.toObject();
        for (auto it = monitors.begin(); it != monitors.end(); ++it) {
            const auto id = monitorIdOf(it.key());
            if (!id) {
                return refuse("lists a monitor id out of range");
            }
            status.occupied.insert(*id);
            if (std::string_view(field) == "wallpapers") {
                const QJsonObject wallpaper = it.value().toObject();
                const QJsonValue path = wallpaper.value(QStringLiteral("path_literal"));
                const QVariantMap properties = wallpaper.value(QStringLiteral("properties")).toObject().toVariantMap();
                status.wallpapers.insert(
                    *id, DaemonStatus::Shown{wallpaper.value(QStringLiteral("type")).toString(), path.toString(), properties});
                const QString decoding =
                    wallpaper.value(QStringLiteral("processor_decoding")).toObject().value(QStringLiteral("state")).toString();
                // Any other state, a later daemon's included, shows nothing.
                using Share = DaemonStatus::OnProcessor::Share;
                const std::optional<Share> share = decoding == u"parts" ? std::optional(Share::Parts)
                                                   : decoding == u"all" ? std::optional(Share::All)
                                                                        : std::nullopt;
                if (share && path.isString()) {
                    status.onProcessor.insert(*id, DaemonStatus::OnProcessor{path.toString(), *share});
                }
            }
            if (std::string_view(field) == "pause") {
                const QString mode = it.value().toObject().value(QStringLiteral("mode")).toString();
                status.pauseModes.insert(*id, isPauseMode(mode) ? std::optional<QString>(mode) : std::nullopt);
            }
        }
    }

    // A setting a newer daemon wrote is null, and applied as on, as is everything a daemon older than
    // sound settings reports.
    const QJsonValue sound = root.value(QStringLiteral("sound"));
    if (!sound.isUndefined()) {
        if (!sound.isObject()) {
            return refuse("has sound settings that are not an object");
        }
        const QJsonObject settings = sound.toObject();
        status.sound = settings.value(QStringLiteral("enabled")).toBool(true);
        const QJsonValue monitors = settings.value(QStringLiteral("monitors"));
        if (!monitors.isUndefined() && !monitors.isObject()) {
            return refuse("has a list of monitors that is not an object");
        }
        const QJsonObject screens = monitors.toObject();
        for (auto it = screens.begin(); it != screens.end(); ++it) {
            const auto id = monitorIdOf(it.key());
            if (!id) {
                return refuse("lists a monitor id out of range");
            }
            status.occupied.insert(*id);
            status.screenSound.insert(*id, it.value().toObject().value(QStringLiteral("enabled")).toBool(true));
        }
    }

    const QJsonValue frameRate = root.value(QStringLiteral("frame_rate"));
    if (!frameRate.isUndefined()) {
        if (!frameRate.isObject()) {
            return refuse("has frame-rate settings that are not an object");
        }
        const QJsonObject settings = frameRate.toObject();
        FrameRates& rates = status.frameRates.emplace();
        rates.rate = frameRateOf(settings.value(QStringLiteral("rate")));
        // A newer daemon's value is null, and applied as off.
        rates.keepHigher = settings.value(QStringLiteral("keep_higher")).toBool(false);
        const QJsonValue monitors = settings.value(QStringLiteral("monitors"));
        if (!monitors.isUndefined() && !monitors.isObject()) {
            return refuse("has a list of monitors that is not an object");
        }
        const QJsonObject screens = monitors.toObject();
        for (auto it = screens.begin(); it != screens.end(); ++it) {
            const auto id = monitorIdOf(it.key());
            if (!id) {
                return refuse("lists a monitor id out of range");
            }
            status.occupied.insert(*id);
            const QJsonObject screen = it.value().toObject();
            if (!screen.value(QStringLiteral("follows_global")).toBool(true)) {
                rates.screenRates.insert(*id, frameRateOf(screen.value(QStringLiteral("rate"))));
            }
            if (const double refresh = screen.value(QStringLiteral("refresh_rate")).toDouble(0); refresh > 0) {
                rates.refreshRates.insert(*id, refresh);
            }
        }
    }

    const QJsonObject persistence = root.value(QStringLiteral("persistence")).toObject();
    status.persistenceStatus = persistence.value(QStringLiteral("status")).toString();
    status.persistenceDetail = persistence.value(QStringLiteral("detail")).toString();
    return status;
}

std::optional<int> freeMonitorId(const DaemonStatus& status)
{
    QSet<int> taken = status.occupied;
    for (const int id : status.outputs) {
        taken.insert(id);
    }
    for (int id = 0; id < kMonitorIds; ++id) {
        if (!taken.contains(id)) {
            return id;
        }
    }
    return std::nullopt;
}

DaemonClient::DaemonClient(QDBusConnection bus, QString service, DaemonTimeouts timeouts, QObject* parent)
    : QObject(parent)
    , m_bus(std::move(bus))
    , m_service(std::move(service))
    , m_timeouts(timeouts)
{
    auto* watcher = new QDBusServiceWatcher(m_service, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString&, const QString&, const QString& newOwner) {
                if (newOwner.isEmpty()) {
                    ++m_generation;
                    settle(State::Unavailable, notRunning(), {});
                } else {
                    refresh();
                    emit serviceStarted();
                }
            });
    // How a save the daemon did not wait for is heard of: its reply comes before the write.
    m_bus.connect(m_service, QString::fromLatin1(kObjectPath), QString::fromLatin1(kInterface),
                  QStringLiteral("PersistenceChanged"), this, SLOT(refresh()));
    // How a wallpaper's scan, which ends after its SetWallpaper is answered, is heard of.
    m_bus.connect(m_service, QString::fromLatin1(kObjectPath), QString::fromLatin1(kInterface),
                  QStringLiteral("ProcessorDecodingChanged"), this, SLOT(refresh()));
}

QVariantMap DaemonClient::pauseModes() const
{
    QVariantMap modes;
    for (auto it = m_status.outputs.cbegin(); it != m_status.outputs.cend(); ++it) {
        const auto mode = m_status.pauseModes.constFind(it.value());
        modes.insert(it.key(), mode == m_status.pauseModes.cend() ? QStringLiteral("play") : mode->value_or(QString()));
    }
    return modes;
}

QVariantMap DaemonClient::screenSound() const
{
    QVariantMap sound;
    for (auto it = m_status.outputs.cbegin(); it != m_status.outputs.cend(); ++it) {
        sound.insert(it.key(), m_status.screenSound.value(it.value(), true));
    }
    return sound;
}

int DaemonClient::frameRate() const
{
    return m_status.frameRates ? m_status.frameRates->rate.value_or(0) : kDefaultFrameRate;
}

QVariantMap DaemonClient::screenFrameRates() const
{
    QVariantMap rates;
    for (auto it = m_status.outputs.cbegin(); it != m_status.outputs.cend(); ++it) {
        int rate = 0;
        if (m_status.frameRates) {
            if (const auto own = m_status.frameRates->screenRates.constFind(it.value());
                own != m_status.frameRates->screenRates.cend()) {
                rate = own->value_or(-1);
            }
        }
        rates.insert(it.key(), rate);
    }
    return rates;
}

bool DaemonClient::keepHigherFrameRate() const
{
    return m_status.frameRates && m_status.frameRates->keepHigher;
}

QString DaemonClient::persistenceProblem() const
{
    if (m_status.persistenceStatus != u"fail") {
        return {};
    }
    return QStringLiteral("The WallpaperX service could not save its settings, so they may be lost when it restarts: %1")
        .arg(clamped(m_status.persistenceDetail));
}

QDBusMessage DaemonClient::method(const char* name) const
{
    return QDBusMessage::createMethodCall(m_service, QString::fromLatin1(kObjectPath), QString::fromLatin1(kInterface),
                                          QString::fromLatin1(name));
}

void DaemonClient::send(QObject* context, const QDBusMessage& message, int timeoutMs,
                        std::function<void(std::expected<QDBusMessage, QString>)> onReply)
{
    whenAnswered(context, m_bus.asyncCall(message, timeoutMs),
                 [onReply = std::move(onReply), member = message.member()](const QDBusMessage& reply) {
                     if (reply.type() == QDBusMessage::ErrorMessage) {
                         onReply(std::unexpected(describe(QDBusError(reply), member)));
                     } else {
                         onReply(reply);
                     }
                 });
}

void DaemonClient::sendAndRefresh(QObject* context, const QDBusMessage& message, Done done)
{
    send(context, message, m_timeouts.callMs, [this, done = std::move(done)](std::expected<QDBusMessage, QString> reply) {
        if (!reply) {
            done(std::unexpected(std::move(reply.error())));
            return;
        }
        refresh();
        done({});
    });
}

void DaemonClient::refresh()
{
    const quint64 generation = ++m_generation;
    if (m_state == State::Unavailable) {
        settle(State::Loading, {}, {});
    }
    send(this, method("GetStatus"), m_timeouts.callMs, [this, generation](std::expected<QDBusMessage, QString> reply) {
        if (generation != m_generation) {
            return;
        }
        settleRead(reply ? statusOf(*reply) : std::unexpected(std::move(reply.error())));
    });
}

void DaemonClient::bindMonitor(QObject* context, const QString& connector,
                               std::function<void(std::expected<int, QString>)> done)
{
    m_binds.push_back(Bind{context, connector, std::move(done)});
    if (m_binds.size() == 1) {
        bindNext();
    }
}

void DaemonClient::bindNext()
{
    while (!m_binds.empty() && m_binds.front().context.isNull()) {
        m_binds.pop_front();
    }
    if (m_binds.empty()) {
        return;
    }
    // Read fresh, never from the last refresh: the id chosen must be free now.
    const QString connector = m_binds.front().connector;
    const quint64 generation = m_generation;
    send(this, method("GetStatus"), m_timeouts.callMs, [this, connector, generation](std::expected<QDBusMessage, QString> reply) {
        auto status = reply ? statusOf(*reply) : std::unexpected(std::move(reply.error()));
        // Shown, failure included, only when no refresh was sent after it; one sent before it
        // reads older state, so its reply is then discarded.
        if (generation == m_generation) {
            ++m_generation;
            settleRead(status);
        }
        if (!status) {
            finishBind(std::unexpected(std::move(status.error())));
            return;
        }
        if (const auto known = status->outputs.constFind(connector); known != status->outputs.cend()) {
            finishBind(*known);
            return;
        }
        const auto id = freeMonitorId(*status);
        if (!id) {
            finishBind(std::unexpected(QStringLiteral("The WallpaperX service has no monitor left for %1: all %2 are in use.")
                                           .arg(connector)
                                           .arg(kMonitorIds)));
            return;
        }
        QDBusMessage bind = method("SetMonitorOutput");
        bind.setArguments({*id, connector});
        send(this, bind, m_timeouts.callMs, [this, id = *id, connector](std::expected<QDBusMessage, QString> reply) {
            if (!reply) {
                finishBind(std::unexpected(std::move(reply.error())));
                return;
            }
            if (m_state == State::Ready && !m_status.outputs.contains(connector)) {
                m_status.outputs.insert(connector, id);
                emit statusChanged();
            }
            finishBind(id);
        });
    });
}

void DaemonClient::finishBind(std::expected<int, QString> result)
{
    Bind finished = std::move(m_binds.front());
    m_binds.pop_front();
    // The next starts before this one's caller hears, so a bind that caller asks for joins the queue
    // instead of running beside it.
    if (!m_binds.empty()) {
        bindNext();
    }
    if (finished.context) {
        finished.done(std::move(result));
    }
}

void DaemonClient::setWallpaper(QObject* context, const QString& type, const QString& path, int monitorId,
                                const QVariantMap& properties, Done done)
{
    QDBusMessage message = method("SetWallpaperWithProperties");
    message.setArguments({type, path, monitorId, properties});
    sendAndRefresh(context, message, std::move(done));
}

void DaemonClient::setProperties(QObject* context, int monitorId, const QVariantMap& properties, Done done)
{
    QDBusMessage message = method("SetWallpaperProperties");
    message.setArguments({monitorId, properties});
    send(context, message, m_timeouts.callMs, [done = std::move(done)](std::expected<QDBusMessage, QString> reply) {
        done(reply ? std::expected<void, QString>() : std::unexpected(std::move(reply.error())));
    });
}

void DaemonClient::setPause(QObject* context, int monitorId, const QString& mode, Done done)
{
    QDBusMessage message = method("SetMonitorPause");
    message.setArguments({monitorId, mode});
    sendSetting(context, message, [this, monitorId, mode] {
        m_status.pauseModes.insert(monitorId, mode);
        m_status.occupied.insert(monitorId);
    }, std::move(done));
}

void DaemonClient::setSound(QObject* context, bool on, Done done)
{
    QDBusMessage message = method("SetSound");
    message.setArguments({on});
    sendSetting(context, message, [this, on] { m_status.sound = on; }, std::move(done));
}

void DaemonClient::setScreenSound(QObject* context, int monitorId, bool on, Done done)
{
    QDBusMessage message = method("SetMonitorSound");
    message.setArguments({monitorId, on});
    sendSetting(context, message, [this, monitorId, on] {
        m_status.screenSound.insert(monitorId, on);
        m_status.occupied.insert(monitorId);
    }, std::move(done));
}

void DaemonClient::setFrameRate(QObject* context, int rate, Done done)
{
    QDBusMessage message = method("SetFrameRate");
    message.setArguments({rate});
    sendSetting(context, message, [this, rate] { written(m_status.frameRates).rate = rate; }, std::move(done));
}

void DaemonClient::setScreenFrameRate(QObject* context, int monitorId, int rate, Done done)
{
    QDBusMessage message = method("SetMonitorFrameRate");
    message.setArguments({monitorId, rate});
    sendSetting(context, message, [this, monitorId, rate] {
        FrameRates& rates = written(m_status.frameRates);
        if (rate == 0) {
            rates.screenRates.remove(monitorId);
        } else {
            rates.screenRates.insert(monitorId, rate);
        }
        m_status.occupied.insert(monitorId);
    }, std::move(done));
}

void DaemonClient::setKeepHigherFrameRate(QObject* context, bool keep, Done done)
{
    QDBusMessage message = method("SetKeepHigherFrameRate");
    message.setArguments({keep});
    sendSetting(context, message, [this, keep] { written(m_status.frameRates).keepHigher = keep; },
                std::move(done));
}

void DaemonClient::setOutputRefreshRate(QObject* context, const QString& connector, double rate, Done done)
{
    QDBusMessage message = method("SetOutputRefreshRate");
    message.setArguments({connector, rate});
    sendSetting(context, message, [this, connector, rate] {
        const auto id = m_status.outputs.constFind(connector);
        if (id == m_status.outputs.cend()) {
            return;
        }
        FrameRates& rates = written(m_status.frameRates);
        if (rate == 0) {
            rates.refreshRates.remove(*id);
        } else {
            rates.refreshRates.insert(*id, rate);
        }
    }, std::move(done));
}

void DaemonClient::sendSetting(QObject* context, const QDBusMessage& message, std::function<void()> record, Done done)
{
    send(context, message, m_timeouts.callMs,
         [this, record = std::move(record), done = std::move(done)](std::expected<QDBusMessage, QString> reply) {
             if (!reply) {
                 done(std::unexpected(std::move(reply.error())));
                 return;
             }
             // The reply is the daemon saying so, and the screen shows it before the re-read lands.
             if (m_state == State::Ready) {
                 record();
                 emit statusChanged();
             }
             refresh();
             done({});
         });
}

void DaemonClient::settle(State state, QString reason, DaemonStatus status)
{
    const bool changes = m_state != state || m_unavailableReason != reason || m_status != status;
    m_state = state;
    m_unavailableReason = std::move(reason);
    m_status = std::move(status);
    if (changes) {
        emit statusChanged();
    }
}

void DaemonClient::settleRead(std::expected<DaemonStatus, QString> status)
{
    if (status) {
        settle(State::Ready, {}, std::move(*status));
        emit statusRead();
    } else {
        settle(State::Unavailable, std::move(status.error()), {});
    }
}

} // namespace wallpaperx
