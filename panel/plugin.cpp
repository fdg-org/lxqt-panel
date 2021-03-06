/* BEGIN_COMMON_COPYRIGHT_HEADER
 * (c)LGPL2+
 *
 * LXDE-Qt - a lightweight, Qt based, desktop toolset
 * http://razor-qt.org
 *
 * Copyright: 2012 Razor team
 * Authors:
 *   Alexander Sokoloff <sokoloff.a@gmail.com>
 *
 * This program or library is free software; you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA
 *
 * END_COMMON_COPYRIGHT_HEADER */


#include "plugin.h"
#include "ilxqtpanelplugin.h"
#include "lxqtpanel.h"
#include <QDebug>
#include <QProcessEnvironment>
#include <QStringList>
#include <QDir>
#include <QFileInfo>
#include <QPluginLoader>
#include <QGridLayout>
#include <QDialog>
#include <QEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QApplication>
#include <QCryptographicHash>

#include <LXQt/Settings>
#include <LXQt/Translator>
#include <XdgIcon>
#include <algorithm> // for std::lower_bound()

// statically linked built-in plugins
#include "../plugin-clock/lxqtclock.h" // clock
#include "../plugin-desktopswitch/desktopswitch.h" // desktopswitch
#include "../plugin-mainmenu/lxqtmainmenu.h" // mainmenu
#include "../plugin-quicklaunch/lxqtquicklaunchplugin.h" // quicklaunch
#include "../plugin-showdesktop/showdesktop.h" // showdesktop
#include "../plugin-taskbar/lxqttaskbarplugin.h" // taskbar
#include "../plugin-tray/lxqttrayplugin.h" // tray
#include "../plugin-worldclock/lxqtworldclock.h" // worldclock

QColor Plugin::mMoveMarkerColor= QColor(255, 0, 0, 255);

/************************************************

 ************************************************/
Plugin::Plugin(const LxQt::PluginInfo &desktopFile, const QString &settingsFile, const QString &settingsGroup, LxQtPanel *panel) :
    QFrame(panel),
    mDesktopFile(desktopFile),
    mPluginLoader(0),
    mPlugin(0),
    mPluginWidget(0),
    mAlignment(AlignLeft),
    mSettingsGroup(settingsGroup),
    mPanel(panel)
{

    mSettings = new LxQt::Settings(settingsFile, QSettings::IniFormat, this);
    connect(mSettings, SIGNAL(settingsChanged()), this, SLOT(settingsChanged()));
    mSettings->beginGroup(settingsGroup);

    mSettingsHash = calcSettingsHash();

    setWindowTitle(desktopFile.name());
    mName = desktopFile.name();

    QStringList dirs;
    dirs << QProcessEnvironment::systemEnvironment().value("LXQTPANEL_PLUGIN_PATH").split(":");
    dirs << PLUGIN_DIR;

    bool found = false;
    if(ILxQtPanelPluginLibrary* pluginLib = findStaticPlugin(desktopFile.id()))
    {
        // this is a static plugin
        found = true;
        loadLib(pluginLib);
    }
    else {
        // this plugin is a dynamically loadable module
        QString baseName = QString("lib%1.so").arg(desktopFile.id());
        foreach(QString dirName, dirs)
        {
            QFileInfo fi(QDir(dirName), baseName);
            if (fi.exists())
            {
                found = true;
                if (loadModule(fi.absoluteFilePath()))
                    break;
            }
        }
    }

    if (!isLoaded())
    {
        if (!found)
            qWarning() << QString("Plugin %1 not found in the").arg(desktopFile.id()) << dirs;

        return;
    }

    // Load plugin translations
    LxQt::Translator::translatePlugin(desktopFile.id(), QLatin1String("lxqt-panel"));

    setObjectName(mPlugin->themeId() + "Plugin");
    QString s = mSettings->value("alignment").toString();

    // Retrun default value
    if (s.isEmpty())
    {
        mAlignment = (mPlugin->flags().testFlag(ILxQtPanelPlugin::PreferRightAlignment)) ?
                    Plugin::AlignRight :
                    Plugin::AlignLeft;
    }
    else
    {
        mAlignment = (s.toUpper() == "RIGHT") ?
                    Plugin::AlignRight :
                    Plugin::AlignLeft;

    }

    if (mPluginWidget)
    {
        QGridLayout* layout = new QGridLayout(this);
        layout->setSpacing(0);
        layout->setMargin(0);
        layout->setContentsMargins(0, 0, 0, 0);
        setLayout(layout);
        layout->addWidget(mPluginWidget, 0, 0);
    }

    saveSettings();
}


