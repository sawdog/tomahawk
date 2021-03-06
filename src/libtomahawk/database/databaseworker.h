/* === This file is part of Tomahawk Player - <http://tomahawk-player.org> ===
 *
 *   Copyright 2010-2011, Christian Muehlhaeuser <muesli@tomahawk-player.org>
 *
 *   Tomahawk is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Tomahawk is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Tomahawk. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DATABASEWORKER_H
#define DATABASEWORKER_H

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QList>
#include <QSharedPointer>

#include <qjson/parser.h>
#include <qjson/serializer.h>
#include <qjson/qobjecthelper.h>

#include "databasecommand.h"

class Database;
class DatabaseCommandLoggable;

class DatabaseWorker : public QThread
{
Q_OBJECT

public:
    DatabaseWorker( DatabaseImpl*, Database*, bool mutates );
    ~DatabaseWorker();

    bool busy() const { return m_outstanding > 0; }
    unsigned int outstandingJobs() const { return m_outstanding; }

public slots:
    void enqueue( const QSharedPointer<DatabaseCommand>& );

protected:
    void run();

private slots:
    void doWork();

private:
    void logOp( DatabaseCommandLoggable* command );

    QMutex m_mut;
    DatabaseImpl* m_dbimpl;
    QList< QSharedPointer<DatabaseCommand> > m_commands;
    int m_outstanding;

    QJson::Serializer m_serializer;
};

#endif // DATABASEWORKER_H
