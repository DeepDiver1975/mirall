/*
 * Copyright (C) by Duncan Mac-Vicar P. <duncan@kde.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include <iostream>

#include "mirall/application.h"
#include "mirall/folder.h"
#include "mirall/folderwatcher.h"
#include "mirall/networklocation.h"
#include "mirall/unisonfolder.h"
#include "mirall/owncloudfolder.h"
#include "mirall/owncloudsetupwizard.h"
#include "mirall/owncloudinfo.h"
#include "mirall/sslerrordialog.h"
#include "mirall/theme.h"
#include "mirall/mirallconfigfile.h"
#include "mirall/updatedetector.h"
#include "mirall/version.h"
#include "mirall/credentialstore.h"
#include "mirall/logger.h"
#include "mirall/settingsdialog.h"

#ifdef WITH_CSYNC
#include "mirall/csyncfolder.h"
#endif
#include "mirall/inotify.h"

#include <QtCore>
#include <QtGui>
#include <QHash>
#include <QHashIterator>
#include <QUrl>
#include <QDesktopServices>
#include <QTranslator>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>

#ifdef Q_OS_LINUX
#include <dlfcn.h>
#endif

namespace Mirall {

// application logging handler.
void mirallLogCatcher(QtMsgType type, const char *msg)
{
  Q_UNUSED(type)
  Logger::instance()->mirallLog( QString::fromUtf8(msg) );
}

namespace {
QString applicationTrPath()
{
#ifdef Q_OS_LINUX
    // FIXME - proper path!
    return QLatin1String("/usr/share/mirall/i18n/");
#endif
#ifdef Q_OS_MAC
    return QApplication::applicationDirPath()+QLatin1String("/../Resources/Translations"); // path defaults to app dir.
#endif
#ifdef Q_OS_WIN32
   return QApplication::applicationDirPath();
#endif
}
}

// ----------------------------------------------------------------------------------

Application::Application(int &argc, char **argv) :
    SharedTools::QtSingleApplication(argc, argv),
    _tray(0),
#if QT_VERSION >= 0x040700
    _networkMgr(new QNetworkConfigurationManager(this)),
#endif
    _sslErrorDialog(0),
    _contextMenu(0),
    _theme(Theme::instance()),
    _updateDetector(0),
    _logBrowser(0),
    _showLogWindow(false),
    _logFlush(false),
    _helpOnly(false),
    _fileItemDialog(0),
    _settingsDialog(0)
{
    setApplicationName( _theme->appNameGUI() );
    setWindowIcon( _theme->applicationIcon() );

    parseOptions(arguments());
    setupTranslations();
    setupLogBrowser();
    //no need to waste time;
    if ( _helpOnly ) return;

#ifdef Q_OS_LINUX
        // HACK: bump the refcount for libgnutls by calling dlopen()
        // so gnutls, which is an dependency of libneon on some linux
        // distros, and does not cleanup it's FDs properly, does
        // not get unloaded. This works around a FD exhaustion crash
        // (#154). We are not using gnutls at all and it's fine
        // if loading fails, so no error handling is performed here.
        dlopen("libgnutls.so", RTLD_LAZY|RTLD_NODELETE);
#endif

    connect( this, SIGNAL(messageReceived(QString)), SLOT(slotParseOptions(QString)));
    connect( Logger::instance(), SIGNAL(guiLog(QString,QString)),
             this, SLOT(slotShowTrayMessage(QString,QString)));
    // create folder manager for sync folder management
    _folderScheduler = FolderScheduler::instance();
    connect( _folderScheduler, SIGNAL(folderSyncStateChange(QString)),
             this,SLOT(slotSyncStateChange(QString)));

    _folderScheduler->setSyncEnabled(false);

    /* use a signal mapper to map the open requests to the alias names */
    _folderOpenActionMapper = new QSignalMapper(this);
    connect(_folderOpenActionMapper, SIGNAL(mapped(const QString &)),
            this, SLOT(slotFolderOpenAction(const QString &)));

    setQuitOnLastWindowClosed(false);

    _owncloudSetupWizard = new OwncloudSetupWizard( _folderScheduler, _theme, this );
    connect( _owncloudSetupWizard, SIGNAL(ownCloudWizardDone(int)),
             this, SLOT(slotownCloudWizardDone(int)));

