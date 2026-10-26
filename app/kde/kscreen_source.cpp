// SPDX-License-Identifier: GPL-2.0-or-later

#include "kscreen_source.h"

#include "kscreen_dbus_source.h"

#if WALLPAPERX_HAVE_KSCREEN_LIBRARY
#include "kscreen_library_source.h"
#endif

#include <utility>

namespace wallpaperx {

std::unique_ptr<KScreenSource> systemKScreenSource(QDBusConnection bus)
{
#if WALLPAPERX_HAVE_KSCREEN_LIBRARY
    // Decided by the library loaded, not the one built against: the two can differ.
    if (KScreenLibrarySource::inProcess()) {
        return std::make_unique<KScreenLibrarySource>(KScreenLibrarySource::kDefaultAnswerMs);
    }
#endif
    return std::make_unique<KScreenDBusSource>(std::move(bus), QString::fromLatin1(KScreenDBusSource::kDefaultService),
                                               KScreenTimeouts{});
}

} // namespace wallpaperx
