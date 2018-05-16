/*
 * Copyright (C) 2017 ~ 2018 Deepin Technology Co., Ltd.
 *
 * Author:     zccrs <zccrs@live.com>
 *
 * Maintainer: zccrs <zhangjide@deepin.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "daspluginloader.h"

#include <QMutex>
#include <QDir>
#include <QJsonArray>
#include <QPluginLoader>
#include <QCoreApplication>
#include <QDebug>

DAS_BEGIN_NAMESPACE

Q_GLOBAL_STATIC(QList<DASPluginLoader *>, qt_factory_loaders)

Q_GLOBAL_STATIC_WITH_ARGS(QMutex, qt_factoryloader_mutex, (QMutex::Recursive))

// avoid duplicate QStringLiteral data:
inline QString iidKeyLiteral() { return QStringLiteral("IID"); }
#ifdef QT_SHARED
inline QString versionKeyLiteral() { return QStringLiteral("version"); }
#endif
inline QString metaDataKeyLiteral() { return QStringLiteral("MetaData"); }
inline QString keysKeyLiteral() { return QStringLiteral("Keys"); }

class DASPluginLoaderPrivate
{
//    Q_DECLARE_PUBLIC(PluginLoader)
public:
    DASPluginLoaderPrivate();
    ~DASPluginLoaderPrivate();
    mutable QMutex mutex;
    QByteArray iid;
    QList<QPluginLoader*> pluginLoaderList;
    QMultiMap<QString,QPluginLoader*> keyMap;
    QString suffix;
    Qt::CaseSensitivity cs;
    bool rki;
    QStringList loadedPaths;

    static QStringList pluginPaths;
};

QStringList DASPluginLoaderPrivate::pluginPaths;

DASPluginLoaderPrivate::DASPluginLoaderPrivate()
{
    if (pluginPaths.isEmpty()) {
        if (QT_PREPEND_NAMESPACE(qEnvironmentVariableIsEmpty)("DAS_PLUGIN_PATH"))
            pluginPaths.append(QString::fromLocal8Bit(PLUGINDIR).split(':'));
        else
            pluginPaths = QString::fromLocal8Bit(qgetenv("DAS_PLUGIN_PATH")).split(':');
    }
}

DASPluginLoaderPrivate::~DASPluginLoaderPrivate()
{
    for (int i = 0; i < pluginLoaderList.count(); ++i) {
        QPluginLoader *loader = pluginLoaderList.at(i);
        loader->unload();
    }
}

DASPluginLoader::DASPluginLoader(const char *iid,
                                   const QString &suffix,
                                   Qt::CaseSensitivity cs,
                                   bool repetitiveKeyInsensitive)
    : d_ptr(new DASPluginLoaderPrivate)
{
    Q_D(DASPluginLoader);
    d->iid = iid;
    d->suffix = suffix;
    d->cs = cs;
    d->rki = repetitiveKeyInsensitive;

    QMutexLocker locker(qt_factoryloader_mutex());
    Q_UNUSED(locker)
    update();
    qt_factory_loaders()->append(this);
}

/* Internal, for debugging */
static bool dfm_debug_component()
{
#ifdef QT_DEBUG
    return true;
#endif

    static int debug_env = QT_PREPEND_NAMESPACE(qEnvironmentVariableIntValue)("DAS_DEBUG_PLUGINS");
    return debug_env != 0;
}