//    _statusDialog = new StatusDialog( _theme );
//    connect( _statusDialog, SIGNAL(addASync()), this, SLOT(slotAddFolder()) );

//    connect( _statusDialog, SIGNAL(removeFolderAlias( const QString&)),
//             SLOT(slotRemoveFolder(const QString&)));
//    connect( _statusDialog, SIGNAL(enableFolderAlias(QString,bool)),
//             SLOT(slotEnableFolder(QString,bool)));
//    connect( _statusDialog, SIGNAL(infoFolderAlias(const QString&)),
//             SLOT(slotInfoFolder( const QString&)));
//    connect( _statusDialog, SIGNAL(openFolderAlias(const QString&)),
//             SLOT(slotFolderOpenAction(QString)));

#if 0
#if QT_VERSION >= 0x040700
    qDebug() << "* Network is" << (_networkMgr->isOnline() ? "online" : "offline");
    foreach (const QNetworkConfiguration& netCfg, _networkMgr->allConfigurations(QNetworkConfiguration::Active)) {
        //qDebug() << "Network:" << netCfg.identifier();
    }
#endif
#endif

    MirallConfigFile cfg;
    _theme->setSystrayUseMonoIcons(cfg.monoIcons());
    connect (_theme, SIGNAL(systrayUseMonoIconsChanged(bool)), SLOT(slotUseMonoIconsChanged(bool)));

    setupActions();
    setupSystemTray();
    setupProxy();

    int cnt = _folderScheduler->setupFolders();

//    _statusDialog->setFolderList( _folderScheduler->map() );

    // Check if the update check should be done.
    if( !cfg.ownCloudSkipUpdateCheck() ) {
        QTimer::singleShot( 3000, this, SLOT( slotStartUpdateDetector() ));
    }

    // Catch the SSL problems that could happen during network in ownCloudInfo
    connect( ownCloudInfo::instance(), SIGNAL(sslFailed(QNetworkReply*, QList<QSslError>)),
             this,SLOT(slotSSLFailed(QNetworkReply*, QList<QSslError>)));

    // Validate the connection.
    _conValidator = new ConnectionValidator;
    connect( _conValidator, SIGNAL(connectionResult( ConnectionValidator::Status )),
             this, SLOT(slotConValidatorResult(ConnectionValidator::Status)) );
    _conValidator->checkConnection();

    // qDebug() << "Network Location: " << NetworkLocation::currentLocation().encoded();
}

Application::~Application()
{
    delete _tray; // needed, see ctor
    if( _fileItemDialog) delete _fileItemDialog;
//    if( _statusDialog && ! _helpOnly)  delete _statusDialog;
    delete _conValidator;
    if( _settingsDialog )
        delete _settingsDialog;

    qDebug() << "* Mirall shutdown";
}

void Application::slotStartUpdateDetector()
{
    _updateDetector = new UpdateDetector(this);
    _updateDetector->versionCheck(_theme);
}

void Application::slotUseMonoIconsChanged(bool)
{
    computeOverallSyncStatus();
}

