// dbcommands_admin.cpp

/**
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
   this file has dbcommands that are for dba type administration
   mostly around dbs and collections
   NOT system stuff
*/


#include "pch.h"
#include "jsobj.h"
#include "pdfile.h"
#include "namespace-inl.h"
#include "commands.h"
#include "cmdline.h"
#include "btree.h"
#include "curop-inl.h"
#include "../util/background.h"
#include "../util/logfile.h"
#include "../util/alignedbuilder.h"
#include "../util/paths.h"
#include "../scripting/engine.h"

namespace mongo {

    class CleanCmd : public Command {
    public:
        CleanCmd() : Command( "clean" ) {}

        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return WRITE; }

        virtual void help(stringstream& h) const { h << "internal"; }

        bool run(const string& dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            string dropns = dbname + "." + cmdObj.firstElement().valuestrsafe();

            if ( !cmdLine.quiet )
                tlog() << "CMD: clean " << dropns << endl;

            NamespaceDetails *d = nsdetails(dropns.c_str());

            if ( ! d ) {
                errmsg = "ns not found";
                return 0;
            }

            for ( int i = 0; i < Buckets; i++ )
                d->deletedList[i].Null();

            result.append("ns", dropns.c_str());
            return 1;
        }

    } cleanCmd;

    namespace dur {
        filesystem::path getJournalDir();
    }
 
    class JournalLatencyTestCmd : public Command {
    public:
        JournalLatencyTestCmd() : Command( "journalLatencyTest" ) {}

        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return NONE; }
        virtual bool adminOnly() const { return true; }
        virtual void help(stringstream& h) const { h << "test how long to write and fsync to a test file in the journal/ directory"; }

        bool run(const string& dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            filesystem::path p = dur::getJournalDir();
            p /= "journalLatencyTest";
        
            // remove file if already present
            try { 
                remove(p);
            }
            catch(...) { }

            BSONObjBuilder bb[2];
            for( int pass = 0; pass < 2; pass++ ) {
                LogFile f(p.string());
                AlignedBuilder b(1024 * 1024);
                {
                    Timer t;
                    for( int i = 0 ; i < 100; i++ ) { 
                        f.synchronousAppend(b.buf(), 8192);
                    }
                    bb[pass].append("8KB", t.millis() / 100.0);
                }
                {
                    const int N = 50;
                    Timer t2;
                    long long x = 0;
                    for( int i = 0 ; i < N; i++ ) { 
                        Timer t;
                        f.synchronousAppend(b.buf(), 8192);
                        x += t.micros();
                        sleepmillis(4);
                    }
                    long long y = t2.micros() - 4*N*1000;
                    // not really trusting the timer granularity on all platforms so whichever is higher of x and y
                    bb[pass].append("8KBWithPauses", max(x,y) / (N*1000.0));
                }
                {
                    Timer t;
                    for( int i = 0 ; i < 20; i++ ) { 
                        f.synchronousAppend(b.buf(), 1024 * 1024);
                    }
                    bb[pass].append("1MB", t.millis() / 20.0);
                }
                // second time around, we are prealloced.
            }
            result.append("timeMillis", bb[0].obj());
            result.append("timeMillisWithPrealloc", bb[1].obj());

            try { 
                remove(p);
            }
            catch(...) { }

            try {
                result.append("onSamePartition", onSamePartition(dur::getJournalDir().string(), dbpath));
            }
            catch(...) { }

            return 1;
        }
    } journalLatencyTestCmd;

    class ValidateCmd : public Command {
    public:
        ValidateCmd() : Command( "validate" ) {}

        virtual bool slaveOk() const {
            return true;
        }

        virtual void help(stringstream& h) const { h << "Validate contents of a namespace by scanning its data structures for correctness.  Slow."; }

        virtual LockType locktype() const { return READ; }
        //{ validate: "collectionnamewithoutthedbpart" [, scandata: <bool>] } */

        bool run(const string& dbname , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            string ns = dbname + "." + cmdObj.firstElement().valuestrsafe();
            NamespaceDetails * d = nsdetails( ns.c_str() );
            if ( !cmdLine.quiet )
                tlog() << "CMD: validate " << ns << endl;

            if ( ! d ) {
                errmsg = "ns not found";
                return 0;
            }

            result.append( "ns", ns );
            result.append( "result" , validateNS( ns.c_str() , d, &cmdObj ) );
            return 1;
        }


        string validateNS(const char *ns, NamespaceDetails *d, BSONObj *cmdObj) {
            bool scanData = true;
            if( cmdObj && cmdObj->hasElement("scandata") && !cmdObj->getBoolField("scandata") )
                scanData = false;
            bool valid = true;
            stringstream ss;
            ss << "\nvalidate\n";
            //ss << "  details: " << hex << d << " ofs:" << nsindex(ns)->detailsOffset(d) << dec << endl;
            if ( d->capped )
                ss << "  capped:" << d->capped << " max:" << d->max << '\n';

            ss << "  firstExtent:" << d->firstExtent.toString() << " ns:" << d->firstExtent.ext()->nsDiagnostic.toString()<< '\n';
            ss << "  lastExtent:" << d->lastExtent.toString()    << " ns:" << d->lastExtent.ext()->nsDiagnostic.toString() << '\n';
            try {
                d->firstExtent.ext()->assertOk();
                d->lastExtent.ext()->assertOk();

                DiskLoc el = d->firstExtent;
                int ne = 0;
                while( !el.isNull() ) {
                    Extent *e = el.ext();
                    e->assertOk();
                    el = e->xnext;
                    ne++;
                    killCurrentOp.checkForInterrupt();
                }
                ss << "  # extents:" << ne << '\n';
            }
            catch (...) {
                valid=false;
                ss << " extent asserted ";
            }

            ss << "  datasize?:" << d->stats.datasize << " nrecords?:" << d->stats.nrecords << " lastExtentSize:" << d->lastExtentSize << '\n';
            ss << "  padding:" << d->paddingFactor << '\n';
            try {

                try {
                    ss << "  first extent:\n";
                    d->firstExtent.ext()->dump(ss);
                    valid = valid && d->firstExtent.ext()->validates();
                }
                catch (...) {
                    ss << "\n    exception firstextent\n" << endl;
                }

                set<DiskLoc> recs;
                if( scanData ) {
                    shared_ptr<Cursor> c = theDataFileMgr.findAll(ns);
                    int n = 0;
                    int nInvalid = 0;
                    long long len = 0;
                    long long nlen = 0;
                    int outOfOrder = 0;
                    DiskLoc cl_last;
                    while ( c->ok() ) {
                        n++;

                        DiskLoc cl = c->currLoc();
                        if ( n < 1000000 )
                            recs.insert(cl);
                        if ( d->capped ) {
                            if ( cl < cl_last )
                                outOfOrder++;
                            cl_last = cl;
                        }

                        Record *r = c->_current();
                        len += r->lengthWithHeaders;
                        nlen += r->netLength();

                        BSONObj obj(r);
                        if (!obj.isValid() || !obj.valid()){ // both fast and deep checks
                            valid = false;
                            nInvalid++;
                            if (strcmp("_id", obj.firstElement().fieldName()) == 0){
                                try {
                                    obj.firstElement().validate(); // throws on error
                                    log() << "Invalid bson detected in " << ns << " with _id: " << obj.firstElement().toString(false) << endl;
                                }
                                catch(...){
                                    log() << "Invalid bson detected in " << ns << " with corrupt _id" << endl;
                                }
                            }
                            else {
                                log() << "Invalid bson detected in " << ns << " and couldn't find _id" << endl;
                            }
                        }

                        c->advance();
                    }
                    if ( d->capped && !d->capLooped() ) {
                        ss << "  capped outOfOrder:" << outOfOrder;
                        if ( outOfOrder > 1 ) {
                            valid = false;
                            ss << " ???";
                        }
                        else ss << " (OK)";
                        ss << '\n';
                    }
                    ss << "  " << n << " objects found, nobj:" << d->stats.nrecords << '\n';
                    ss << "  " << nInvalid << " invalid BSON objects found\n";

                    ss << "  " << len << " bytes data w/headers\n";
                    ss << "  " << nlen << " bytes data wout/headers\n";
                }

                ss << "  deletedList: ";
                for ( int i = 0; i < Buckets; i++ ) {
                    ss << (d->deletedList[i].isNull() ? '0' : '1');
                }
                ss << endl;
                int ndel = 0;
                long long delSize = 0;
                int incorrect = 0;
                for ( int i = 0; i < Buckets; i++ ) {
                    DiskLoc loc = d->deletedList[i];
                    try {
                        int k = 0;
                        while ( !loc.isNull() ) {
                            if ( recs.count(loc) )
                                incorrect++;
                            ndel++;

                            if ( loc.questionable() ) {
                                if( d->capped && !loc.isValid() && i == 1 ) {
                                    /* the constructor for NamespaceDetails intentionally sets deletedList[1] to invalid
                                       see comments in namespace.h
                                    */
                                    break;
                                }

                                if ( loc.a() <= 0 || strstr(ns, "hudsonSmall") == 0 ) {
                                    ss << "    ?bad deleted loc: " << loc.toString() << " bucket:" << i << " k:" << k << endl;
                                    valid = false;
                                    break;
                                }
                            }

                            DeletedRecord *d = loc.drec();
                            delSize += d->lengthWithHeaders;
                            loc = d->nextDeleted;
                            k++;
                            killCurrentOp.checkForInterrupt();
                        }
                    }
                    catch (...) {
                        ss <<"    ?exception in deleted chain for bucket " << i << endl;
                        valid = false;
                    }
                }
                ss << "  deleted: n: " << ndel << " size: " << delSize << endl;
                if ( incorrect ) {
                    ss << "    ?corrupt: " << incorrect << " records from datafile are in deleted list\n";
                    valid = false;
                }

                int idxn = 0;
                try  {
                    ss << "  nIndexes:" << d->nIndexes << endl;
                    NamespaceDetails::IndexIterator i = d->ii();
                    while( i.more() ) {
                        IndexDetails& id = i.next();
                        ss << "    " << id.indexNamespace() << " keys:" <<
                           id.head.btree()->fullValidate(id.head, id.keyPattern()) << endl;
                    }
                }
                catch (...) {
                    ss << "\n    exception during index validate idxn:" << idxn << endl;
                    valid=false;
                }

            }
            catch (AssertionException) {
                ss << "\n    exception during validate\n" << endl;
                valid = false;
            }

            if ( !valid )
                ss << " ns corrupt, requires dbchk\n";

            return ss.str();
        }
    } validateCmd;

    bool lockedForWriting = false; // read from db/instance.cpp
    static bool unlockRequested = false;
    static mongo::mutex fsyncLockMutex("fsyncLock");
    static boost::condition fsyncLockCondition;
    static OID fsyncLockID; // identifies the current lock job

    /*
        class UnlockCommand : public Command {
        public:
            UnlockCommand() : Command( "unlock" ) { }
            virtual bool readOnly() { return true; }
            virtual bool slaveOk() const { return true; }
            virtual bool adminOnly() const { return true; }
            virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
                if( lockedForWriting ) {
                    log() << "command: unlock requested" << endl;
                    errmsg = "unlock requested";
                    unlockRequested = true;
                }
                else {
                    errmsg = "not locked, so cannot unlock";
                    return 0;
                }
                return 1;
            }

        } unlockCommand;
    */
    /* see unlockFsync() for unlocking:
       db.$cmd.sys.unlock.findOne()
    */
    class FSyncCommand : public Command {
        static const char* url() { return  "http://www.mongodb.org/display/DOCS/fsync+Command"; }
        class LockDBJob : public BackgroundJob {
        protected:
            virtual string name() const { return "lockdbjob"; }
            void run() {
                Client::initThread("fsyncjob");
                Client& c = cc();
                {
                    scoped_lock lk(fsyncLockMutex);
                    while (lockedForWriting){ // there is a small window for two LockDBJob's to be active. This prevents it.
                        fsyncLockCondition.wait(lk.boost());
                    }
                    lockedForWriting = true;
                    fsyncLockID.init();
                }
                readlock lk("");
                MemoryMappedFile::flushAll(true);
                log() << "db is now locked for snapshotting, no writes allowed. db.fsyncUnlock() to unlock" << endl;
                log() << "    For more info see " << FSyncCommand::url() << endl;
                _ready = true;
                {
                    scoped_lock lk(fsyncLockMutex);
                    while( !unlockRequested ) {
                        fsyncLockCondition.wait(lk.boost());
                    }
                    unlockRequested = false;
                    lockedForWriting = false;
                    fsyncLockCondition.notify_all();
                }
                c.shutdown();
            }
        public:
            bool& _ready;
            LockDBJob(bool& ready) : BackgroundJob( true /* delete self */ ), _ready(ready) {
                _ready = false;
            }
        };
    public:
        FSyncCommand() : Command( "fsync" ) {}
        virtual LockType locktype() const { return WRITE; }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        /*virtual bool localHostOnlyIfNoAuth(const BSONObj& cmdObj) {
            string x = cmdObj["exec"].valuestrsafe();
            return !x.empty();
        }*/
        virtual void help(stringstream& h) const { h << url(); }
        virtual bool run(const string& dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            bool sync = !cmdObj["async"].trueValue(); // async means do an fsync, but return immediately
            bool lock = cmdObj["lock"].trueValue();
            log() << "CMD fsync:  sync:" << sync << " lock:" << lock << endl;

            if( lock ) {
                // fsync and lock variation 

                uassert(12034, "fsync: can't lock while an unlock is pending", !unlockRequested);
                uassert(12032, "fsync: sync option must be true when using lock", sync);
                /* With releaseEarly(), we must be extremely careful we don't do anything
                   where we would have assumed we were locked.  profiling is one of those things.
                   Perhaps at profile time we could check if we released early -- however,
                   we need to be careful to keep that code very fast it's a very common code path when on.
                */
                uassert(12033, "fsync: profiling must be off to enter locked mode", cc().database()->profile == 0);

                // todo future: Perhaps we could do this in the background thread.  As is now, writes may interleave between 
                //              the releaseEarly below and the acquisition of the readlock in the background thread. 
                //              However the real problem is that it seems complex to unlock here and then have a window for 
                //              writes before the bg job -- can be done correctly but harder to reason about correctness.
                //              If this command ran within a read lock in the first place, would it work, and then that 
                //              would be quite easy?
                //              Or, could we downgrade the write lock to a read lock, wait for ready, then release?
                getDur().syncDataAndTruncateJournal();

                bool ready = false;
                LockDBJob *l = new LockDBJob(ready);

                dbMutex.releaseEarly();
                
                // There is a narrow window for another lock request to come in
                // here before the LockDBJob grabs the readlock. LockDBJob will
                // ensure that the requests are serialized and never running
                // concurrently

                l->go();
                // don't return until background thread has acquired the read lock
                while( !ready ) {
                    sleepmillis(10);
                }
                result.append("info", "now locked against writes, use db.fsyncUnlock() to unlock");
                result.append("seeAlso", url());
            }
            else {
                // the simple fsync command case

                if (sync)
                    getDur().commitNow();
                result.append( "numFiles" , MemoryMappedFile::flushAll( sync ) );
            }
            return 1;
        }

    } fsyncCmd;

    // Note that this will only unlock the current lock.  If another thread
    // relocks before we return we still consider the unlocking successful.
    // This is imporant because if two scripts are trying to fsync-lock, each
    // one must be assured that between the fsync return and the call to unlock
    // that the database is fully locked
    void unlockFsyncAndWait(){
        scoped_lock lk(fsyncLockMutex);
        if (lockedForWriting) { // could have handled another unlock before we grabbed the lock
            OID curOp = fsyncLockID;
            unlockRequested = true;
            fsyncLockCondition.notify_all();
            while (lockedForWriting && fsyncLockID == curOp){
                fsyncLockCondition.wait( lk.boost() );
            }
        }
    }
}