void DASPluginLoader::update()
{
#ifdef QT_SHARED
    Q_D(DASPluginLoader);

    const QStringList &paths = d->pluginPaths;
    for (int i = 0; i < paths.count(); ++i) {
        const QString &pluginDir = paths.at(i);
        // Already loaded, skip it...
        if (d->loadedPaths.contains(pluginDir))
            continue;
        d->loadedPaths << pluginDir;

        QString path = pluginDir + d->suffix;

        if (dfm_debug_component())
            qDebug() << "PluginLoader::PluginLoader() checking directory path" << path << "...";

        if (!QDir(path).exists(QLatin1String(".")))
            continue;

        QStringList plugins = QDir(path).entryList(QDir::Files);
        QPluginLoader *loader = 0;

#ifdef Q_OS_MAC
        // Loading both the debug and release version of the cocoa plugins causes the objective-c runtime
        // to print "duplicate class definitions" warnings. Detect if DFMFactoryLoader is about to load both,
        // skip one of them (below).
        //
        // ### FIXME find a proper solution
        //
        const bool isLoadingDebugAndReleaseCocoa = plugins.contains(QStringLiteral("libqcocoa_debug.dylib"))
                && plugins.contains(QStringLiteral("libqcocoa.dylib"));
#endif
        for (int j = 0; j < plugins.count(); ++j) {
            QString fileName = QDir::cleanPath(path + QLatin1Char('/') + plugins.at(j));

#ifdef Q_OS_MAC
            if (isLoadingDebugAndReleaseCocoa) {
#ifdef QT_DEBUG
               if (fileName.contains(QStringLiteral("libqcocoa.dylib")))
                   continue;    // Skip release plugin in debug mode
#else
               if (fileName.contains(QStringLiteral("libqcocoa_debug.dylib")))
                   continue;    // Skip debug plugin in release mode
#endif
            }
#endif
            if (dfm_debug_component()) {
                qDebug() << "PluginLoader::PluginLoader() looking at" << fileName;
            }
            loader = (new QPluginLoader(fileName));
            if (!loader->load()) {
                if (dfm_debug_component()) {
                    qDebug() << loader->errorString();
                }
                loader->deleteLater();
                continue;
            }

            QStringList keys;
            bool metaDataOk = false;

            QString iid = loader->metaData().value(iidKeyLiteral()).toString();
            if (iid == QLatin1String(d->iid.constData(), d->iid.size())) {
                QJsonObject object = loader->metaData().value(metaDataKeyLiteral()).toObject();
                metaDataOk = true;

                QJsonArray k = object.value(keysKeyLiteral()).toArray();
                for (int i = 0; i < k.size(); ++i)
                    keys += d->cs ? k.at(i).toString() : k.at(i).toString().toLower();
            }
            if (dfm_debug_component())
                qDebug() << "Got keys from plugin meta data" << keys;


            if (!metaDataOk) {
                loader->deleteLater();
                continue;
            }

            int keyUsageCount = 0;
            for (int k = 0; k < keys.count(); ++k) {
                // first come first serve, unless the first
                // library was built with a future Qt version,
                // whereas the new one has a Qt version that fits
                // better
                const QString &key = keys.at(k);

                if (d->rki) {
                    d->keyMap.insertMulti(key, loader);
                    ++keyUsageCount;
                } else {
                    QPluginLoader *previous = d->keyMap.value(key);
                    int prev_dfm_version = 0;
                    if (previous) {
                        prev_dfm_version = (int)previous->metaData().value(versionKeyLiteral()).toDouble();
                    }
                    int dfm_version = (int)loader->metaData().value(versionKeyLiteral()).toDouble();
                    if (!previous || (prev_dfm_version > QString(QMAKE_VERSION).toDouble() && dfm_version <= QString(QMAKE_VERSION).toDouble())) {
                        d->keyMap.insertMulti(key, loader);
                        ++keyUsageCount;
                    }
                }
            }
            if (keyUsageCount || keys.isEmpty())
                d->pluginLoaderList += loader;
            else
                loader->deleteLater();
        }
    }
#else
    Q_D(PluginLoader);
    if (dfm_debug_component()) {
        qDebug() << "PluginLoader::PluginLoader() ignoring" << d->iid
                 << "since plugins are disabled in static builds";
    }
#endif
}

DASPluginLoader::~DASPluginLoader()
{
    QMutexLocker locker(qt_factoryloader_mutex());
    qt_factory_loaders()->removeAll(this);
}