void Application::slotConValidatorResult(ConnectionValidator::Status status)
{
    qDebug() << "Connection Validator Result: " << _conValidator->statusString(status);
    int cnt = 0;
    QString trayMsg, trayHeader;

    setupContextMenu();

    switch( status ) {
    case ConnectionValidator::Undefined:
        qDebug() << "Connection Validator Undefined.";
        break;
    case ConnectionValidator::Connected:
        qDebug() << "Connection Validator Connected.";
        _folderScheduler->setSyncEnabled(true);

        _tray->setIcon( _theme->syncStateIcon( SyncResult::NotYetStarted, true ) );
        _tray->show();

        cnt = _folderScheduler->map().size();
        if( _tray )
            _tray->showMessage(tr("%1 Sync Started").arg(_theme->appNameGUI()),
                               tr("Sync started for %1 configured sync folder(s).").arg(cnt));

        // queue up the sync for all folders.
        _folderScheduler->slotScheduleAllFolders();

        QMetaObject::invokeMethod(_folderScheduler, "slotScheduleFolderSync");

        computeOverallSyncStatus();

//        _actionOpenStatus->setEnabled( true );
        setupContextMenu();
        break;
    case ConnectionValidator::NotConfigured:
        qDebug() << "Connection Validator Not Configured.";
        _owncloudSetupWizard->startWizard(); // Setup with intro
        break;
    case ConnectionValidator::ServerVersionMismatch:
        qDebug() << "Connection Validator ServerVersionMismatch.";
        QMessageBox::warning(0, tr("%1 Server Mismatch").arg(_theme->appNameGUI()),
                             tr("<p>The configured server for this client is too old.</p>"
                                "<p>Please update to the latest %1 server and restart the client.</p>").arg(_theme->appNameGUI()));
        return;
        break;
    case ConnectionValidator::CredentialsTooManyAttempts:
        qDebug() << "Connection Validator Too many attempts.";
        trayMsg = tr("Too many authentication attempts to %1.")
                .arg(Theme::instance()->appNameGUI());
        trayHeader = tr("Credentials");
        break;
    case ConnectionValidator::CredentialError:
        qDebug() << "Connection Validator Credential Error.";
        trayMsg = tr("Error to fetch user credentials to %1. Please check configuration.")
                .arg(Theme::instance()->appNameGUI());
        trayHeader = tr("Credentials");
        break;
    case ConnectionValidator::CredentialsUserCanceled:
        qDebug() << "Connection Validator Credential User Canceled.";
        trayMsg = tr("User canceled authentication request to %1")
                .arg(Theme::instance()->appNameGUI());
        trayHeader = tr("Credentials");
        break;
    case ConnectionValidator::CredentialsWrong:
        qDebug() << "Connection Validator Credentials wrong.";
        trayMsg = tr("%1 user credentials are wrong. Please check configuration.")
                .arg(Theme::instance()->appNameGUI());
        trayHeader = tr("Credentials");
        break;
    case ConnectionValidator::StatusNotFound:
        qDebug() << "Connection Validator Status No Found.";
        // Check again in a while.
        QTimer::singleShot(30000, _conValidator, SLOT(checkConnection()));
        break;
    default:
        qDebug() << "Connection Validator Undefined.";
        break;
    }

    if( !trayMsg.isEmpty() ) {
        _tray->showMessage(trayHeader, trayMsg);
//        _actionOpenStatus->setEnabled( false );
    }
}

void Application::slotSSLFailed( QNetworkReply *reply, QList<QSslError> errors )
{
    qDebug() << "SSL-Warnings happened for url " << reply->url().toString();

    if( ownCloudInfo::instance()->certsUntrusted() ) {
        // User decided once to untrust. Honor this decision.
        qDebug() << "Untrusted by user decision, returning.";
        return;
    }

    QString configHandle = ownCloudInfo::instance()->configHandle(reply);

    // make the ssl dialog aware of the custom config. It loads known certs.
    if( ! _sslErrorDialog ) {
        _sslErrorDialog = new SslErrorDialog;
    }
    _sslErrorDialog->setCustomConfigHandle( configHandle );

    if( _sslErrorDialog->setErrorList( errors ) ) {
        // all ssl certs are known and accepted. We can ignore the problems right away.
        qDebug() << "Certs are already known and trusted, Warnings are not valid.";
        reply->ignoreSslErrors();
    } else {
        if( _sslErrorDialog->exec() == QDialog::Accepted ) {
            if( _sslErrorDialog->trustConnection() ) {
                reply->ignoreSslErrors();
            } else {
                // User does not want to trust.
                ownCloudInfo::instance()->setCertsUntrusted(true);
            }
        } else {
            ownCloudInfo::instance()->setCertsUntrusted(true);
        }
    }
}

