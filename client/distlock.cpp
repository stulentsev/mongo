// @file distlock.h

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "pch.h"
#include "dbclient.h"
#include "distlock.h"

namespace mongo {

    LabeledLevel DistributedLock::logLvl( 1 );

    ThreadLocalValue<string> distLockIds("");

    /* ==================
     * Module initialization
     */

    boost::once_flag _init = BOOST_ONCE_INIT;
    static string* _cachedProcessString = NULL;

    static void initModule() {
        // cache process string
        stringstream ss;
        ss << getHostName() << ":" << cmdLine.port << ":" << time(0) << ":" << rand();
        _cachedProcessString = new string( ss.str() );
    }

    /* =================== */

    string getDistLockProcess() {
        boost::call_once( initModule, _init );
        assert( _cachedProcessString );
        return *_cachedProcessString;
    }

    string getDistLockId() {
        string s = distLockIds.get();
        if ( s.empty() ) {
            stringstream ss;
            ss << getDistLockProcess() << ":" << getThreadName() << ":" << rand();
            s = ss.str();
            distLockIds.set( s );
        }
        return s;
    }


    class DistributedLockPinger {
    public:

        DistributedLockPinger()
            : _mutex( "DistributedLockPinger" ) {
        }

        void _distLockPingThread( ConnectionString addr, string process, unsigned long long sleepTime ) {

            setThreadName( "LockPinger" );

            string pingId = pingThreadId( addr, process );

            log( DistributedLock::logLvl - 1 ) << "creating distributed lock ping thread for " << addr
                                               << " and process " << process
                                               << " (sleeping for " << sleepTime << "ms)" << endl;

            static int loops = 0;
            while( ! inShutdown() && ! shouldKill( addr, process ) ) {

                log( DistributedLock::logLvl + 2 ) << "distributed lock pinger '" << pingId << "' about to ping." << endl;

                Date_t pingTime;

                try {
                    ScopedDbConnection conn( addr );

                    pingTime = jsTime();

                    // refresh the entry corresponding to this process in the lockpings collection
                    conn->update( DistributedLock::lockPingNS ,
                                  BSON( "_id" << process ) ,
                                  BSON( "$set" << BSON( "ping" << pingTime ) ) ,
                                  true );

                    string err = conn->getLastError();
                    if ( ! err.empty() ) {
                        warning() << "pinging failed for distributed lock pinger '" << pingId << "'."
                                  << m_error_message(err) << endl;
                        conn.done();

                        // Sleep for normal ping time
                        sleepmillis(sleepTime);
                        continue;
                    }

                    // remove really old entries from the lockpings collection if they're not holding a lock
                    // (this may happen if an instance of a process was taken down and no new instance came up to
                    // replace it for a quite a while)
                    // if the lock is taken, the take-over mechanism should handle the situation
                    auto_ptr<DBClientCursor> c = conn->query( DistributedLock::locksNS , BSONObj() );
                    vector<string> pids;
                    while ( c->more() ) {
                        BSONObj lock = c->next();
                        if ( ! lock["process"].eoo() ) {
                            pids.push_back( lock["process"].valuestrsafe() );
                        }
                    }

                    Date_t fourDays = pingTime - ( 4 * 86400 * 1000 ); // 4 days
                    conn->remove( DistributedLock::lockPingNS , BSON( "_id" << BSON( "$nin" << pids ) << "ping" << LT << fourDays ) );
                    err = conn->getLastError();
                    if ( ! err.empty() ) {
                        warning() << "ping cleanup for distributed lock pinger '" << pingId << " failed."
                                  << m_error_message(err) << endl;
                        conn.done();

                        // Sleep for normal ping time
                        sleepmillis(sleepTime);
                        continue;
                    }

                    // create index so remove is fast even with a lot of servers
                    if ( loops++ == 0 ) {
                        conn->ensureIndex( DistributedLock::lockPingNS , BSON( "ping" << 1 ) );
                    }

                    conn.done();

                    log( DistributedLock::logLvl - ( loops % 10 == 0 ? 1 : 0 ) ) << "cluster " << addr << " pinged successfully at " << pingTime
                            << " by distributed lock pinger '" << pingId
                            << "', sleeping for " << sleepTime << "ms" << endl;
                }
                catch ( std::exception& e ) {
                    warning() << "distributed lock pinger '" << pingId << "' detected an exception while pinging."
                              << m_error_message(e.what()) << endl;
                }

                sleepmillis(sleepTime);
            }

            warning() << "removing distributed lock ping thread '" << pingId << "'" << endl;


            if( shouldKill( addr, process ) )
                finishKill( addr, process );

        }

