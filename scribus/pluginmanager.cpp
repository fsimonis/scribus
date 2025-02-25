/*
For general Scribus (>=1.3.2) copyright and licensing information please refer
to the COPYING file provided with the program. Following this notice may exist
a copyright and/or license notice that predates the release of Scribus 1.3.2
for which a new license (GPL+exception) is in place.
*/
#include "pluginmanager.h"
#include "scplugin.h"
#include "loadsaveplugin.h"

#include <QDir>
#include <QEvent>
#include <QMessageBox>

#include "scconfig.h"


#include "scribusdoc.h"
#include "scribuscore.h"
#include "ui/sctoolbar.h"
#include "selection.h"
#include "ui/scmwmenumanager.h"
#include "scraction.h"
#include "ui/splash.h"
#include "prefsmanager.h"
#include "prefsfile.h"
#include "scpaths.h"
#include "commonstrings.h"
#include "ui/storyeditor.h"

#ifdef HAVE_DLFCN_H
#include <dlfcn.h>
#elif defined(DLL_USE_NATIVE_API) && defined(_WIN32)
#include <windows.h>
#else
#include <QLibrary>
#endif

PluginManager::PluginManager() :
	QObject(nullptr),
	prefs(PrefsManager::instance().prefsFile->getPluginContext("pluginmanager"))
{
}

void* PluginManager::loadDLL(const QString& plugin)
{
	void* lib = nullptr;
#ifdef HAVE_DLFCN_H
	QString libpath = QDir::toNativeSeparators(plugin);
	lib = dlopen(libpath.toLocal8Bit().data(), RTLD_LAZY | RTLD_GLOBAL);
	if (!lib)
	{
		const char* error = dlerror();
		qDebug() << tr("Error loading plugin", "plugin manager").toLocal8Bit().data();
		if (error)
			qDebug("%s", error);
		else
			qDebug() << tr("Unknown error","plugin manager").toLocal8Bit().data();
	}
#elif defined(DLL_USE_NATIVE_API) && defined(_WIN32)
	QString libpath = QDir::toNativeSeparators(plugin);
	HINSTANCE hdll = LoadLibraryW((const wchar_t*) libpath.utf16());
	lib = (void*) hdll;
#else
	if (QFile::exists(plugin))
		lib = (void*) new QLibrary(plugin);
	else
	{
		qDebug() << tr("Error loading plugin", "plugin manager").toLocal8Bit().data();
		qDebug("%s", plugin.toLocal8Bit().data());
	}
#endif
	return lib;
}

QFunctionPointer PluginManager::resolveSym(void* plugin, const char* sym)
{
	QFunctionPointer symAddr = nullptr;
#ifdef HAVE_DLFCN_H
	const char* error;
	dlerror();
	symAddr = (QFunctionPointer) dlsym(plugin, sym);
	if ((error = dlerror()) != nullptr)
	{
		qDebug("%s", tr("Cannot find symbol (%1)", "plugin manager").arg(error).toLocal8Bit().data());
		symAddr = nullptr;
	}
#elif defined(DLL_USE_NATIVE_API) && defined(_WIN32)
	symAddr = (QFunctionPointer) GetProcAddress((HMODULE) plugin, sym);
	if (symAddr == nullptr)
		qDebug("%s", tr("Cannot find symbol (%1)", "plugin manager").arg(sym).toLocal8Bit().data());
#else
	QLibrary* qlib = (QLibrary*) plugin;
	if (plugin)
		symAddr = qlib->resolve(sym);
	if (symAddr == nullptr)
		qDebug("%s", tr("Cannot find symbol (%1)", "plugin manager").arg(sym).toLocal8Bit().data());
#endif
	return symAddr;
}

void  PluginManager::unloadDLL(void* plugin)
{
#ifdef HAVE_DLFCN_H
	dlclose(plugin);
	dlerror();
#elif defined(DLL_USE_NATIVE_API) && defined(_WIN32)
	FreeLibrary((HMODULE) plugin);
#else
	delete ((QLibrary*) plugin);
#endif
}

