// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "kscreen_source.h"

#include <KScreen/Types>

#include <QTimer>

#include <optional>

namespace wallpaperx {

// A model name learned for one monitor, as KScreenSource::named() carries it.
struct KScreenModelName {
    int kscreenId = -1;
    QString modelName;

    friend bool operator==(const KScreenModelName&, const KScreenModelName&) = default;
};

// What one library read hands on: `monitors` as answered() carries them, each displayName the
// connector name, then `names` in monitor order as named() carries them.
struct KScreenLibraryAnswer {
    QList<Monitor> monitors;
    QList<KScreenModelName> names;
};

// Reads libkscreen's outputs through parseKScreenConfig, as getConfig's map, so both sources refuse
// the same contradictions. A connected output with an EDID gets a name entry: the EDID's model name,
// empty when the EDID has none. Outputs must not be null; the map's keys are ignored.
std::expected<KScreenLibraryAnswer, QString> parseKScreenOutputs(const KScreen::OutputList& outputs);

// libkscreen in this process. From Plasma 6.8 its backend is built in and talks to the compositor
// over the app's own Wayland connection, and there is no D-Bus service to ask instead.
class KScreenLibrarySource final : public KScreenSource {
    Q_OBJECT

public:
    // Whether the libkscreen loaded can run its backend only in this process, as 6.8 and later do.
    // Older ones run it here too except on X11, where they start KScreen's launcher; they come with
    // that D-Bus service, which is read instead, so nothing constructs this source for them.
    static bool inProcess();

    // How long a read shows Loading before the picker says the library is not answering. It waits
    // only on the compositor, never on a service starting; unmeasured on 6.8, so D-Bus's default
    // call limit. A late answer is still shown, so a lower figure costs only a brief Unavailable.
    static constexpr int kDefaultAnswerMs = 25000;

    // Asks the compositor once, here, whether it can be read; needs the QGuiApplication. A read
    // that has not ended `answerMs` after it began reports unavailable(), then still reports its
    // own end when it comes unless a newer read() superseded it.
    explicit KScreenLibrarySource(int answerMs, QObject* parent = nullptr);

    void read() override;

private:
    // Why no read can list the screens; nullopt when the compositor offers what libkscreen needs.
    const std::optional<QString> m_refusal;
    quint64 m_generation = 0;
    // Runs while the latest read is outstanding.
    QTimer m_deadline;
};

} // namespace wallpaperx