void Application::slotownCloudWizardDone( int res )
{
    if( res == QDialog::Accepted ) {
        int cnt = _folderScheduler->setupFolders();
        qDebug() << "Set up " << cnt << " folders.";
//        _statusDialog->setFolderList( _folderScheduler->map() );
    }
    _folderScheduler->setSyncEnabled( true );
    // slotStartFolderSetup( res );
}

void Application::setupActions()
{
    _actionOpenoC = new QAction(tr("Open %1 in browser...").arg(_theme->appNameGUI()), this);
    connect(_actionOpenoC, SIGNAL(triggered(bool)), SLOT(slotOpenOwnCloud()));
    _actionConfigure = new QAction(tr("Configure..."), this);
    connect(_actionConfigure, SIGNAL(triggered(bool)), SLOT(slotConfigure()));
    _actionQuit = new QAction(tr("Quit"), this);
    connect(_actionQuit, SIGNAL(triggered(bool)), SLOT(quit()));
}

void Application::setupSystemTray()
{
    // Setting a parent heres will crash on X11 since by the time qapp runs
    // its childrens dtors, the X11->screen variable queried for is gone -> crash
    _tray = new QSystemTrayIcon;
    _tray->setIcon( _theme->syncStateIcon( SyncResult::NotYetStarted, true ) );

    connect(_tray,SIGNAL(activated(QSystemTrayIcon::ActivationReason)),
            SLOT(slotTrayClicked(QSystemTrayIcon::ActivationReason)));

    setupContextMenu();

    _tray->show();
}

void Application::setupContextMenu()
{
    bool isConfigured = ownCloudInfo::instance()->isConfigured();

    _actionOpenoC->setEnabled(isConfigured);

    if( _contextMenu ) {
        _contextMenu->clear();
    } else {
        _contextMenu = new QMenu();
        // this must be called only once after creating the context menu, or
        // it will trigger a bug in Ubuntu's SNI bridge patch (11.10, 12.04).
        _tray->setContextMenu(_contextMenu);
    }
    _contextMenu->setTitle(_theme->appNameGUI() );
//    _contextMenu->addAction(_actionOpenStatus);
    _contextMenu->addAction(_actionOpenoC);

    _contextMenu->addSeparator();

    int folderCnt = _folderScheduler->map().size();
    // add open actions for all sync folders to the tray menu
    if( _theme->singleSyncFolder() ) {
        if( folderCnt != 0 ) {
            // there should be exactly one folder. No sync-folder add action will be shown.
            QStringList li = _folderScheduler->map().keys();
            if( li.size() == 1 ) {
                Folder *folder = _folderScheduler->map().value(li.first());
                if( folder ) {
                    // if there is singleFolder mode, a generic open action is displayed.
                    QAction *action = new QAction( tr("Open %1 folder").arg(_theme->appNameGUI()), this);
                    action->setIcon( _theme->trayFolderIcon( folder->backend()) );

                    connect( action, SIGNAL(triggered()),_folderOpenActionMapper,SLOT(map()));
                    _folderOpenActionMapper->setMapping( action, folder->alias() );

                    _contextMenu->addAction(action);
                }
            }
        }
    } else {
        // show a grouping with more than one folder.
        if ( folderCnt ) {
            _contextMenu->addAction(tr("Managed Folders:"))->setDisabled(true);
        }
        foreach (Folder *folder, _folderScheduler->map() ) {
            QAction *action = new QAction( folder->alias(), this );
            action->setIcon( _theme->trayFolderIcon( folder->backend()) );

            connect( action, SIGNAL(triggered()),_folderOpenActionMapper,SLOT(map()));
            _folderOpenActionMapper->setMapping( action, folder->alias() );

            _contextMenu->addAction(action);
        }
    }

    _contextMenu->addSeparator();
    _contextMenu->addAction(_actionConfigure);
    _contextMenu->addSeparator();

    _contextMenu->addAction(_actionQuit);
}