void PluginManager::savePreferences()
{
	// write configuration
	for (auto it = pluginMap.constBegin(); it != pluginMap.constEnd(); ++it)
	{
		const PluginData& pluginData = it.value();
		prefs->set(pluginData.pluginName, pluginData.enableOnStartup);
	}
}

QString PluginManager::getPluginName(const QString& fileName)
{
	// Must return plugin name. Note that this may be platform dependent;
	// it's likely to need some adjustment for platform naming schemes.
	// It currently handles:
	//    (lib)?pluginname(\.pluginext)?
	QFileInfo fi(fileName);
	QString baseName(fi.baseName());
	if (baseName.startsWith("lib"))
		baseName.remove(0, 3);
	if (baseName.endsWith(platformDllExtension()))
		baseName.chop(1 + platformDllExtension().length());
	// check name
	for (int i = 0; i < (int) baseName.length(); i++)
	{
		if (! baseName[i].isLetterOrNumber() && baseName[i] != '_' )
		{
			qDebug("Invalid character in plugin name for %s; skipping",
					fileName.toLocal8Bit().data());
			return QString();
		}
	}
	return baseName.toLatin1();
}

int PluginManager::initPlugin(const QString& fileName)
{
	PluginData pda;
	pda.pluginFile = QString("%1/%2").arg(ScPaths::instance().pluginDir(), fileName);
	pda.pluginName = getPluginName(pda.pluginFile);
	if (pda.pluginName.isNull())
		// Couldn't determine plugname from filename. We've already complained, so
		// move on to the next one.
		return 0;
	pda.plugin = nullptr;
	pda.pluginDLL = nullptr;
	pda.enabled = false;
	pda.enableOnStartup = prefs->getBool(pda.pluginName, false);
	ScCore->setSplashStatus( tr("Plugin: loading %1", "plugin manager").arg(pda.pluginName));
	if (loadPlugin(pda))
	{
		//HACK: Always enable our only persistent plugin, scripter
		if (pda.plugin->inherits("ScPersistentPlugin"))
			pda.enableOnStartup = true;
		if (pda.enableOnStartup)
			enablePlugin(pda);
		pluginMap.insert(pda.pluginName, pda);
		return 1;
	}
	return 0;
}

void PluginManager::initPlugs()
{
	Q_ASSERT(!pluginMap.count());
	QString libPattern = QString("*.%1*").arg(platformDllExtension());
	QMap<QString, int> allPlugs;
	int loaded = 0;
	uint changes = 1;
	QStringList failedPlugs; // output string for warn dialog

	/*! \note QDir::Reversed is there due the Mac plugin dependency.
	barcode depends on psimport. and load on that platform expect the
	psimp before barcode.You know, security by obscurity ;) PV */
	QDir dirList(ScPaths::instance().pluginDir(),
				 libPattern, QDir::Name | QDir::Reversed,
				 (QDir::Filter) PluginManager::platformDllSearchFlags());

	if ((!dirList.exists()) || (dirList.count() == 0))
		return;
	for (uint i = 0; i < dirList.count(); ++i)
	{
		int res = initPlugin(dirList[i]);
		allPlugs[dirList[i]] = res;
		if (res != 0)
			++loaded;
		else
			failedPlugs.append(dirList[i]);
	}
	/* Re-try the failed plugins again and again until it promote
	any progress (changes variable is changing ;)) */
	while (loaded < allPlugs.count() && changes != 0)
	{
		changes = 0;
		for (auto it = allPlugs.begin(); it != allPlugs.end(); ++it)
		{
			if (it.value() != 0)
				continue;
			int res = initPlugin(it.key());
			allPlugs[it.key()] = res;
			if (res == 1)
			{
				++loaded;
				++changes;
				failedPlugs.removeAll(it.key());
			}
		}
	}
	if (loaded != allPlugs.count())
	{
		if (ScCore->usingGUI())
		{
			bool splashShown = ScCore->splashShowing();
			QString failedStr("<ul>");
			for (QStringList::Iterator it = failedPlugs.begin(); it != failedPlugs.end(); ++it)
				failedStr += "<li>" + *it + "</li>";
			failedStr += "</ul>";
			if (splashShown)
				ScCore->showSplash(false);
			ScMessageBox::warning(ScCore->primaryMainWindow(), CommonStrings::trWarning,
								 "<qt>" + tr("There is a problem loading %1 of %2 plugins. %3 This is probably caused by some kind of dependency issue or old plugins existing in your install directory. If you clean out your install directory and reinstall and this still occurs, please report it on bugs.scribus.net."
										).arg(allPlugs.count() - loaded).arg(allPlugs.count()).arg(failedStr)
									 + "</qt>");
			if (splashShown)
				ScCore->showSplash(true);
		}
	}
}