        void distLockPingThread( ConnectionString addr, long long clockSkew, string processId, unsigned long long sleepTime ) {
            try {
                jsTimeVirtualThreadSkew( clockSkew );
                _distLockPingThread( addr, processId, sleepTime );
            }
            catch ( std::exception& e ) {
                error() << "unexpected error while running distributed lock pinger for " << addr << ", process " << processId << m_error_message(e.what()) << endl;
            }
            catch ( ... ) {
                error() << "unknown error while running distributed lock pinger for " << addr << ", process " << processId << endl;
            }
        }

        string pingThreadId( const ConnectionString& conn, const string& processId ) {
            return conn.toString() + "/" + processId;
        }

        string got( DistributedLock& lock, unsigned long long sleepTime ) {

            // Make sure we don't start multiple threads for a process id
            scoped_lock lk( _mutex );

            const ConnectionString& conn = lock.getRemoteConnection();
            const string& processId = lock.getProcessId();
            string s = pingThreadId( conn, processId );

            // Ignore if we already have a pinging thread for this process.
            if ( _seen.count( s ) > 0 ) return "";

            // Check our clock skew
            try {
                if( lock.isRemoteTimeSkewed() ) {
                    m_throw_exception(13650, LockException, "clock skew of the cluster " << conn.toString() << " is too far out of bounds to allow distributed locking.");
                }
            }
            catch( LockException& e) {
                m_chain_exception(13651, e, LockException, "error checking clock skew of cluster " << conn.toString());
            }

            boost::thread t( boost::bind( &DistributedLockPinger::distLockPingThread, this, conn, getJSTimeVirtualThreadSkew(), processId, sleepTime) );

            _seen.insert( s );

            return s;
        }

        void kill( ConnectionString& conn, string& processId ) {
            // Make sure we're in a consistent state before other threads can see us
            scoped_lock lk( _mutex );

            string pingId = pingThreadId( conn, processId );

            assert( _seen.count( pingId ) > 0 );
            _kill.insert( pingId );

        }

        bool shouldKill( ConnectionString& conn, string& processId ) {
            return _kill.count( pingThreadId( conn, processId ) ) > 0;
        }

        void finishKill( ConnectionString& conn, string& processId ) {
            // Make sure we're in a consistent state before other threads can see us
            scoped_lock lk( _mutex );

            string pingId = pingThreadId( conn, processId );

            _kill.erase( pingId );
            _seen.erase( pingId );

        }

        set<string> _kill;
        set<string> _seen;
        mongo::mutex _mutex;

    } distLockPinger;


    const string DistributedLock::lockPingNS = "config.lockpings";
    const string DistributedLock::locksNS = "config.locks";