void Application::setupLogBrowser()
{
    // might be called from second instance
    if (!_logBrowser) {
        // init the log browser.
        _logBrowser = new LogBrowser;
        qInstallMsgHandler( mirallLogCatcher );
        // ## TODO: allow new log name maybe?
        if (!_logFile.isEmpty()) {
            qDebug() << "Logging into logfile: " << _logFile << " with flush " << _logFlush;
            _logBrowser->setLogFile( _logFile, _logFlush );
        }
    }

    if (_showLogWindow)
        slotOpenLogBrowser();

    qDebug() << QString::fromLatin1( "################## %1 %2 (%3) %4").arg(_theme->appName())
                .arg( QLocale::system().name() )
                .arg(property("ui_lang").toString())
                .arg(_theme->version());

}

void Application::setupProxy()
{
    //
    Mirall::MirallConfigFile cfg;
    int proxy = cfg.proxyType();

    switch(proxy) {
    case QNetworkProxy::NoProxy: {
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
        break;
    }
    case QNetworkProxy::DefaultProxy: {
        QNetworkProxyFactory::setUseSystemConfiguration(true);
        break;
    }

    case QNetworkProxy::Socks5Proxy: {
        proxy = QNetworkProxy::HttpProxy;
        cfg.setProxyType(proxy);
        // fall through
    }
    case QNetworkProxy::HttpProxy:{
        QNetworkProxy proxy;
        proxy.setType(QNetworkProxy::HttpProxy);
        proxy.setHostName(cfg.proxyHostName());
        proxy.setPort(cfg.proxyPort());
        proxy.setUser(cfg.proxyUser());
        proxy.setPassword(cfg.proxyPassword());
        QNetworkProxy::setApplicationProxy(proxy);
        break;
    }
    }
}

/*
 * open the folder with the given Alais
 */
void Application::slotFolderOpenAction( const QString& alias )
{
    Folder *f = _folderScheduler->folder(alias);
    qDebug() << "opening local url " << f->path();
    if( f ) {
        QUrl url(f->path(), QUrl::TolerantMode);
        url.setScheme( QLatin1String("file") );

#ifdef Q_OS_WIN32
        // work around a bug in QDesktopServices on Win32, see i-net
        QString filePath = f->path();

        if (filePath.startsWith(QLatin1String("\\\\")) || filePath.startsWith(QLatin1String("//")))
            url.setUrl(QDir::toNativeSeparators(filePath));
        else
            url = QUrl::fromLocalFile(filePath);
#endif
        QDesktopServices::openUrl(url);
    }
}

void Application::slotOpenOwnCloud()
{
  MirallConfigFile cfgFile;

  QString url = cfgFile.ownCloudUrl();
  QDesktopServices::openUrl( url );
}

void Application::slotTrayClicked( QSystemTrayIcon::ActivationReason reason )
{
    // A click on the tray icon should only open the status window on Win and
    // Linux, not on Mac. They want a menu entry.
    // If the user canceled login, rather open the login window.
    if( CredentialStore::instance()->state() == CredentialStore::UserCanceled ||
            CredentialStore::instance()->state() == CredentialStore::Error ) {
        _conValidator->checkConnection();
    }
#if defined Q_WS_WIN || defined Q_WS_X11
//    if( reason == QSystemTrayIcon::Trigger && _actionOpenStatus->isEnabled() ) {
//        slotOpenStatus();
//    }
#endif
}

//void Application::slotOpenStatus()
//{
//  if( ! _statusDialog ) return;

//  QWidget *raiseWidget = 0;

//  // check if there is a mirall.cfg already.
//  if( _owncloudSetupWizard->wizard()->isVisible() ) {
//    raiseWidget = _owncloudSetupWizard->wizard();
//  }