// After a plugin has been initialized, this method calls its setup
// routines and connects it to the application.
void PluginManager::enablePlugin(PluginData & pda)
{
	Q_ASSERT(pda.enabled == false);
	QString failReason;
	bool isActionPlugin = false;
	if (pda.plugin->inherits("ScActionPlugin"))
	{
		isActionPlugin = true;
	}
	else if (pda.plugin->inherits("ScPersistentPlugin"))
	{
		ScPersistentPlugin* plugin = qobject_cast<ScPersistentPlugin*>(pda.plugin);
		assert(plugin);
		pda.enabled = plugin->initPlugin();
		if (!pda.enabled)
			failReason = tr("init failed", "plugin load error");
	}
/* temporary hack to enable the import plugins */
	else if (pda.plugin->inherits("LoadSavePlugin"))
		pda.enabled = true;
	else
		failReason = tr("unknown plugin type", "plugin load error");

	if (pda.enabled || isActionPlugin)
		ScCore->setSplashStatus(tr("Plugin: %1 loaded", "plugin manager").arg(pda.plugin->fullTrName()));
	else
		ScCore->setSplashStatus(tr("Plugin: %1 failed to load: %2", "plugin manager").arg(pda.plugin->fullTrName(), failReason));
}

bool PluginManager::setupPluginActions(ScribusMainWindow *mw)
{
	if (!mw)
		return false;
	ScActionPlugin* plugin = nullptr;

	for (PluginMap::Iterator it = pluginMap.begin(); it != pluginMap.end(); ++it)
	{
		if (!it.value().plugin->inherits("ScActionPlugin"))
		{
			it.value().plugin->addToMainWindowMenu(mw);
			continue;
		}

		//Add in ScrAction based plugin linkage
		//Insert DLL Action into Dictionary with values from plugin interface
		plugin = qobject_cast<ScActionPlugin*>(it.value().plugin);
		assert(plugin);

		ScActionPlugin::ActionInfo ai(plugin->actionInfo());
		ScrAction* action = new ScrAction(ScrAction::ActionDLL, ai.iconPath1, ai.iconPath2, ai.text, ai.keySequence, mw);
		Q_CHECK_PTR(action);
		action->setStatusTip(ai.helpText);
		action->setToolTip(ai.helpText);
		mw->scrActions.insert(ai.name, action);

		// then enable and connect up the action
		mw->scrActions[ai.name]->setEnabled(ai.enabledOnStartup);

		// Connect action's activated signal with the plugin's run method
		it.value().enabled = connect(mw->scrActions[ai.name], SIGNAL(triggeredData(ScribusDoc*)), plugin, SLOT(run(ScribusDoc*)) );

			//Get the menu manager to add the DLL's menu item to the right menu, after the chosen existing item
		if (ai.menuAfterName.isEmpty())
		{
			if (!ai.menu.isEmpty())
			{
				if ((!ai.subMenuName.isEmpty()) && (!ai.parentMenu.isEmpty()))
				{
					if (!mw->scrMenuMgr->menuExists(ai.menu))
					{
						mw->scrMenuMgr->createMenu(ai.menu, ai.subMenuName, ai.parentMenu);
					}
				}
				mw->scrMenuMgr->addMenuItemString(ai.name, ai.menu);
			}
		}
		else
		{
			if ((!ai.subMenuName.isEmpty()) && (!ai.parentMenu.isEmpty()))
			{
				if (!mw->scrMenuMgr->menuExists(ai.menu))
					mw->scrMenuMgr->createMenu(ai.menu, ai.subMenuName, ai.parentMenu);
			}
			mw->scrMenuMgr->addMenuItemStringAfter(ai.name, ai.menuAfterName, ai.menu);
		}
		if (!ai.toolbar.isEmpty())
		{
			QString tbName = ai.toolbar;
			if (mw->scrToolBars.contains(tbName))
				mw->scrToolBars[tbName]->addAction(mw->scrActions[ai.name]);
			else
			{
				ScToolBar *tb = new ScToolBar(ai.toolBarName, ai.toolbar, mw);
				tb->addAction(mw->scrActions[ai.name]);
				mw->addScToolBar(tb, tbName);
			}
		}
		if (it.value().enabled)
			ScCore->setSplashStatus( tr("Plugin: %1 initialized ok ", "plugin manager").arg(plugin->fullTrName()));
		else
			ScCore->setSplashStatus( tr("Plugin: %1 failed post initialization", "plugin manager").arg(plugin->fullTrName()));
	}

	//CB maybe we should just call mw->createMenuBar() here instead...
	mw->scrMenuMgr->clearMenu("File");
	mw->scrMenuMgr->addMenuItemStringsToMenuBar("File", mw->scrActions);
	mw->scrMenuMgr->clearMenu("Edit");
	mw->scrMenuMgr->addMenuItemStringsToMenuBar("Edit", mw->scrActions);
	mw->scrMenuMgr->clearMenu("Insert");
	mw->scrMenuMgr->addMenuItemStringsToMenuBar("Insert", mw->scrActions);
	mw->scrMenuMgr->clearMenu("Item");
	mw->scrMenuMgr->addMenuItemStringsToMenuBar("Item", mw->scrActions);
	mw->scrMenuMgr->clearMenu("Page");
	mw->scrMenuMgr->addMenuItemStringsToMenuBar("Page", mw->scrActions);
	mw->scrMenuMgr->clearMenu("ItemTable");
	mw->scrMenuMgr->addMenuItemStringsToMenuBar("ItemTable", mw->scrActions);
	mw->scrMenuMgr->clearMenu("Extras");
	mw->scrMenuMgr->addMenuItemStringsToMenuBar("Extras", mw->scrActions);
	mw->scrMenuMgr->clearMenu("View");
	mw->scrMenuMgr->addMenuItemStringsToMenuBar("View", mw->scrActions);
	mw->scrMenuMgr->clearMenu("Help");
	mw->scrMenuMgr->addMenuItemStringsToMenuBar("Help", mw->scrActions);

	return true;
}

