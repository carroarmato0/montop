#pragma once

#include <QQmlEngine>
#include <QtQml/qqmlregistration.h>

#include "monitor.h"
#include "theme.h"
#include "translator.h"

// QML reaches the Monitor, Theme and Translator that main() creates as the
// singletons of the same name in the Montop module. They are registered
// through these foreign stand-ins, not with QML_SINGLETON on the classes
// themselves: Qt builds a default-constructible singleton class with its
// constructor and never calls its create(), and QML would get fresh, idle
// copies instead of the running ones.
namespace qmlsingletons {
template <typename T>
T *hand(T *instance, QJSEngine *engine) {
    if (!instance || engine->thread() != instance->thread())
        qFatal("A Montop singleton was used before main() set it, or from another thread");
    QJSEngine::setObjectOwnership(instance, QJSEngine::CppOwnership);
    return instance;
}
}

struct MonitorSingleton {
    Q_GADGET
    QML_FOREIGN(Monitor)
    QML_NAMED_ELEMENT(Monitor)
    QML_SINGLETON
public:
    static inline Monitor *instance = nullptr;
    static Monitor *create(QQmlEngine *, QJSEngine *engine) { return qmlsingletons::hand(instance, engine); }
};

struct ThemeSingleton {
    Q_GADGET
    QML_FOREIGN(Theme)
    QML_NAMED_ELEMENT(Theme)
    QML_SINGLETON
public:
    static inline Theme *instance = nullptr;
    static Theme *create(QQmlEngine *, QJSEngine *engine) { return qmlsingletons::hand(instance, engine); }
};

struct TranslatorSingleton {
    Q_GADGET
    QML_FOREIGN(Translator)
    QML_NAMED_ELEMENT(Translator)
    QML_SINGLETON
public:
    static inline Translator *instance = nullptr;
    static Translator *create(QQmlEngine *, QJSEngine *engine) { return qmlsingletons::hand(instance, engine); }
};
