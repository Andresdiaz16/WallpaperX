// SPDX-License-Identifier: GPL-2.0-or-later

#include "kscreen_client.h"

#include <algorithm>
#include <utility>

namespace wallpaperx {

namespace {

// A model name read from an EDID is kept for the same output only while the same panel is
// attached, so a re-read does not fall back to connector names until the source names it again.
void keepKnownNames(QList<Monitor>& fresh, const QList<Monitor>& known)
{
    for (Monitor& monitor : fresh) {
        const auto was = std::ranges::find(known, monitor.outputName, &Monitor::outputName);
        if (was != known.end() && !monitor.panel.isEmpty() && was->panel == monitor.panel) {
            monitor.displayName = was->displayName;
        }
    }
}

} // namespace

KScreenClient::KScreenClient(std::unique_ptr<KScreenSource> source, QObject* parent)
    : QObject(parent)
    , m_source(source.release())
{
    Q_ASSERT(m_source != nullptr);
    m_source->setParent(this);
    connect(m_source, &KScreenSource::loading, this, [this] { settle(State::Loading, {}); });
    connect(m_source, &KScreenSource::unavailable, this, [this](const QString& reason) { settle(State::Unavailable, reason); });
    connect(m_source, &KScreenSource::answered, this, &KScreenClient::settleReady);
    connect(m_source, &KScreenSource::named, this, &KScreenClient::rename);
    connect(m_source, &KScreenSource::changed, this, &KScreenClient::refresh);
    // Only an Unavailable client has no read running; any other will read what came back.
    connect(m_source, &KScreenSource::returned, this, [this] {
        if (m_state == State::Unavailable) {
            refresh();
        }
    });
}

Monitor KScreenClient::selectedMonitor() const
{
    if (!m_selection || m_state != State::Ready) {
        return {};
    }
    const auto it = std::ranges::find(m_answer, *m_selection, &Monitor::outputName);
    return (it == m_answer.end() || !it->selectable()) ? Monitor{} : *it;
}

void KScreenClient::refresh()
{
    if (m_state == State::Unavailable) {
        settle(State::Loading, {});
    }
    m_source->read();
}

void KScreenClient::settle(State state, QString reason)
{
    Q_ASSERT(state != State::Ready);
    const Shown before = shown();
    m_state = state;
    m_unavailableReason = std::move(reason);
    settled(before);
}

void KScreenClient::settleReady(QList<Monitor> answer)
{
    const Shown before = shown();
    keepKnownNames(answer, m_answer);
    m_state = State::Ready;
    m_unavailableReason.clear();
    m_answer = std::move(answer);
    settled(before);
}

void KScreenClient::rename(int kscreenId, const QString& modelName)
{
    const auto it = std::ranges::find(m_answer, kscreenId, &Monitor::kscreenId);
    if (it == m_answer.end()) {
        return;
    }
    const QString& shown = modelName.isEmpty() ? it->outputName : modelName;
    if (it->displayName == shown) {
        return;
    }
    it->displayName = shown;
    emit monitorsChanged();
    if (it->outputName == selectedOutput()) {
        emit selectionChanged();
    }
}

void KScreenClient::settled(const Shown& before)
{
    // Only an answer can say a monitor is gone: Loading and Unavailable carry no list, and the
    // accessors hide a remembered choice until a Ready answer lists it again.
    if (m_state == State::Ready) {
        if (m_selection) {
            if (selectedMonitor().outputName.isEmpty()) {
                m_selection->clear();
            }
        } else if (std::ranges::count_if(m_answer, &Monitor::selectable) == 1) {
            m_selection = std::ranges::find_if(m_answer, &Monitor::selectable)->outputName;
        }
    }

    // Emitted after every field is written, so no handler sees a state paired with the
    // previous answer's monitors.
    if (m_state != before.state || m_unavailableReason != before.reason) {
        emit stateChanged();
    }
    if (monitors() != before.monitors) {
        emit monitorsChanged();
    }
    if (selectedMonitor() != before.selection) {
        emit selectionChanged();
    }
}

bool KScreenClient::select(const QString& outputName)
{
    const QList<Monitor> shownMonitors = monitors();
    const auto it = std::ranges::find(shownMonitors, outputName, &Monitor::outputName);
    if (it == shownMonitors.end() || !it->selectable()) {
        return false;
    }
    if (selectedOutput() != outputName) {
        m_selection = outputName;
        emit selectionChanged();
    }
    return true;
}

} // namespace wallpaperx