bool PluginManager::setupPluginActions(StoryEditor *sew)
{
	if (!sew)
		return false;
	ScActionPlugin* plugin = nullptr;

	for (PluginMap::Iterator it = pluginMap.begin(); it != pluginMap.end(); ++it)
	{
		if (!it.value().plugin->inherits("ScActionPlugin"))
			continue;

		//Add in ScrAction based plugin linkage
		//Insert DLL Action into Dictionary with values from plugin interface
		plugin = qobject_cast<ScActionPlugin*>(it.value().plugin);
		assert(plugin);

		ScActionPlugin::ActionInfo ai(plugin->actionInfo());
		if (!ai.enabledForStoryEditor)
			continue;

		ScrAction* action = new ScrAction(ScrAction::ActionDLLSE, ai.iconPath1, ai.iconPath2, ai.text, ai.keySequence, sew);
		Q_CHECK_PTR(action);
		sew->seActions.insert(ai.name, action);

		// then enable and connect up the action
		sew->seActions[ai.name]->setEnabled(ai.enabledForStoryEditor);

		// Connect action's activated signal with the plugin's run method
		it.value().enabled = connect(sew->seActions[ai.name], SIGNAL(triggeredData(QWidget*,ScribusDoc*)), plugin, SLOT(run(QWidget*,ScribusDoc*)));

		//Get the menu manager to add the DLL's menu item to the right menu, after the chosen existing item
		if (ai.menuAfterName.isEmpty())
		{
			if (!ai.seMenu.isEmpty())
			{
				if ((!ai.subMenuName.isEmpty()) && (!ai.parentMenu.isEmpty()))
				{
					if (!sew->seMenuMgr->menuExists(ai.seMenu))
						sew->seMenuMgr->createMenu(ai.seMenu, ai.subMenuName, ai.parentMenu);
				}
				sew->seMenuMgr->addMenuItemString(ai.name, ai.menu);
			}
		}
		else
		{
			if ((!ai.subMenuName.isEmpty()) && (!ai.parentMenu.isEmpty()))
			{
				if (!sew->seMenuMgr->menuExists(ai.seMenu))
					sew->seMenuMgr->createMenu(ai.seMenu, ai.subMenuName, ai.parentMenu);
			}
			sew->seMenuMgr->addMenuItemStringAfter(ai.name, ai.menuAfterName, ai.menu);
		}
	}
	return true;
}