    /**
     * Create a new distributed lock, potentially with a custom sleep and takeover time.  If a custom sleep time is
     * specified (time between pings)
     */
    DistributedLock::DistributedLock( const ConnectionString& conn , const string& name , unsigned long long lockTimeout, bool asProcess, bool legacy)
        : _conn(conn) , _name(name) , _lockTimeout(lockTimeout), _takeoverMinutes(0), _lastPingCheck(string(""), (mongo::Date_t) 0, (mongo::Date_t) 0) {

        _id = BSON( "_id" << name );
        _ns = locksNS;

        _maxClockSkew = 0;
        _maxNetSkew = 0;
        _lockPing = 0;

        // If this is a legacy lock, set our takeover minutes for local time comparisons
        if(legacy) {
            _takeoverMinutes = (unsigned) _lockTimeout;
            if(_takeoverMinutes == 0) _takeoverMinutes = 15;
            _lockTimeout = 0;

            _lockPing = _maxClockSkew = _maxNetSkew = (_takeoverMinutes * 60 * 1000) / LOCK_SKEW_FACTOR;
        }
        else {
            if(lockTimeout == 0) _lockTimeout = lockTimeout = LOCK_TIMEOUT;
            //if(lockPing == 0) lockPing = LOCK_PING;

            _lockPing = _maxClockSkew = _maxNetSkew = _lockTimeout / LOCK_SKEW_FACTOR;
        }

        // If we're emulating a new process for this lock, generate a processId
        if(asProcess) _processId = getDistLockId();
        else _processId = getDistLockProcess();

        log( logLvl - 1 ) << "created new distributed lock for " << name << " on " << conn
                          << " ( lock timeout : " << _lockTimeout << ", legacy timeout : " << _takeoverMinutes
                          << ", ping interval : " << _lockPing << ", process : " << asProcess
                          << ", legacy : " << legacy << " )" << endl;
    }

    Date_t DistributedLock::getRemoteTime() {
        return DistributedLock::remoteTime( _conn, _maxNetSkew );
    }

    bool DistributedLock::isRemoteTimeSkewed() {
        return !DistributedLock::checkSkew( _conn, NUM_LOCK_SKEW_CHECKS, _maxClockSkew, _maxNetSkew );
    }

    const ConnectionString& DistributedLock::getRemoteConnection() {
        return _conn;
    }

    const string& DistributedLock::getProcessId() {
        return _processId;
    }

    /**
     * Returns the remote time as reported by the cluster or server.  The maximum difference between the reported time
     * and the actual time on the remote server (at the completion of the function) is the maxNetSkew
     */
    Date_t DistributedLock::remoteTime( const ConnectionString& cluster, unsigned long long maxNetSkew ) {

        ConnectionString server( *cluster.getServers().begin() );
        ScopedDbConnection conn( server );

        BSONObj result;
        long long delay;

        try {
            Date_t then = jsTime();
            bool success = conn->runCommand( string("admin"), BSON( "serverStatus" << 1 ), result );
            delay = jsTime() - then;

            // TODO : Pick exception number
            if( !success )
                m_throw_exception( 13647, TimeNotFoundException, "could not get status from server " << server.toString() << " in cluster " << cluster.toString() << " to check time");

            // Make sure that our delay is not more than 2x our maximum network skew, since this is the max our remote
            // time value can be off by if we assume a response in the middle of the delay.
            if( delay > (long long) (maxNetSkew * 2) )
                m_throw_exception( 13648, TimeNotFoundException, "server " << server.toString() << " in cluster " << cluster.toString() << " did not respond within max network delay of " << maxNetSkew << "ms");
        }
        catch(...) {
            conn.done();
            throw;
        }

        conn.done();

        return result["localTime"].Date() - (delay / 2);

    }

    bool DistributedLock::checkSkew( const ConnectionString& cluster, unsigned skewChecks, unsigned long long maxClockSkew, unsigned long long maxNetSkew ) {

        vector<HostAndPort> servers = cluster.getServers();

        if(servers.size() < 1) return true;

        vector<long long> avgSkews;

        for(unsigned i = 0; i < skewChecks; i++) {

            // Find the average skew for each server
            unsigned s = 0;
            for(vector<HostAndPort>::iterator si = servers.begin(); si != servers.end(); ++si,s++) {

                if(i == 0) avgSkews.push_back(0);

                // Could check if this is self, but shouldn't matter since local network connection should be fast.
                ConnectionString server( *si );

                vector<long long> skew;

                BSONObj result;

                Date_t remote = remoteTime( server, maxNetSkew );
                Date_t local = jsTime();

                // Remote time can be delayed by at most MAX_NET_SKEW

                // Skew is how much time we'd have to add to local to get to remote
                avgSkews[s] += (long long) (remote - local);

                log( logLvl + 1 ) << "skew from remote server " << server << " found: " << (long long) (remote - local) << endl;

            }
        }

        // Analyze skews

        long long serverMaxSkew = 0;
        long long serverMinSkew = 0;

        for(unsigned s = 0; s < avgSkews.size(); s++) {

            long long avgSkew = (avgSkews[s] /= skewChecks);

            // Keep track of max and min skews
            if(s == 0) {
                serverMaxSkew = avgSkew;
                serverMinSkew = avgSkew;
            }
            else {
                if(avgSkew > serverMaxSkew)
                    serverMaxSkew = avgSkew;
                if(avgSkew < serverMinSkew)
                    serverMinSkew = avgSkew;
            }

        }

        long long totalSkew = serverMaxSkew - serverMinSkew;

        // Make sure our max skew is not more than our pre-set limit
        if(totalSkew > (long long) maxClockSkew) {
            log( logLvl + 1 ) << "total clock skew of " << totalSkew << "ms for servers " << cluster << " is out of " << maxClockSkew << "ms bounds." << endl;
            return false;
        }

        log( logLvl + 1 ) << "total clock skew of " << totalSkew << "ms for servers " << cluster << " is in " << maxClockSkew << "ms bounds." << endl;
        return true;
    }

