// SPDX-License-Identifier: GPL-2.0-or-later

#include "kscreen_library_source.h"

#include "dbus_reply.h"

#include <KScreen/Config>
#include <KScreen/ConfigMonitor>
#include <KScreen/EDID>
#include <KScreen/GetConfigOperation>
#include <KScreen/Mode>
#include <KScreen/Output>

#include <QGuiApplication>
#include <QTimer>
#include <QtGui/qguiapplication_platform.h>

#include <algorithm>
#include <cstring>

#include <dlfcn.h>
#include <wayland-client.h>

namespace wallpaperx {

namespace {

QVariantMap sizeMap(QSize size)
{
    return {{QStringLiteral("width"), size.width()}, {QStringLiteral("height"), size.height()}};
}

// The output as the D-Bus service's getConfig describes it, so one parser reads both sources and
// refuses the same contradictions.
QVariantMap describe(const KScreen::Output& output)
{
    QVariantList modes;
    for (const KScreen::ModePtr& mode : output.modes()) {
        modes.append(QVariantMap{{QStringLiteral("id"), mode->id()},
                                 {QStringLiteral("size"), sizeMap(mode->size())},
                                 {QStringLiteral("refreshRate"), double(mode->refreshRate())}});
    }
    return {{QStringLiteral("id"), output.id()},
            {QStringLiteral("name"), output.name()},
            {QStringLiteral("pos"), QVariantMap{{QStringLiteral("x"), output.pos().x()}, {QStringLiteral("y"), output.pos().y()}}},
            {QStringLiteral("scale"), double(output.scale())},
            {QStringLiteral("size"), sizeMap(output.size())},
            {QStringLiteral("rotation"), int(output.rotation())},
            {QStringLiteral("currentModeId"), output.currentModeId()},
            {QStringLiteral("connected"), output.isConnected()},
            {QStringLiteral("enabled"), output.isEnabled()},
            {QStringLiteral("priority"), qlonglong(output.priority())},
            {QStringLiteral("sizeMM"), sizeMap(output.sizeMm())},
            {QStringLiteral("replicationSource"), output.replicationSource()},
            {QStringLiteral("modes"), modes}};
}

// libkscreen 6.8 lists outputs only through KWin's output-device registry; under a compositor
// without it the read answers with no screens at all. Revisit if libkscreen gains another way.
constexpr const char* kOutputDeviceRegistry = "kde_output_device_registry_v2";

// Whether the compositor announces `interface`. On a queue of its own, so nothing meant for Qt is
// dispatched here. Blocks until the compositor answers, as libkscreen's own first read does.
bool compositorOffers(wl_display* display, const char* interface)
{
    struct Search {
        const char* interface;
        bool found = false;
    } search{interface};
    static const wl_registry_listener listener{
        [](void* data, wl_registry*, uint32_t, const char* announced, uint32_t) {
            auto* s = static_cast<Search*>(data);
            s->found = s->found || std::strcmp(announced, s->interface) == 0;
        },
        [](void*, wl_registry*, uint32_t) {},
    };
    wl_event_queue* queue = wl_display_create_queue(display);
    auto* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
    if (queue == nullptr || wrapper == nullptr) {
        if (wrapper != nullptr) {
            wl_proxy_wrapper_destroy(wrapper);
        }
        if (queue != nullptr) {
            wl_event_queue_destroy(queue);
        }
        return false;
    }
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), queue);
    wl_registry* registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    wl_registry_add_listener(registry, &listener, &search);
    const bool answered = wl_display_roundtrip_queue(display, queue) >= 0;
    wl_registry_destroy(registry);
    wl_event_queue_destroy(queue);
    return answered && search.found;
}

std::optional<QString> refusal()
{
    const auto* wayland = qGuiApp != nullptr ? qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>() : nullptr;
    if (wayland == nullptr) {
        return QStringLiteral("Monitors can only be listed in a Wayland session.");
    }
    if (!compositorOffers(wayland->display(), kOutputDeviceRegistry)) {
        return QStringLiteral("Monitors can only be listed under KDE Plasma.");
    }
    return std::nullopt;
}

} // namespace

std::expected<KScreenLibraryAnswer, QString> parseKScreenOutputs(const KScreen::OutputList& outputs)
{
    QVariantList described;
    for (const KScreen::OutputPtr& output : outputs) {
        described.append(describe(*output));
    }
    auto monitors = parseKScreenConfig({{QStringLiteral("outputs"), described}});
    if (!monitors) {
        return std::unexpected(monitors.error());
    }
    KScreenLibraryAnswer answer{std::move(*monitors), {}};
    for (const Monitor& monitor : answer.monitors) {
        // The parser refused the reply if two outputs share an id.
        const auto output = std::ranges::find_if(outputs, [&](const KScreen::OutputPtr& candidate) {
            return candidate->id() == monitor.kscreenId;
        });
        if (output == outputs.end() || !(*output)->edid() || !monitor.connected) {
            continue;
        }
        answer.names.append({monitor.kscreenId, edidMonitorName((*output)->edid()->rawData())});
    }
    return answer;
}

bool KScreenLibrarySource::inProcess()
{
    // BackendManager::method() is the switch between a backend in this process and one in KScreen's
    // launcher. Every libkscreen that has the launcher exports it; 6.8 removed both together, and
    // nothing public says which kind is loaded.
    return ::dlsym(RTLD_DEFAULT, "_ZNK7KScreen14BackendManager6methodEv") == nullptr;
}

KScreenLibrarySource::KScreenLibrarySource(int answerMs, QObject* parent)
    : KScreenSource(parent)
    , m_refusal(refusal())
{
    Q_ASSERT(inProcess());
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(answerMs);
    // The read stays current: the operation cannot be cancelled, and its answer, when it comes, is
    // still the latest one.
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        emit unavailable(QStringLiteral("KDE's display configuration library (KScreen) did not answer."));
    });
    if (!m_refusal) {
        connect(KScreen::ConfigMonitor::instance(), &KScreen::ConfigMonitor::configurationChanged, this,
                &KScreenSource::changed);
    }
}

void KScreenLibrarySource::read()
{
    const quint64 generation = ++m_generation;
    if (m_refusal) {
        // Reported later, as a read's end always is.
        QTimer::singleShot(0, this, [this, generation] {
            if (generation == m_generation) {
                emit unavailable(*m_refusal);
            }
        });
        return;
    }
    m_deadline.start();
    // Deletes itself once finished.
    auto* operation = new KScreen::GetConfigOperation();
    connect(operation, &KScreen::ConfigOperation::finished, this, [this, generation](KScreen::ConfigOperation* done) {
        if (generation != m_generation) {
            return;
        }
        m_deadline.stop();
        const KScreen::ConfigPtr config = done->config();
        if (done->hasError() || !config) {
            const QString why = clamped(done->errorString());
            emit unavailable(QStringLiteral("KDE's display configuration library (KScreen) could not read the screens.")
                             + (why.isEmpty() ? QString() : QLatin1Char(' ') + why));
            return;
        }
        const auto answer = parseKScreenOutputs(config->outputs());
        if (!answer) {
            emit unavailable(answer.error());
            return;
        }
        emit answered(answer->monitors);
        // A slot may have started another read, which this one's names must not reach.
        for (const KScreenModelName& name : answer->names) {
            if (generation != m_generation) {
                return;
            }
            emit named(name.kscreenId, name.modelName);
        }
    });
}

} // namespace wallpaperx