void PluginManager::enableOnlyStartupPluginActions(ScribusMainWindow* mw)
{
	if (!mw)
		return;

	ScActionPlugin* plugin = nullptr;
	for (PluginMap::Iterator it = pluginMap.begin(); it != pluginMap.end(); ++it)
	{
		if (!it.value().plugin->inherits("ScActionPlugin"))
			continue;
		plugin = qobject_cast<ScActionPlugin*>(it.value().plugin);
		assert(plugin);
		ScActionPlugin::ActionInfo ai(plugin->actionInfo());
		if (mw->scrActions.contains(ai.name))
			mw->scrActions[ai.name]->setEnabled(ai.enabledOnStartup);
	}
}

void PluginManager::enablePluginActionsForSelection(ScribusMainWindow* mw)
{
	if (!mw || !mw->doc)
		return;
	ScribusDoc* doc = mw->doc;

	int selectedType = -1;
	if (doc->m_Selection->count() > 0)
	{
		PageItem *currItem = doc->m_Selection->itemAt(0);
		selectedType = currItem->itemType();
	}
	bool isLayerLocked = doc->layerLocked(doc->activeLayer());

	ScActionPlugin* actionPlug = nullptr;
	ScrAction* pluginAction = nullptr;
	for (PluginMap::Iterator it = pluginMap.begin(); it != pluginMap.end(); ++it)
	{
		if (!it.value().plugin->inherits("ScActionPlugin"))
			continue;
		actionPlug = qobject_cast<ScActionPlugin*>(it.value().plugin);
		if (!actionPlug)
			continue;

		ScActionPlugin::ActionInfo actionInfo(actionPlug->actionInfo());
		pluginAction = mw->scrActions[actionInfo.name];
		if (pluginAction == nullptr)
			continue;
		if (isLayerLocked && !actionInfo.enabledOnStartup)
			pluginAction->setEnabled(false);
		else
			pluginAction->setEnabled(actionPlug->handleSelection(doc, selectedType));
	}
}

