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

#include "databasecommand_addfiles.h"

#include <QSqlQuery>

#include "artist.h"
#include "album.h"
#include "collection.h"
#include "database/database.h"
#include "databasecommand_collectionstats.h"
#include "databaseimpl.h"
#include "network/dbsyncconnection.h"
#include "network/servent.h"
#include "sourcelist.h"

#include "utils/logger.h"

using namespace Tomahawk;


// remove file paths when making oplog/for network transmission
QVariantList
DatabaseCommand_AddFiles::files() const
{
    QVariantList list;
    foreach ( const QVariant& v, m_files )
    {
        // replace url with the id, we don't leak file paths over the network.
        QVariantMap m = v.toMap();
        m.remove( "url" );
        m.insert( "url", QString::number( m.value( "id" ).toInt() ) );
        list.append( m );
    }
    return list;
}


// After changing a collection, we need to tell other bits of the system:
void
DatabaseCommand_AddFiles::postCommitHook()
{
    if ( source().isNull() || source()->collection().isNull() )
    {
        qDebug() << "Source has gone offline, not emitting to GUI.";
        return;
    }

    // make the collection object emit its tracksAdded signal, so the
    // collection browser will update/fade in etc.
    Collection* coll = source()->collection().data();

    connect( this, SIGNAL( notify( QList<unsigned int> ) ),
             coll, SLOT( setTracks( QList<unsigned int> ) ),
             Qt::QueuedConnection );

    emit notify( m_ids );

    if ( source()->isLocal() )
    {
        Servent::instance()->triggerDBSync();

        // Re-calculate local db stats
        DatabaseCommand_CollectionStats* cmd = new DatabaseCommand_CollectionStats( SourceList::instance()->getLocal() );
        connect( cmd, SIGNAL( done( QVariantMap ) ),
                 SourceList::instance()->getLocal().data(), SLOT( setStats( QVariantMap ) ), Qt::QueuedConnection );
        Database::instance()->enqueue( QSharedPointer<DatabaseCommand>( cmd ) );
    }
}


void
DatabaseCommand_AddFiles::exec( DatabaseImpl* dbi )
{
    qDebug() << Q_FUNC_INFO;
    Q_ASSERT( !source().isNull() );

    TomahawkSqlQuery query_file = dbi->newquery();
    TomahawkSqlQuery query_filejoin = dbi->newquery();
    TomahawkSqlQuery query_trackattr = dbi->newquery();
    TomahawkSqlQuery query_file_del = dbi->newquery();

    query_file.prepare( "INSERT INTO file(source, url, size, mtime, md5, mimetype, duration, bitrate) VALUES (?, ?, ?, ?, ?, ?, ?, ?)" );
    query_filejoin.prepare( "INSERT INTO file_join(file, artist, album, track, albumpos) VALUES (?, ?, ?, ?, ?)" );
    query_trackattr.prepare( "INSERT INTO track_attributes(id, k, v) VALUES (?, ?, ?)" );
    query_file_del.prepare( QString( "DELETE FROM file WHERE source %1 AND url = ?" )
                               .arg( source()->isLocal() ? "IS NULL" : QString( "= %1" ).arg( source()->id() ) ) );

    int added = 0;
    QVariant srcid = source()->isLocal() ? QVariant( QVariant::Int ) : source()->id();
    qDebug() << "Adding" << m_files.length() << "files to db for source" << srcid;

    QList<QVariant>::iterator it;
    for( it = m_files.begin(); it != m_files.end(); ++it )
    {
        QVariant& v = *it;
        QVariantMap m = v.toMap();

        QString url      = m.value( "url" ).toString();
        int mtime        = m.value( "mtime" ).toInt();
        uint size        = m.value( "size" ).toUInt();
        QString hash     = m.value( "hash" ).toString();
        QString mimetype = m.value( "mimetype" ).toString();
        uint duration    = m.value( "duration" ).toUInt();
        uint bitrate     = m.value( "bitrate" ).toUInt();
        QString artist   = m.value( "artist" ).toString();
        QString album    = m.value( "album" ).toString();
        QString track    = m.value( "track" ).toString();
        uint albumpos    = m.value( "albumpos" ).toUInt();
        int year         = m.value( "year" ).toInt();

        int fileid = 0, artistid = 0, albumid = 0, trackid = 0;
        query_file_del.bindValue( 0, url );
        query_file_del.exec();

        query_file.bindValue( 0, srcid );
        query_file.bindValue( 1, url );
        query_file.bindValue( 2, size );
        query_file.bindValue( 3, mtime );
        query_file.bindValue( 4, hash );
        query_file.bindValue( 5, mimetype );
        query_file.bindValue( 6, duration );
        query_file.bindValue( 7, bitrate );
        if( !query_file.exec() )
        {
            qDebug() << "Failed to insert to file:"
                     << query_file.lastError().databaseText()
                     << query_file.lastError().driverText()
                     << query_file.boundValues();
            continue;
        }
        else
        {
            if( added % 1000 == 0 )
                qDebug() << "Inserted" << added;
        }
        // get internal IDs for art/alb/trk
        fileid = query_file.lastInsertId().toInt();
        m.insert( "id", fileid );
        // this is the qvariant(map) the remote will get
        v = m;

        if( !source()->isLocal() )
            url = QString( "servent://%1\t%2" ).arg( source()->userName() ).arg( url );

        artistid = dbi->artistId( artist, true );
        if ( artistid < 1 )
            continue;
        trackid = dbi->trackId( artistid, track, true );
        if ( trackid < 1 )
            continue;
        albumid = dbi->albumId( artistid, album, true );

        // Now add the association
        query_filejoin.bindValue( 0, fileid );
        query_filejoin.bindValue( 1, artistid );
        query_filejoin.bindValue( 2, albumid > 0 ? albumid : QVariant( QVariant::Int ) );
        query_filejoin.bindValue( 3, trackid );
        query_filejoin.bindValue( 4, albumpos );
        if ( !query_filejoin.exec() )
        {
            qDebug() << "Error inserting into file_join table";
            continue;
        }

        query_trackattr.bindValue( 0, trackid );
        query_trackattr.bindValue( 1, "releaseyear" );
        query_trackattr.bindValue( 2, year );
        query_trackattr.exec();

/*        QVariantMap attr;
        Tomahawk::query_ptr query = Tomahawk::Query::get( artist, track, album );
        attr["releaseyear"] = m.value( "year" );

        Tomahawk::artist_ptr artistptr = Tomahawk::Artist::get( artistid, artist );
        Tomahawk::album_ptr albumptr = Tomahawk::Album::get( albumid, album, artistptr );
        Tomahawk::result_ptr result = Tomahawk::Result::get( url );
        result->setModificationTime( mtime );
        result->setSize( size );
        result->setMimetype( mimetype );
        result->setDuration( duration );
        result->setBitrate( bitrate );
        result->setArtist( artistptr );
        result->setAlbum( albumptr );
        result->setTrack( track );
        result->setAlbumPos( albumpos );
        result->setAttributes( attr );
        result->setCollection( source()->collection() );
        result->setScore( 1.0 );
        result->setId( trackid );

        QList<Tomahawk::result_ptr> results;
        results << result;
        query->addResults( results );

        m_queries << query;*/

        m_ids << fileid;
        added++;
    }
    qDebug() << "Inserted" << added << "tracks to database";

    if ( added )
        source()->updateIndexWhenSynced();

    qDebug() << "Committing" << added << "tracks...";
    emit done( m_files, source()->collection() );
}