/************************************************

 ************************************************/
Plugin::~Plugin()
{
    delete mPlugin;
    if (mPluginLoader)
    {
        mPluginLoader->unload();
        delete mPluginLoader;
    }
}

void Plugin::setAlignment(Plugin::Alignment alignment)
{
    mAlignment = alignment;
    saveSettings();
}


/************************************************

 ************************************************/

ILxQtPanelPluginLibrary* Plugin::findStaticPlugin(const QString &libraryName)
{
    // find a static plugin library by name
    // internally this is implemented using binary search
    // statically linked built-in plugins
    static LxQtClockPluginLibrary clock_lib; // clock
    static DesktopSwitchPluginLibrary desktopswitch_lib; // desktopswitch
    static LxQtMainMenuPluginLibrary mainmenu_lib; // mainmenu
    static LxQtQuickLaunchPluginLibrary quicklaunch_lib; // quicklaunch
    static ShowDesktopLibrary showdesktop_lib; //showdesktop
    static LxQtTaskBarPluginLibrary taskbar_lib; //taskbar
    static LxQtTrayPluginLibrary tray_lib; //tray
    static LxQtWorldClockLibrary worldclock_lib; // worldclock

    static const QString names[] = // the names should be kept sorted (for binary search)
    {
        QStringLiteral("clock"),
        QStringLiteral("desktopswitch"),
        QStringLiteral("mainmenu"),
        QStringLiteral("quicklaunch"),
        QStringLiteral("showdesktop"),
        QStringLiteral("taskbar"),
        QStringLiteral("tray"),
        QStringLiteral("worldclock")
    };
    static ILxQtPanelPluginLibrary* staticPlugins[] = // should be kept in the same order as names
    {
        &clock_lib,
        &desktopswitch_lib,
        &mainmenu_lib,
        &quicklaunch_lib,
        &showdesktop_lib,
        &taskbar_lib,
        &tray_lib,
        &worldclock_lib
    };

    // for small tables, binary search is often faster than hash tables
    const QString* end = names + sizeof(names)/sizeof(QString);
    const QString* it = std::lower_bound(names, end, libraryName);
    if(it != end && *it == libraryName) {
        return staticPlugins[(it - names)];
    }
    return NULL;
}

// load a plugin from a library
bool Plugin::loadLib(ILxQtPanelPluginLibrary* pluginLib)
{
    ILxQtPanelPluginStartupInfo startupInfo;
    startupInfo.settings = mSettings;
    startupInfo.desktopFile = &mDesktopFile;
    startupInfo.lxqtPanel = mPanel;

    mPlugin = pluginLib->instance(startupInfo);
    if (!mPlugin)
    {
        qWarning() << QString("Can't load plugin \"%1\". Plugin can't build ILxQtPanelPlugin.").arg(mPluginLoader->fileName());
        return false;
    }

    mPluginWidget = mPlugin->widget();
    if (mPluginWidget)
    {
        mPluginWidget->setObjectName(mPlugin->themeId());
    }
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    return true;
}

// load dynamic plugin from a *.so module
bool Plugin::loadModule(const QString &libraryName)
{
    mPluginLoader = new QPluginLoader(libraryName);

    if (!mPluginLoader->load())
    {
        qWarning() << mPluginLoader->errorString();
        return false;
    }

    QObject *obj = mPluginLoader->instance();
    if (!obj)
    {
        qWarning() << mPluginLoader->errorString();
        return false;
    }

    ILxQtPanelPluginLibrary* pluginLib= qobject_cast<ILxQtPanelPluginLibrary*>(obj);
    if (!pluginLib)
    {
        qWarning() << QString("Can't load plugin \"%1\". Plugin is not a ILxQtPanelPluginLibrary.").arg(mPluginLoader->fileName());
        delete obj;
        return false;
    }
    return loadLib(pluginLib);
}