bool PluginManager::DLLexists(const QString& name, bool includeDisabled) const
{
	// the plugin name must be known
	if (pluginMap.contains(name))
	{
		// the plugin must be loaded
		if (pluginMap[name].plugin)
		{
			// and the plugin must be enabled unless we were told otherwise
			if (pluginMap[name].enabled)
				return true;
			return includeDisabled;
		}
	}
	return false;
}

bool PluginManager::loadPlugin(PluginData& pluginData)
{
	typedef int (*getPluginAPIVersionPtr)();
	typedef ScPlugin* (*getPluginPtr)();
	getPluginAPIVersionPtr getPluginAPIVersion;
	getPluginPtr getPlugin;

	Q_ASSERT(pluginData.plugin == nullptr);
	Q_ASSERT(pluginData.pluginDLL == nullptr);
	Q_ASSERT(!pluginData.enabled);
	pluginData.plugin = nullptr;

	pluginData.pluginDLL = loadDLL(pluginData.pluginFile);
	if (!pluginData.pluginDLL)
		return false;

	getPluginAPIVersion = (getPluginAPIVersionPtr)
		resolveSym(pluginData.pluginDLL, QString(pluginData.pluginName + "_getPluginAPIVersion").toLocal8Bit().data());
	if (getPluginAPIVersion)
	{
		int gotVersion = (*getPluginAPIVersion)();
		if (gotVersion != PLUGIN_API_VERSION)
		{
			qDebug("API version mismatch when loading %s: Got %i, expected %i",
					pluginData.pluginFile.toLocal8Bit().data(), gotVersion, PLUGIN_API_VERSION);
		}
		else
		{
			getPlugin = (getPluginPtr)
				resolveSym(pluginData.pluginDLL, QString(pluginData.pluginName + "_getPlugin").toLocal8Bit().data());
			if (getPlugin)
			{
				pluginData.plugin = (*getPlugin)();
				if (!pluginData.plugin)
				{
					qDebug("Unable to get ScPlugin when loading %s",
							pluginData.pluginFile.toLocal8Bit().data());
				}
				else
					return true;
			}
		}
	}
	unloadDLL(pluginData.pluginDLL);
	pluginData.pluginDLL = nullptr;
	Q_ASSERT(!pluginData.plugin);
	return false;
}

void PluginManager::cleanupPlugins()
{
	for (PluginMap::Iterator it = pluginMap.begin(); it != pluginMap.end(); ++it)
	{
		if (!it.value().enabled)
			continue;
		finalizePlug(it.value());
	}
}

void PluginManager::finalizePlug(PluginData & pluginData)
{
	typedef void (*freePluginPtr)(ScPlugin* plugin);
	if (pluginData.plugin)
	{
		if (pluginData.enabled)
			disablePlugin(pluginData);
		Q_ASSERT(!pluginData.enabled);
		freePluginPtr freePlugin = (freePluginPtr) resolveSym(pluginData.pluginDLL, QString(pluginData.pluginName + "_freePlugin").toLocal8Bit().data());
		if (freePlugin)
			(*freePlugin)(pluginData.plugin);
		pluginData.plugin = nullptr;
	}
	Q_ASSERT(!pluginData.enabled);
	if (pluginData.pluginDLL)
	{
		unloadDLL(pluginData.pluginDLL);
		pluginData.pluginDLL = nullptr;
	}
}

void PluginManager::disablePlugin(PluginData & pda)
{
	Q_ASSERT(pda.enabled);
	Q_ASSERT(pda.plugin);
	if (pda.plugin->inherits("ScActionPlugin"))
	{
		ScActionPlugin* plugin = qobject_cast<ScActionPlugin*>(pda.plugin);
		assert(plugin);
		plugin->cleanupPlugin();
		// FIXME: Correct way to delete action?
		delete ScCore->primaryMainWindow()->scrActions[plugin->actionInfo().name];
	}
	else if (pda.plugin->inherits("ScPersistentPlugin"))
	{
		ScPersistentPlugin* plugin = qobject_cast<ScPersistentPlugin*>(pda.plugin);
		assert(plugin);
		plugin->cleanupPlugin();
	}
/* temporary hack to enable the import plugins */
	else if (pda.plugin->inherits("LoadSavePlugin"))
		pda.enabled = false;
	else
		Q_ASSERT(false); // We shouldn't ever have enabled an unknown plugin type.
	pda.enabled = false;
}