//  // if no config file is there, start the configuration wizard.
//  if( ! raiseWidget ) {
//    MirallConfigFile cfgFile;
//    if( !cfgFile.exists() ) {
//      qDebug() << "No configured folders yet, start the Owncloud integration dialog.";
//      _folderScheduler->setSyncEnabled(false);
//      _owncloudSetupWizard->startWizard();
//    } else {
//      qDebug() << "#============# Status dialog starting #=============#";
//      raiseWidget = _statusDialog;
//      _statusDialog->setFolderList( _folderScheduler->map() );
//    }
//  }
//  raiseDialog( raiseWidget );
//}

void Application::raiseDialog( QWidget *raiseWidget )
{
  // viel hilft viel ;-)
  if( raiseWidget ) {
#if defined(Q_WS_WIN) || defined (Q_OS_MAC)
    Qt::WindowFlags eFlags = raiseWidget->windowFlags();
    eFlags |= Qt::WindowStaysOnTopHint;
    raiseWidget->setWindowFlags(eFlags);
    raiseWidget->show();
    eFlags &= ~Qt::WindowStaysOnTopHint;
    raiseWidget->setWindowFlags(eFlags);
#endif
    raiseWidget->show();
    raiseWidget->raise();
    raiseWidget->activateWindow();
  }
}

void Application::slotOpenLogBrowser()
{
    _logBrowser->show();
    _logBrowser->raise();
}

void Application::slotAbout()
{
    QMessageBox::about(0, tr("About %1").arg(_theme->appNameGUI()),
                       Theme::instance()->about());
}

/*
  * the folder is to be removed. The slot is called from a signal emitted by
  * the status dialog, which removes the folder from its list by itself.
  */
void Application::slotRemoveFolder( const QString& alias )
{
    int ret = QMessageBox::question( 0, tr("Confirm Folder Remove"),
                                     tr("Do you really want to remove upload folder <i>%1</i>?").arg(alias),
                                     QMessageBox::Yes|QMessageBox::No );

    if( ret == QMessageBox::No ) {
        return;
    }

    _folderScheduler->slotRemoveFolder( alias );
//    _statusDialog->slotRemoveSelectedFolder( );
    computeOverallSyncStatus();
    setupContextMenu();
}

// Open the File list info dialog.
void Application::slotInfoFolder( const QString& alias )
{
    qDebug() << "details of folder with alias " << alias;

    if( !_fileItemDialog ) {
        _fileItemDialog = new FileItemDialog(_theme);
    }

    SyncResult folderResult = _folderScheduler->syncResult( alias );

    _fileItemDialog->setSyncResult( folderResult );
    raiseDialog( _fileItemDialog );
}

void Application::slotEnableFolder(const QString& alias, const bool enable)
{
    qDebug() << "Application: enable folder with alias " << alias;
    bool terminate = false;

    // this sets the folder status to disabled but does not interrupt it.
    Folder *f = _folderScheduler->folder( alias );
    if( f && !enable ) {
        // check if a sync is still running and if so, ask if we should terminate.
        if( f->isBusy() ) { // its still running
            int reply = QMessageBox::question( 0, tr("Sync Running"),
                                               tr("The syncing operation is running.<br/>Do you want to terminate it?"),
                                               QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes );
            if ( reply == QMessageBox::Yes )
                terminate = true;
            else
                return; // do nothing
        }
    }

    // message box can return at any time while the thread keeps running,
    // so better check again after the user has responded.
    if ( f->isBusy() && terminate )
        _folderScheduler->terminateSyncProcess( alias );

    _folderScheduler->slotEnableFolder( alias, enable );
//    _statusDialog->slotUpdateFolderState( f );
}

void Application::slotConfigure()
{
    if (!_settingsDialog)
        _settingsDialog = new SettingsDialog;
    _settingsDialog->open();
}

void Application::slotParseOptions(const QString &opts)
{
    QStringList options = opts.split(QLatin1Char('|'));
    parseOptions(options);
    setupLogBrowser();
}

void Application::slotShowTrayMessage(const QString &title, const QString &msg)
{
    _tray->showMessage(title, msg);
}