QList<QJsonObject> DASPluginLoader::metaData() const
{
    Q_D(const DASPluginLoader);
    QMutexLocker locker(&d->mutex);
    QList<QJsonObject> metaData;
    for (int i = 0; i < d->pluginLoaderList.size(); ++i)
        metaData.append(d->pluginLoaderList.at(i)->metaData());

    return metaData;
}

QObject *DASPluginLoader::instance(int index) const
{
    Q_D(const DASPluginLoader);
    if (index < 0)
        return 0;

    if (index < d->pluginLoaderList.size()) {
        QPluginLoader *loader = d->pluginLoaderList.at(index);
        if (loader->instance()) {
            QObject *obj = loader->instance();
            if (obj) {
                if (!obj->parent())
                    obj->moveToThread(qApp->thread());
                return obj;
            }
        }
        return 0;
    }

    return 0;
}

#if defined(Q_OS_UNIX) && !defined (Q_OS_MAC)
QPluginLoader *DASPluginLoader::pluginLoader(const QString &key) const
{
    Q_D(const DASPluginLoader);
    return d->keyMap.value(d->cs ? key : key.toLower());
}

QList<QPluginLoader*> DASPluginLoader::pluginLoaderList(const QString &key) const
{
    Q_D(const DASPluginLoader);
    return d->keyMap.values(d->cs ? key : key.toLower());
}
#endif

void DASPluginLoader::refreshAll()
{
    QMutexLocker locker(qt_factoryloader_mutex());
    QList<DASPluginLoader *> *loaders = qt_factory_loaders();
    for (QList<DASPluginLoader *>::const_iterator it = loaders->constBegin();
         it != loaders->constEnd(); ++it) {
        (*it)->update();
    }
}

QMultiMap<int, QString> DASPluginLoader::keyMap() const
{
    QMultiMap<int, QString> result;
    const QString metaDataKey = metaDataKeyLiteral();
    const QString keysKey = keysKeyLiteral();
    const QList<QJsonObject> metaDataList = metaData();
    for (int i = 0; i < metaDataList.size(); ++i) {
        const QJsonObject metaData = metaDataList.at(i).value(metaDataKey).toObject();
        const QJsonArray keys = metaData.value(keysKey).toArray();
        const int keyCount = keys.size();
        for (int k = 0; k < keyCount; ++k)
            result.insert(i, keys.at(k).toString());
    }
    return result;
}

int DASPluginLoader::indexOf(const QString &needle) const
{
    const QString metaDataKey = metaDataKeyLiteral();
    const QString keysKey = keysKeyLiteral();
    const QList<QJsonObject> metaDataList = metaData();
    for (int i = 0; i < metaDataList.size(); ++i) {
        const QJsonObject metaData = metaDataList.at(i).value(metaDataKey).toObject();
        const QJsonArray keys = metaData.value(keysKey).toArray();
        const int keyCount = keys.size();
        for (int k = 0; k < keyCount; ++k) {
            if (!keys.at(k).toString().compare(needle, Qt::CaseInsensitive))
                return i;
        }
    }
    return -1;
}

QList<int> DASPluginLoader::getAllIndexByKey(const QString &needle) const
{
    QList<int> list;

    const QString metaDataKey = metaDataKeyLiteral();
    const QString keysKey = keysKeyLiteral();
    const QList<QJsonObject> metaDataList = metaData();
    for (int i = 0; i < metaDataList.size(); ++i) {
        const QJsonObject metaData = metaDataList.at(i).value(metaDataKey).toObject();
        const QJsonArray keys = metaData.value(keysKey).toArray();
        const int keyCount = keys.size();
        for (int k = 0; k < keyCount; ++k) {
            if (!keys.at(k).toString().compare(needle, Qt::CaseInsensitive))
                list << i;
        }
    }
    return list;
}

DAS_END_NAMESPACE