QString PluginManager::platformDllExtension()
{
#ifdef __hpux
	// HP/UX
	return "sl";
#elif defined(__APPLE__) && defined(__MACH__)
	// MacOS/X, Darwin

	// MacOS/X may actually use both 'so' and 'dylib'. .so is usually used
	// for plugins etc, dylib for system and app libraries We need to
	// support this distinction in the plugin manager, but for now it's
	// most appropriate to return the extension used by plugins -- CR

	//return "dylib";
	return "so";
#elif defined(_WIN32) || defined(_WIN64)
	return "dll";
#else
	// Generic *NIX
	return "so";
#endif
}

int PluginManager::platformDllSearchFlags()
{
#if defined(_WIN32) || defined(_WIN64)
	return (QDir::Files | QDir::NoSymLinks);
#else
	return (QDir::Files | QDir::Executable | QDir::NoSymLinks);
#endif
}

void PluginManager::languageChange()
{
	ScPlugin* plugin = nullptr;
	ScActionPlugin* ixplug = nullptr;
	ScrAction* pluginAction = nullptr;
	for (PluginMap::Iterator it = pluginMap.begin(); it != pluginMap.end(); ++it)
	{
		plugin = it.value().plugin;
		if (!plugin)
			continue;
		plugin->languageChange();

		ixplug = qobject_cast<ScActionPlugin*>(plugin);
		if (!ixplug)
			continue;

		ScActionPlugin::ActionInfo ai(ixplug->actionInfo());
		pluginAction = ScCore->primaryMainWindow()->scrActions[ai.name];
		if (pluginAction != nullptr)
			pluginAction->setText(ai.text);
		if ((!ai.menu.isEmpty()) && (!ai.subMenuName.isEmpty()))
			ScCore->primaryMainWindow()->scrMenuMgr->setText(ai.menu, ai.subMenuName);
	}
}

ScPlugin* PluginManager::getPlugin(const QString & pluginName, bool includeDisabled) const
{
	if (DLLexists(pluginName, includeDisabled))
		return pluginMap[pluginName].plugin;
	return nullptr;
}

PluginManager & PluginManager::instance()
{
	return (*ScCore->pluginManager);
}

QString PluginManager::getPluginPath(const QString & pluginName) const
{
	// It is not legal to call this function without a valid
	// plugin name.
	Q_ASSERT(pluginMap.contains(pluginName));
	return pluginMap[pluginName].pluginFile;
}

bool & PluginManager::enableOnStartup(const QString & pluginName)
{
	// It is not legal to call this function without a valid
	// plugin name.
	Q_ASSERT(pluginMap.contains(pluginName));
	return pluginMap[pluginName].enableOnStartup;
}

bool PluginManager::enabled(const QString & pluginName)
{
	// It is not legal to call this function without a valid
	// plugin name.
	Q_ASSERT(pluginMap.contains(pluginName));
	return pluginMap[pluginName].enabled;
}

QStringList PluginManager::pluginNames(bool includeDisabled, const char* inherits) const
{
	// Scan the plugin map for plugins...
	QStringList names;
	for (PluginMap::ConstIterator it = pluginMap.constBegin(); it != pluginMap.constEnd(); ++it)
	{
		if (!includeDisabled && !it.value().enabled)
			continue;

		// Only including plugins that inherit a named parent (if
		// specified), using the QMetaObject system.
		if (!inherits || it.value().plugin->inherits(inherits))
			names.append(it.value().pluginName);
	}
	return names;
}