void Application::slotSyncStateChange( const QString& alias )
{
    SyncResult result = _folderScheduler->syncResult( alias );

    // _statusDialog->slotUpdateFolderState( _folderScheduler->folder(alias) );

    if( _fileItemDialog && _fileItemDialog->isVisible() ) {
        _fileItemDialog->setSyncResult( _folderScheduler->syncResult(alias) );
    }
    computeOverallSyncStatus();

    qDebug() << "Sync state changed for folder " << alias << ": "  << result.statusString();
}

void Application::parseOptions(const QStringList &options)
{
    QStringListIterator it(options);
    // skip file name;
    if (it.hasNext()) it.next();
    //parse options; if help or bad option exit
    while (it.hasNext()) {
        QString option = it.next();
       	if (option == QLatin1String("--help") || option == QLatin1String("-h")) {
            setHelp();
            break;
        } else if (option == QLatin1String("--logwindow") ||
                option == QLatin1String("-l")) {
            _showLogWindow = true;
        } else if (option == QLatin1String("--logfile")) {
            if (it.hasNext() && !it.peekNext().startsWith(QLatin1String("--"))) {
                _logFile = it.next();
            } else {
                setHelp();
            }
        } else if (option == QLatin1String("--logflush")) {
            _logFlush = true;
	} else {
	    setHelp();
	    std::cout << "Option not recognized:  " << option.toStdString() << std::endl;
	    break;
	}
    }
}

void Application::computeOverallSyncStatus()
{

    // display the info of the least successful sync (eg. not just display the result of the latest sync
    SyncResult overallResult(SyncResult::Undefined );
    QString trayMessage;
    Folder::Map map = _folderScheduler->map();

    QStringList allStatusStrings;

    foreach ( Folder *syncedFolder, map.values() ) {
        QString folderMessage;
        SyncResult folderResult = syncedFolder->syncResult();
        SyncResult::Status syncStatus = folderResult.status();

        switch( syncStatus ) {
        case SyncResult::Undefined:
            if ( overallResult.status() != SyncResult::Error ) {
                overallResult.setStatus(SyncResult::Error);
            }
            folderMessage = tr( "Undefined State." );
            break;
        case SyncResult::NotYetStarted:
            folderMessage = tr( "Waits to start syncing." );
            overallResult.setStatus( SyncResult::NotYetStarted );
            break;
        case SyncResult::SyncPrepare:
            folderMessage = tr( "Preparing for sync." );
            overallResult.setStatus( SyncResult::SyncPrepare );
            break;
        case SyncResult::SyncRunning:
            folderMessage = tr( "Sync is running." );
            overallResult.setStatus( SyncResult::SyncRunning );
            break;
        case SyncResult::Unavailable:
            folderMessage = tr( "Server is currently not available." );
            overallResult.setStatus( SyncResult::Unavailable );
            break;
        case SyncResult::Success:
            if( overallResult.status() == SyncResult::Undefined ) {
                folderMessage = tr( "Last Sync was successful." );
                overallResult.setStatus( SyncResult::Success );
            }
            break;
        case SyncResult::Error:
            overallResult.setStatus( SyncResult::Error );
            folderMessage = tr( "Syncing Error." );
            break;
        case SyncResult::SetupError:
            if ( overallResult.status() != SyncResult::Error ) {
                overallResult.setStatus( SyncResult::SetupError );
            }
            folderMessage = tr( "Setup Error." );
            break;
        default:
            folderMessage = tr( "Undefined Error State." );
            overallResult.setStatus( SyncResult::Error );
        }
        if( !syncedFolder->syncEnabled() ) {
            // sync is disabled.
            folderMessage += tr( " (Sync is paused)" );
        }

        qDebug() << "Folder in overallStatus Message: " << syncedFolder << " with name " << syncedFolder->alias();
        allStatusStrings += QString::fromLatin1("Folder %1: %2").arg(syncedFolder->alias()).arg(folderMessage);

    }

    // create the tray blob message, check if we have an defined state
    if( overallResult.status() != SyncResult::Undefined ) {
        if( ! allStatusStrings.isEmpty() )
            trayMessage = allStatusStrings.join(QLatin1String("\n"));
        else
            trayMessage = tr("No sync folders configured.");

        QIcon statusIcon = _theme->syncStateIcon( overallResult.status(), true); // size 48 before

        _tray->setIcon( statusIcon );
        _tray->setToolTip(trayMessage);
    }
}