/************************************************

 ************************************************/
QByteArray Plugin::calcSettingsHash()
{
    QCryptographicHash hash(QCryptographicHash::Md5);
    QStringList keys = mSettings->allKeys();
    foreach (const QString &key, keys)
    {
        hash.addData(key.toUtf8());
        hash.addData(mSettings->value(key).toByteArray());
    }
    return hash.result();
}


/************************************************

 ************************************************/
void Plugin::settingsChanged()
{
    QByteArray hash = calcSettingsHash();
    if (mSettingsHash != hash)
    {
        mSettingsHash = hash;
        mPlugin->settingsChanged();
    }
}


/************************************************

 ************************************************/
void Plugin::saveSettings()
{
    mSettings->setValue("alignment", (mAlignment == AlignLeft) ? "Left" : "Right");
    mSettings->setValue("type", mDesktopFile.id());
    mSettings->sync();

}


/************************************************

 ************************************************/
void Plugin::contextMenuEvent(QContextMenuEvent *event)
{
    mPanel->showPopupMenu(this);
}


/************************************************

 ************************************************/
void Plugin::mousePressEvent(QMouseEvent *event)
{
    switch (event->button())
    {
    case Qt::LeftButton:
        mPlugin->activated(ILxQtPanelPlugin::Trigger);
        break;

    case Qt::MidButton:
        mPlugin->activated(ILxQtPanelPlugin::MiddleClick);
        break;

    default:
        break;
    }
}


/************************************************

 ************************************************/
void Plugin::mouseDoubleClickEvent(QMouseEvent*)
{
    mPlugin->activated(ILxQtPanelPlugin::DoubleClick);
}


/************************************************

 ************************************************/
void Plugin::showEvent(QShowEvent *)
{
    if (mPluginWidget)
        mPluginWidget->adjustSize();
}


/************************************************

 ************************************************/
QMenu *Plugin::popupMenu() const
{
    QString name = this->name().replace("&", "&&");
    QMenu* menu = new QMenu(windowTitle());

    if (mPlugin->flags().testFlag(ILxQtPanelPlugin::HaveConfigDialog))
    {
        QAction* configAction = new QAction(
            XdgIcon::fromTheme(QStringLiteral("preferences-other")),
            tr("Configure \"%1\"").arg(name), menu);
        menu->addAction(configAction);
        connect(configAction, SIGNAL(triggered()), this, SLOT(showConfigureDialog()));
    }

    QAction* moveAction = new QAction(XdgIcon::fromTheme("transform-move"), tr("Move \"%1\"").arg(name), menu);
    menu->addAction(moveAction);
    connect(moveAction, SIGNAL(triggered()), this, SIGNAL(startMove()));

    menu->addSeparator();

    QAction* removeAction = new QAction(
        XdgIcon::fromTheme(QStringLiteral("list-remove")),
        tr("Remove \"%1\"").arg(name), menu);
    menu->addAction(removeAction);
    connect(removeAction, SIGNAL(triggered()), this, SLOT(requestRemove()));

    return menu;
}


/************************************************

 ************************************************/
bool Plugin::isSeparate() const
{
   return mPlugin->isSeparate();
}


/************************************************

 ************************************************/
bool Plugin::isExpandable() const
{
    return mPlugin->isExpandable();
}


/************************************************

 ************************************************/
void Plugin::realign()
{
    if (mPlugin)
        mPlugin->realign();
}


/************************************************

 ************************************************/
void Plugin::showConfigureDialog()
{
    // store a pointer to each plugin using the plugins' names
    static QHash<QString, QPointer<QDialog> > refs;
    QDialog *dialog = refs[name()].data();

    if (!dialog)
    {
        dialog = mPlugin->configureDialog();
        refs[name()] = dialog;
        connect(this, SIGNAL(destroyed()), dialog, SLOT(close()));
    }

    if (!dialog)
        return;

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}


/************************************************

 ************************************************/
void Plugin::requestRemove()
{
    emit remove();
    deleteLater();
}
