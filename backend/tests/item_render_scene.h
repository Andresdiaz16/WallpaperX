// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The render-controlled scene the plugin's import tests draw through: the plugin's own type in a
// QQuickRenderControl window, one scene-graph pass at a time, and the lockstep drive (announce one
// picture, pass until it is drawn) their counts are read against. Shared by item_import_cache_test
// and item_timeline_test, which grade the same item on the same drive; two copies of the drive
// would be two definitions of "one picture drawn".

#include "tests/gl_import_harness.h"
#include "tests/item_stub_scene.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QOpenGLContext>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QSGTexture>
#include <QString>
#include <QtQuick/qsgtexture_platform.h>

#include <array>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

namespace wallpaperx::testing {

// The plugin's own type, with its counters BOUND the way a QML reader binds them -- which is the
// only reader they exist for, and one that is frozen at its first value without a NOTIFY.
inline QString cacheQmlSource() {
    return QStringLiteral(R"(
import QtQuick
import com.wallpaperx

Item {
    id: root
    width: 68
    height: 36
    property Item rendererItem: renderer
    property string publishedReason: renderer.unavailableReason
    property var boundImports: renderer.importCount
    property var boundReused: renderer.reusedImportCount
    property var boundSemaphores: renderer.semaphoreImportCount
    property var boundLost: renderer.lostImportCount
    property int reasonSignals: 0
    property int frameShownSignals: 0

    WallpaperXRenderer {
        id: renderer
        anchors.fill: parent
        monitorId: 0
        onUnavailableReasonChanged: root.reasonSignals++
        onFrameShownChanged: root.frameShownSignals++
    }
}
)");
}

struct Scene {
    QQuickRenderControl control;
    QQuickWindow window{&control};
    QQmlEngine engine;
    std::unique_ptr<QQmlComponent> component;
    std::unique_ptr<QObject> root;
    QQuickItem* item = nullptr;

    // One scene-graph pass. `afterFrame` runs once the frame has been rendered and before the
    // context is dropped, which is where a readback may run without disturbing Qt's own state.
    void renderPass(const std::function<void()>& afterFrame = nullptr) {
        control.polishItems();
        control.beginFrame();
        control.sync();
        control.endFrame();
        if (afterFrame) afterFrame();
        // For item_import_release_test's reason: a real compositor's GUI thread holds no context
        // between frames, and a harness that left one current would hide a free made there.
        if (QOpenGLContext* current = QOpenGLContext::currentContext()) current->doneCurrent();
    }

    quint64 imports() const { return item->property("importCount").toULongLong(); }
    quint64 reused() const { return item->property("reusedImportCount").toULongLong(); }
    quint64 pictures() const { return imports() + reused(); }
    quint64 semaphoreImports() const { return item->property("semaphoreImportCount").toULongLong(); }
    quint64 lost() const { return item->property("lostImportCount").toULongLong(); }
    QString reason() const { return root->property("publishedReason").toString(); }
};

inline GLuint nativeName(QSGTexture* texture) {
    if (texture == nullptr) return 0;
    auto* opengl = texture->nativeInterface<QNativeInterface::QSGOpenGLTexture>();
    return opengl == nullptr ? 0 : opengl->nativeTexture();
}

inline void announce(QDBusConnection& bus, quint64 generation, int monitor = kFirstMonitor) {
    QDBusMessage ready =
        QDBusMessage::createSignal(QString::fromUtf8(kPath), QString::fromUtf8(kInterface), QStringLiteral("FrameReady"));
    ready.setArguments({monitor, static_cast<qulonglong>(generation)});
    bus.send(ready);
}

// Runs passes until the item has asked for nothing new for a third of a second, which is what makes
// "the next request" mean the next one this phase causes. Needed rather than a fixed number of
// passes: the item's startup makes several forced fetches that coalesce into trailing ones, and one
// landing after a phase has started scheduling replies is served as that phase's first -- measured,
// it put a ninth failed hand-over into a run meant to stop at eight. False when the item never goes
// quiet inside the bound.
inline bool settleQuiet(Scene& scene, ExportingStub& stub) {
    int quietRounds = 0;
    int seen = stub.requests();
    QElapsedTimer clock;
    clock.start();
    while (quietRounds < 10) {
        if (clock.elapsed() > 5000) return false;
        scene.renderPass();
        pumpFor(30);
        if (stub.requests() == seen) {
            ++quietRounds;
        } else {
            seen = stub.requests();
            quietRounds = 0;
        }
    }
    return true;
}

// Announces the next picture and runs passes until the item has drawn one more. Lockstep: one
// picture asked for, one drawn, so a count read afterwards describes exactly the drive. False when
// no picture arrived in the bound, which every caller must branch on.
inline bool drawNext(Scene& scene, ExportingStub& stub, QDBusConnection& bus) {
    const quint64 before = scene.pictures();
    announce(bus, stub.nextGeneration());
    return pumpUntil(
        [&] {
            scene.renderPass();
            return scene.pictures() > before;
        },
        5000);
}

// Runs passes until `drawn()` is true, copying the item's texture after each pass into `copy` (made
// on first use): glCopyImageSubData, which GL orders after any wait the item queued in that pass and
// after nothing else. Flushed, never read here: reading would make this thread wait on the GPU, which
// may itself be waiting on a signal only this thread can send. The import's channel swizzle goes
// with the copy, since it copies raw texels. False at the bound, or when no copy could be made.
inline bool copyWhileDrawing(Scene& scene, const std::function<bool()>& drawn, GLuint& copy) {
    using CopyFn = void (*)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint,
                            GLsizei, GLsizei, GLsizei);
    const auto copyImage = reinterpret_cast<CopyFn>(eglGetProcAddress("glCopyImageSubData"));
    if (copyImage == nullptr) return false;
    const auto take = [&] {
        const GLuint source = nativeName(itemTexture(scene.item));
        if (source == 0) return;
        if (copy == 0) {
            GLint swizzle[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
            glBindTexture(GL_TEXTURE_2D, source);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
            glGenTextures(1, &copy);
            glBindTexture(GL_TEXTURE_2D, copy);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kWidth, kHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            // Complete with one level, or glCopyImageSubData refuses it.
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        copyImage(source, GL_TEXTURE_2D, 0, 0, 0, 0, copy, GL_TEXTURE_2D, 0, 0, 0, 0, kWidth, kHeight, 1);
        glFlush();
    };
    const bool done = pumpUntil(
        [&] {
            scene.renderPass(take);
            pumpFor(20);
            return drawn();
        },
        5000);
    return done && copy != 0;
}

// Reads back what copyWhileDrawing() took, and deletes it.
inline std::vector<unsigned char> readCopy(Scene& scene, GLuint& copy, std::string& why) {
    std::vector<unsigned char> pixels;
    scene.renderPass([&] {
        pixels = readSampled(copy, kWidth, kHeight, why);
        glDeleteTextures(1, &copy);
        copy = 0;
    });
    return pixels;
}

// Which of a few flat colours a pixel is, or -1: the readback is compared by nearest colour rather
// than exactly, so the check is about WHICH picture is shown and not about a driver's rounding.
inline int nearestColour(const unsigned char* pixel, const std::vector<std::array<unsigned char, 4>>& palette) {
    int best = -1;
    int bestDistance = 1 << 30;
    for (size_t i = 0; i < palette.size(); ++i) {
        int distance = 0;
        for (int c = 0; c < 3; ++c) distance += std::abs(static_cast<int>(pixel[c]) - palette[i][c]);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = static_cast<int>(i);
        }
    }
    return bestDistance <= 60 ? best : -1;
}

}  // namespace wallpaperx::testing