void Application::showHelp()
{
    setHelp();
    std::cout << _theme->appName().toLatin1().constData() << " version " <<
                 _theme->version().toLatin1().constData() << std::endl << std::endl;
    std::cout << "File synchronisation desktop utility." << std::endl << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -h --help            : show this help screen." << std::endl;
    std::cout << "  --logwindow          : open a window to show log output." << std::endl;
    std::cout << "  --logfile <filename> : write log output to file <filename>." << std::endl;
    std::cout << "  --logflush           : flush the log file after every write." << std::endl;
    std::cout << std::endl;
    if (_theme->appName() == QLatin1String("ownCloud"))
        std::cout << "For more information, see http://www.owncloud.org" << std::endl;
}

void Application::setHelp()
{
    _helpOnly = true;
}

QString substLang(const QString &lang)
{
    // Map the more apropriate script codes
    // to country codes as used by Qt and
    // transifex translation conventions.

    // Simplified Chinese
    if (lang == QLatin1String("zh_Hans"))
        return QLatin1String("zh_CN");
    // Traditional Chinese
    if (lang == QLatin1String("zh_Hant"))
        return QLatin1String("zh_TW");
    return lang;
}

void Application::setupTranslations()
{
    QStringList uiLanguages;
    // uiLanguages crashes on Windows with 4.8.0 release builds
    #if (QT_VERSION >= 0x040801) || (QT_VERSION >= 0x040800 && !defined(Q_OS_WIN))
        uiLanguages = QLocale::system().uiLanguages();
    #else
        // older versions need to fall back to the systems locale
        uiLanguages << QLocale::system().name();
    #endif

    QString enforcedLocale = Theme::instance()->enforcedLocale();
    if (!enforcedLocale.isEmpty())
        uiLanguages.prepend(enforcedLocale);

    QTranslator *translator = new QTranslator(this);
    QTranslator *qtTranslator = new QTranslator(this);
    QTranslator *qtkeychainTranslator = new QTranslator(this);

    foreach(QString lang, uiLanguages) {
        lang.replace(QLatin1Char('-'), QLatin1Char('_')); // work around QTBUG-25973
        lang = substLang(lang);
        const QString trPath = applicationTrPath();
        const QString trFile = QLatin1String("mirall_") + lang;
        if (translator->load(trFile, trPath) ||
            lang.startsWith(QLatin1String("en"))) {
            // Permissive approach: Qt and keychain translations
            // may be missing, but Qt translations must be there in order
            // for us to accept the language. Otherwise, we try with the next.
            // "en" is an exeption as it is the default language and may not
            // have a translation file provided.
            qDebug() << Q_FUNC_INFO << "Using" << lang << "translation";
            setProperty("ui_lang", lang);
            const QString qtTrPath = QLibraryInfo::location(QLibraryInfo::TranslationsPath);
            const QString qtTrFile = QLatin1String("qt_") + lang;
            if (qtTranslator->load(qtTrFile, qtTrPath)) {
                qtTranslator->load(qtTrFile, trPath);
            }
            const QString qtkeychainFile = QLatin1String("qt_") + lang;
            if (!qtkeychainTranslator->load(qtkeychainFile, qtTrPath)) {
               qtkeychainTranslator->load(qtkeychainFile, trPath);
            }
            if (!translator->isEmpty())
                installTranslator(translator);
            if (!qtTranslator->isEmpty())
                installTranslator(qtTranslator);
            if (!qtkeychainTranslator->isEmpty())
                installTranslator(qtkeychainTranslator);
            break;
        }
        if (property("ui_lang").isNull())
            setProperty("ui_lang", "C");
    }
}

bool Application::giveHelp()
{
    return _helpOnly;
}
} // namespace Mirall

