// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QHash>
#include <QString>
#include <QTimer>
#include <QVariantMap>

namespace wallpaperx {

// The values each wallpaper last had on each screen, by connector and library path, kept in a JSON
// file: {"<connector>": {"<wallpaper path>": {"<key>": value, …}}}. Only values that differ from the
// wallpaper's defaults are held, so a pair never used and a pair left at its defaults are one state;
// and values the wallpaper no longer takes, which a later version of it may take again.
// A wallpaper missing from the library keeps its entries: no scan says for certain it was removed.
//
// Read once when built; a change is written a second after the last one, and on destruction.
class RememberedSettings {
public:
    explicit RememberedSettings(QString file);
    ~RememberedSettings();
    RememberedSettings(const RememberedSettings&) = delete;
    RememberedSettings& operator=(const RememberedSettings&) = delete;

    // Empty for a pair never used or at its defaults. Values as read from JSON: the library's
    // accept() puts them back in the types it holds.
    QVariantMap values(const QString& connector, const QString& wallpaperPath) const;
    // `changed` replaces what the pair held; empty forgets it.
    void remember(const QString& connector, const QString& wallpaperPath, const QVariantMap& changed);

private:
    void save();

    QString m_file;
    QHash<QString, QHash<QString, QVariantMap>> m_values;
    QTimer m_saveDue;
};

// $XDG_STATE_HOME/wallpaperx/screen-settings.json.
QString defaultRememberedSettingsFile();

} // namespace wallpaperx