    // For use in testing, ping thread should run indefinitely in practice.
    bool DistributedLock::killPinger( DistributedLock& lock ) {
        if( lock._threadId == "") return false;

        distLockPinger.kill( lock._conn, lock._processId );
        return true;
    }

    bool DistributedLock::lock_try( string why , BSONObj * other ) {

        // TODO:  Start pinging only when we actually get the lock?
        // If we don't have a thread pinger, make sure we shouldn't have one
        if( _threadId == "" )
            _threadId = distLockPinger.got( *this, _lockPing );

        // This should always be true, if not, we are using the lock incorrectly.
        assert( _name != "" );

        // write to dummy if 'other' is null
        BSONObj dummyOther;
        if ( other == NULL )
            other = &dummyOther;

        ScopedDbConnection conn( _conn );

        BSONObjBuilder queryBuilder;
        queryBuilder.appendElements( _id );
        queryBuilder.append( "state" , 0 );

        {
            // make sure its there so we can use simple update logic below
            BSONObj o = conn->findOne( _ns , _id ).getOwned();

            // Case 1: No locks
            if ( o.isEmpty() ) {
                try {
                    log( logLvl ) << "inserting initial doc in " << _ns << " for lock " << _name << endl;
                    conn->insert( _ns , BSON( "_id" << _name << "state" << 0 << "who" << "" ) );
                }
                catch ( UserException& e ) {
                    warning() << "could not insert initial doc for distributed lock " << _name << m_caused_by(e) << endl;
                }
            }

            // Case 2: A set lock that we might be able to force
            else if ( o["state"].numberInt() > 0 ) {

                string lockName = o["_id"].String() + string("/") + o["process"].String();

                BSONObj lastPing = conn->findOne( lockPingNS , o["process"].wrap( "_id" ) );
                if ( lastPing.isEmpty() ) {
                    if(_lockTimeout > 0) {
                        log( logLvl ) << "empty ping found for process in lock '" << lockName << "'" << endl;
                        // TODO:  Using 0 as a "no time found" value Will fail if dates roll over, but then, so will a lot.
                        lastPing = BSON( "_id" << o["process"].String() << "ping" << (Date_t) 0 );
                    }
                    else {
                        // LEGACY

                        // if a lock is taken but there's no ping for it, we're in an inconsistent situation
                        // if the lock holder (mongos or d)  does not exist anymore, the lock could safely be removed
                        // but we'd require analysis of the situation before a manual intervention
                        warning() << "config.locks: " << _name << " lock is taken by old process? "
                                  << "remove the following lock if the process is not active anymore: " << o << endl;
                        *other = o;
                        other->getOwned();
                        conn.done();
                        return false;
                    }
                }

                unsigned long long elapsed = 0;
                unsigned long long takeover = ( _lockTimeout > 0 ? _lockTimeout : _takeoverMinutes);
                if(_lockTimeout > 0) {

                    log( logLvl ) << "checking last ping for lock '" << lockName << "'" << " against process " << _lastPingCheck.get<0>() << " and ping " << _lastPingCheck.get<1>() << endl;

                    try {

                        Date_t remote = remoteTime( _conn );

                        // Timeout the elapsed time using comparisons of remote clock
                        if(_lastPingCheck.get<0>() != lastPing["_id"].String() || _lastPingCheck.get<1>() != lastPing["ping"].Date()) {
                            // If the ping has changed since we last checked, mark the current date and time.
                            _lastPingCheck = make_tuple(lastPing["_id"].String(), lastPing["ping"].Date(), remote );
                        }
                        else {

                            // GOTCHA!  Due to network issues, it is possible that the current time
                            // is less than the remote time.  We *have* to check this here, otherwise
                            // we overflow and our lock breaks.
                            if(_lastPingCheck.get<2>() >= remote)
                                elapsed = 0;
                            else
                                elapsed = remote - _lastPingCheck.get<2>();
                        }

                    }
                    catch( LockException& e ) {

                        // Remote server cannot be found / is not responsive
                        warning() << "Could not get remote time from " << _conn << m_caused_by(e);

                    }

                }
                else {
                    // LEGACY

                    // GOTCHA!  If jsTime() (current time) is less than the remote time,
                    // we should definitely not break the lock.  However, if we don't check
                    // this here, we get an invalid unsigned elapsed, which is ginormous and
                    // causes the lock to be forced.
                    if(lastPing["ping"].Date() > jsTime())
                        elapsed = 0;
                    else
                        elapsed = jsTime() - lastPing["ping"].Date(); // in ms

                    elapsed = elapsed / ( 1000 * 60 ); // convert to minutes
                }


                if ( elapsed <= takeover) {
                    log( logLvl ) << "could not force lock '" << lockName << "' because elapsed time " << elapsed << " <= " << " takeover time " << takeover << endl;
                    *other = o;
                    conn.done();
                    return false;
                }

                log( logLvl - 1 ) << "forcing lock '" << lockName << "' because elapsed time " << elapsed << " > " << " takeover time " << takeover << endl;
                try {

                    // Check the clock skew again.  If we check this before we get a lock
                    // and after the lock times out, we can be pretty sure the time is
                    // increasing at the same rate on all servers and therefore our
                    // timeout is accurate
                    uassert_msg( 13652, "remote time in cluster " << _conn.toString() << " is now skewed, cannot force lock.", !isRemoteTimeSkewed() );

                    // Make sure we break the lock with the correct "ts" (OID) value, otherwise
                    // we can overwrite a new lock inserted in the meantime.
                    conn->update( _ns , BSON( "_id" << _id["_id"].String() << "state" << o["state"].numberInt() << "ts" << o["ts"] ), BSON( "$set" << BSON( "state" << 0 ) ) );

                    BSONObj err = conn->getLastErrorDetailed();
                    string errMsg = DBClientWithCommands::getLastErrorString(err);

                    // TODO: Clean up all the extra code to exit this method, probably with a refactor
                    if ( !errMsg.empty() || !err["n"].type() || err["n"].numberInt() < 1 ) {
                        ( errMsg.empty() ? log( logLvl - 1 ) : warning() ) << "Could not force lock '" << lockName << "' "
                                << ( !errMsg.empty() ?  m_error_message(errMsg) : string("(another force won)") ) << endl;
                        *other = o;
                        other->getOwned();
                        conn.done();
                        return false;
                    }

                }
                catch( LockException& e ) {
                    warning() << "lock forcing '" << lockName << "' failed." << m_caused_by(e) << endl;
                    *other = o;
                    other->getOwned();
                    conn.done();
                    return false;
                }
                catch( UpdateNotTheSame&) {
                    // Abort since we aren't yet sure if we have multiple lock entries to timeout on the
                    // diff config servers, or this was just interference from other forcing.
                    warning() << "lock forcing '" << lockName << "' inconsistent, aborting." << endl;
                    *other = o;
                    other->getOwned();
                    conn.done();
                    return false;
                }

                // Lock forced, reset our timer
                if(_lockTimeout > 0)
                    _lastPingCheck = make_tuple(string(""), 0, 0);

                log( logLvl - 1 ) << "lock '" << lockName << "' successfully forced" << endl;

                // We don't need the ts value in the query, since we will only ever replace locks with state=0.
            }
            // Case 3: We have an expired lock
            else if ( o["ts"].type() ) {
                queryBuilder.append( o["ts"] );
            }
        }

        bool gotLock = false;
        BSONObj currLock;

        BSONObj lockDetails = BSON( "state" << 1 << "who" << getDistLockId() << "process" << _processId <<
                                    "when" << jsTime() << "why" << why << "ts" << OID::gen() );
        BSONObj whatIWant = BSON( "$set" << lockDetails );

        BSONObj query = queryBuilder.obj();

        string lockName = _name + string("/") + _processId;

        try {

            log( logLvl ) << "about to acquire distributed lock '" << lockName << ":\n"
                          <<  lockDetails.jsonString(Strict, true) << "\n"
                          << query.jsonString(Strict, true) << endl;

            conn->update( _ns , query , whatIWant );

            BSONObj err = conn->getLastErrorDetailed();
            string errMsg = DBClientWithCommands::getLastErrorString(err);

            currLock = conn->findOne( _ns , _id );

            if ( !errMsg.empty() || !err["n"].type() || err["n"].numberInt() < 1 ) {
                ( errMsg.empty() ? log( logLvl - 1 ) : warning() ) << "could not acquire lock '" << lockName << "' "
                        << ( !errMsg.empty() ?  m_error_message(errMsg) : string("(another update won)") ) << endl;
                *other = currLock;
                other->getOwned();
                gotLock = false;
            }
            else {
                gotLock = true;
            }

        }
        catch ( UpdateNotTheSame& up ) {
            // this means our update got through on some, but not others
            warning() << "distributed lock '" << lockName << " did not propagate properly." /* << m_caused_by(up) */ << endl;

            // Find the highest OID value on the diff. servers, that will be the value that
            // "wins"
            for ( unsigned i=0; i<up.size(); i++ ) {

                ScopedDbConnection indDB( up[i].first );

                BSONObj indUpdate = indDB->findOne( _ns , _id );
                if ( currLock.isEmpty() || currLock["ts"] < indUpdate["ts"] ) {
                    currLock = indUpdate.getOwned();
                }

                indDB.done();

            }

            if ( currLock["ts"].OID() == lockDetails["ts"].OID() ) {
                log( logLvl - 1 ) << "lock update won, completing lock propagation for '" << lockName << "'" << endl;
                gotLock = true;

                // TODO: This may not be safe, if we've previously borked here and are recovering via a
                // force.
                conn->update( _ns , _id , whatIWant );
            }
            else {
                log( logLvl - 1 ) << "lock update lost, lock '" << lockName << "' not propagated." << endl;
                gotLock = false;
            }
        }

        if(gotLock)
            log( logLvl - 1 ) << "distributed lock '" << lockName << "' acquired, now : " << currLock << endl;
        else
            log( logLvl - 1 ) << "distributed lock '" << lockName << "' was not acquired." << endl;

        conn.done();

        return gotLock;
    }

    void DistributedLock::unlock() {

        assert( _name != "" );

        string lockName = _name + string("/") + _processId;

        const int maxAttempts = 3;
        int attempted = 0;
        while ( ++attempted <= maxAttempts ) {

            try {
                ScopedDbConnection conn( _conn );
                conn->update( _ns , _id, BSON( "$set" << BSON( "state" << 0 ) ) );

                log( logLvl - 1 ) << "distributed lock '" << lockName << "' unlocked. " << conn->findOne( _ns , _id ) << endl;

                conn.done();
                return;

            }
            catch ( std::exception& e) {
                warning() << "distributed lock '" << lockName << "' failed unlock attempt."
                          << m_error_message(e.what()) <<  endl;

                sleepsecs(1 << attempted);
            }
        }

        warning() << "distributed lock '" << lockName << "' couldn't consummate unlock request. "
                  << "lock will be taken over after "
                  << (_lockTimeout > 0 ? _lockTimeout / (60 * 1000) : _takeoverMinutes)
                  << " minutes timeout." << endl;
    }



}
