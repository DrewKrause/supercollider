/*
    SuperCollider Qt IDE
    Copyright (c) 2012 Jakob Leben & Tim Blechmann
    http://www.audiosynth.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include "main.hpp"
#include "settings/manager.hpp"
#include "session_manager.hpp"
#include "../widgets/main_window.hpp"
#include "../widgets/code_editor/highlighter.hpp"
#include "SC_DirUtils.h"

#include <QApplication>
#include <QAction>
#include <QBuffer>
#include <QDataStream>
#include <QDir>
#include <QFileOpenEvent>
#include <QLibraryInfo>
#include <QTranslator>

using namespace ScIDE;

int main( int argc, char *argv[] )
{
    QApplication app(argc, argv);

    QStringList arguments (QApplication::arguments());
    arguments.pop_front(); // application path

    SingleInstanceGuard guard;
    if (guard.tryConnect(arguments))
        return 0;

    QTranslator qtTranslator;
    qtTranslator.load("qt_" + QLocale::system().name(), QLibraryInfo::location(QLibraryInfo::TranslationsPath));
    app.installTranslator(&qtTranslator);

    QTranslator scideTranslator;
    scideTranslator.load("scide_" + QLocale::system().name());
    app.installTranslator(&scideTranslator);

    Main * main = Main::instance();

    MainWindow *win = new MainWindow(main);

    // NOTE: load session after GUI is created, so that GUI can respond
    QString lastSession = main->sessionManager()->lastSession();
    if (!lastSession.isEmpty()) {
        main->sessionManager()->openSession(lastSession);
        win->show();
    }
    else {
        main->sessionManager()->openSession("Default");
        win->showMaximized();
    }

    foreach (QString argument, arguments) {
        main->documentManager()->open(argument);
    }

    bool startInterpreter = main->settings()->value("IDE/interpreter/autoStart").toBool();
    if (startInterpreter)
        main->scProcess()->startLanguage();

    return app.exec();
}


bool SingleInstanceGuard::tryConnect(QStringList const & arguments)
{
    const int maxNumberOfInstances = 128;
    if (!arguments.empty()) {
        for (int socketID = 0; socketID != maxNumberOfInstances; ++socketID) {
            QString serverName = QString("SuperColliderIDE_Singleton_%1").arg(socketID);
            QSharedPointer<QLocalSocket> socket (new QLocalSocket(this));
            socket->connectToServer(serverName);

            if (socket->waitForConnected(200)) {
                QDataStream stream(socket.data());
                stream.setVersion(QDataStream::Qt_4_6);

                stream << QString("open");
                stream << arguments;
                socket->flush();
                return true;
            }
        }
    }

    mIpcServer = new QLocalServer(this);
    for (int socketID = 0; socketID != maxNumberOfInstances; ++socketID) {
        QString serverName = QString("SuperColliderIDE_Singleton_%1").arg(socketID);

        bool listening = mIpcServer->listen(serverName);
        if (listening) {
            connect(mIpcServer, SIGNAL(newConnection()), this, SLOT(onNewIpcConnection()));
            return false;
        }
    }
    return false;
}

void SingleInstanceGuard::onIpcData()
{
    QByteArray ipcData = mIpcSocket->readAll();

    QBuffer receivedData ( &ipcData );
    receivedData.open ( QIODevice::ReadOnly );

    QDataStream in ( &receivedData );
    in.setVersion ( QDataStream::Qt_4_6 );
    QString id;
    in >> id;
    if ( in.status() != QDataStream::Ok )
        return;

    QStringList message;
    in >> message;
    if ( in.status() != QDataStream::Ok )
        return;

    if (id == QString("open")) {
        foreach (QString path, message)
            Main::instance()->documentManager()->open(path);
    }
}


static QString getSettingsFile()
{
    char config_dir[PATH_MAX];
    sc_GetUserConfigDirectory(config_dir, PATH_MAX);
    return QString(config_dir) + SC_PATH_DELIMITER + "sc_ide_conf.yaml";
}

// NOTE: mSettings must be the first to initialize,
// because other members use it!

Main::Main(void) :
    mSettings( new Settings::Manager( getSettingsFile(), this ) ),
    mSCResponder( new ScResponder(this) ),
    mSCProcess( new SCProcess(this) ),
    mSCServer( new ScServer(this) ),
    mDocManager( new DocumentManager(this) ),
    mSessionManager( new SessionManager(mDocManager, this) )
{
    new SyntaxHighlighterGlobals(this);

    connect(mSCProcess, SIGNAL(response(QString,QString)), mSCResponder, SLOT(onResponse(QString,QString)));
    connect(mSCResponder, SIGNAL(serverRunningChanged(bool,QString,int)), mSCServer, SLOT(onServerRunningChanged(bool,QString,int)));

    qApp->installEventFilter(this);
}

void Main::quit() {
    mSessionManager->saveSession();
    storeSettings();
    QApplication::quit();
}

bool Main::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::FileOpen) {
        // open the file dragged onto the application icon on Mac
        QFileOpenEvent *openEvent = static_cast<QFileOpenEvent*>(event);
        mDocManager->open(openEvent->file());
        return true;
    } else
        return QObject::eventFilter(object, event);
}
