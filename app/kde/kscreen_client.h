// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "kscreen_config.h"
#include "kscreen_source.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <optional>

namespace wallpaperx {

// The monitors KDE's display configuration reports, and which one the user picked. Where the list
// comes from is the source's business (kscreen_source.h); everything here is the same for each.
class KScreenClient : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application with the source it should read.")
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY stateChanged)
    Q_PROPERTY(QList<wallpaperx::Monitor> monitors READ monitors NOTIFY monitorsChanged)
    Q_PROPERTY(QString selectedOutput READ selectedOutput NOTIFY selectionChanged)
    Q_PROPERTY(wallpaperx::Monitor selectedMonitor READ selectedMonitor NOTIFY selectionChanged)

public:
    // Loading: no answer yet, or re-reading after what was shown stopped describing anything.
    // Ready: `monitors` is the source's latest answer. Unavailable: the last read failed, `monitors`
    // is empty, `unavailableReason` says why, and nothing is read until refresh() or the source
    // reports its service back; a read that ran past its limit may still answer.
    enum class State { Loading, Ready, Unavailable };
    Q_ENUM(State)

    // Reads `source`, which becomes this client's child.
    explicit KScreenClient(std::unique_ptr<KScreenSource> source, QObject* parent = nullptr);

    State state() const { return m_state; }
    QString unavailableReason() const { return m_unavailableReason; }
    QList<Monitor> monitors() const { return m_state == State::Ready ? m_answer : QList<Monitor>{}; }
    // Empty when nothing is selected. Always names a selectable monitor in `monitors`, so it is
    // empty while not Ready even if a choice is remembered.
    QString selectedOutput() const { return selectedMonitor().outputName; }
    Monitor selectedMonitor() const;

    // Reads again. A client already Ready stays Ready, showing the previous answer, until the new
    // one arrives; any answer to an earlier refresh is then discarded.
    Q_INVOKABLE void refresh();
    // False, changing nothing, unless `outputName` is a selectable monitor.
    Q_INVOKABLE bool select(const QString& outputName);

signals:
    void stateChanged();
    void monitorsChanged();
    void selectionChanged();

private:
    // The writers of state and reason, settle() for Loading or Unavailable and settleReady() with
    // an answer, so a selection never outlives its monitor in a Ready answer.
    void settle(State state, QString reason);
    void settleReady(QList<Monitor> answer);
    void rename(int kscreenId, const QString& modelName);
    struct Shown {
        State state;
        QString reason;
        QList<Monitor> monitors;
        Monitor selection;
    };
    Shown shown() const { return {m_state, m_unavailableReason, monitors(), selectedMonitor()}; }
    // After a write: applies the selection rules, and announces what differs from `before`.
    void settled(const Shown& before);

    // A child, so it and its pending reads go with this client.
    KScreenSource* m_source;
    State m_state = State::Loading;
    QString m_unavailableReason;
    // The latest Ready answer with the model names learned since. Kept, hidden by monitors(),
    // while not Ready, so the next answer can carry those names instead of showing connector names.
    QList<Monitor> m_answer;
    // The user's choice; read it only through selectedMonitor(), which hides it while `monitors`
    // does not list it as selectable. nullopt until anything is selected: only then may the sole
    // selectable screen be chosen for the user. Empty once a Ready answer lost it, which blocks
    // that until the user picks again, since Apply acts on whatever this names. A failed or
    // pending read is not an answer and keeps it.
    std::optional<QString> m_selection;
};

} // namespace wallpaperx
