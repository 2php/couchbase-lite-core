//
//  Database.cc
//  LiteCore
//
//  Created by Jens Alfke on 10/19/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "Database.hh"
#include "Document.hh"
#include "c4Internal.hh"
#include "c4Document.h"
#include "DataFile.hh"
#include "CASRevisionStore.hh"
#include "Record.hh"
#include "SequenceTracker.hh"
#include "Fleece.hh"
#include "BlobStore.hh"
#include "Upgrader.hh"
#include "forestdb_endian.h"
#include "SecureRandomize.hh"


namespace c4Internal {
    using namespace litecore;
    using namespace fleece;


    static const slice kMaxRevTreeDepthKey = "maxRevTreeDepth"_sl;
    static uint32_t kDefaultMaxRevTreeDepth = 20;

    const slice Database::kPublicUUIDKey = "publicUUID"_sl;
    const slice Database::kPrivateUUIDKey = "privateUUID"_sl;


#pragma mark - LIFECYCLE:


    // `path` is path to bundle; return value is path to db file. Updates config.storageEngine. */
    /*static*/ FilePath Database::findOrCreateBundle(const string &path,
                                                     C4DatabaseConfig &config)
    {
        if (!(config.flags & kC4DB_Bundled))
            return path;

        FilePath bundle(path, "");
        bool createdDir = ((config.flags & kC4DB_Create) && bundle.mkdir());
        if (!createdDir)
            bundle.mustExistAsDir();

        DataFile::Factory *factory = DataFile::factoryNamed(config.storageEngine);
        if (!factory)
            error::_throw(error::InvalidParameter);

        // Look for the file corresponding to the requested storage engine (defaulting to SQLite):

        FilePath dbPath = bundle["db"].withExtension(factory->filenameExtension());
        if (createdDir || factory->fileExists(dbPath)) {
            if (config.storageEngine == nullptr)
                config.storageEngine = factory->cname();
            return dbPath;
        }

        if (config.storageEngine != nullptr) {
            // DB exists but not in the format they specified, so fail:
            error::_throw(error::WrongFormat);
        }

        // Not found, but they didn't specify a format, so try the other formats:
        for (auto otherFactory : DataFile::factories()) {
            if (otherFactory != factory) {
                dbPath = bundle["db"].withExtension(otherFactory->filenameExtension());
                if (factory->fileExists(dbPath)) {
                    config.storageEngine = factory->cname();
                    return dbPath;
                }
            }
        }

        // Weird; the bundle exists but doesn't contain any known type of database, so fail:
        error::_throw(error::WrongFormat);
    }


    // subroutine of Database constructor that creates its _db
    /*static*/ DataFile* Database::newDataFile(const FilePath &path,
                                               const C4DatabaseConfig &config,
                                               bool isMainDB)
    {
        DataFile::Options options { };
        if (isMainDB) {
            options.keyStores.sequences = true;
        }
        options.create = (config.flags & kC4DB_Create) != 0;
        options.writeable = (config.flags & kC4DB_ReadOnly) == 0;

        options.encryptionAlgorithm = (EncryptionAlgorithm)config.encryptionKey.algorithm;
        if (options.encryptionAlgorithm != kNoEncryption) {
            options.encryptionKey = alloc_slice(config.encryptionKey.bytes,
                                                sizeof(config.encryptionKey.bytes));
        }

        const char *storageEngine = config.storageEngine;
        if (!storageEngine) {
            storageEngine = "";
        }

        DataFile::Factory *storage = DataFile::factoryNamed((string)(storageEngine));
        if (!storage)
            error::_throw(error::Unimplemented);

        try {
            // Open the DataFile:
            return storage->openFile(path, &options);
        } catch (const error &x) {
            if (x.domain == error::LiteCore && x.code == error::DatabaseTooOld) {
                // This is an old 1.x database; upgrade it in place, then open:
                if (UpgradeDatabaseInPlace(path.dir(), config))
                    return storage->openFile(path, &options);
            }
            throw;
        }
    }


    Database::Database(const string &path,
                       C4DatabaseConfig inConfig)
    :_db(newDataFile(findOrCreateBundle(path, inConfig), inConfig, true))
    ,config(inConfig)
    ,_encoder(new fleece::Encoder())
    {
        if (config.flags & kC4DB_SharedKeys) {
            _db->useDocumentKeys();
            _encoder->setSharedKeys(documentKeys());
        }
        if (!(config.flags & kc4DB_NonObservable))
            _sequenceTracker.reset(new SequenceTracker());

        // Validate that the versioning matches what's used in the database:
        auto &info = _db->getKeyStore(DataFile::kInfoKeyStoreName);
        Record doc = info.get(slice("versioning"));
        if (doc.exists()) {
            if (doc.bodyAsUInt() != (uint64_t)config.versioning)
                error::_throw(error::WrongFormat);
        } else if (config.flags & kC4DB_Create) {
            doc.setBodyAsUInt((uint64_t)config.versioning);
            Transaction t(*_db);
            info.write(doc, t);
            t.commit();
        } else if (config.versioning != kC4RevisionTrees) {
            error::_throw(error::WrongFormat);
        }
        _db->setOwner(this);

        DocumentFactory* factory;
        switch (config.versioning) {
            case kC4VersionVectors: factory = new VectorDocumentFactory(this); break;
            case kC4RevisionTrees:  factory = new TreeDocumentFactory(this); break;
            default:                error::_throw(error::InvalidParameter);
        }
        _documentFactory.reset(factory);
        _db->setRecordFleeceAccessor(factory->fleeceAccessor());
}


    Database::~Database() {
        Assert(_transactionLevel == 0);
    }


#pragma mark - HOUSEKEEPING:


    void Database::close() {
        mustNotBeInTransaction();
        _db->close();
    }


    void Database::deleteDatabase() {
        mustNotBeInTransaction();
        FilePath bundle = path().dir();
        _db->deleteDataFile();
        if (config.flags & kC4DB_Bundled)
            bundle.delRecursive();
    }


    /*static*/ bool Database::deleteDatabaseAtPath(const string &dbPath,
                                                   const C4DatabaseConfig *config) {
        if (config == nullptr) {
            return FilePath(dbPath).delWithAllExtensions();
        } else if (config->flags & kC4DB_Bundled) {
            // Find the db file in the bundle:
            FilePath bundle {dbPath, ""};
            if (bundle.exists()) {
                try {
                    auto tempConfig = *config;
                    tempConfig.flags &= ~kC4DB_Create;
                    tempConfig.storageEngine = nullptr;
                    auto dbFilePath = findOrCreateBundle(dbPath, tempConfig);
                    // Delete it:
                    tempConfig.flags &= ~kC4DB_Bundled;
                    deleteDatabaseAtPath(dbFilePath, &tempConfig);
                } catch (const error &x) {
                    if (x.code != error::WrongFormat)   // ignore exception if db file isn't found
                        throw;
                }
            }
            // Delete the rest of the bundle:
            return bundle.delRecursive();
        } else {
            FilePath path(dbPath);
            DataFile::Factory *factory = nullptr;
            if (config && config->storageEngine) {
                factory = DataFile::factoryNamed(config->storageEngine);
                if (!factory)
                    Warn("c4db_deleteAtPath: unknown storage engine '%s'", config->storageEngine);
            } else {
                factory = DataFile::factoryForFile(path);
                if (!factory)
                    factory = DataFile::factories()[0];
            }
            if (!factory)
                error::_throw(error::WrongFormat);
            return factory->deleteFile(path);
        }
    }


    void Database::compact() {
        mustNotBeInTransaction();
        dataFile()->compact();
    }


    void Database::setOnCompact(DataFile::OnCompactCallback callback) noexcept {
        dataFile()->setOnCompact(callback);
    }


    void Database::rekey(const C4EncryptionKey *newKey) {
        mustNotBeInTransaction();
        rekeyDataFile(dataFile(), newKey);
    }


    /*static*/ void Database::rekeyDataFile(DataFile* database, const C4EncryptionKey *newKey)
    {
        if (newKey) {
            database->rekey((EncryptionAlgorithm)newKey->algorithm,
                            slice(newKey->bytes, 32));
        } else {
            database->rekey(kNoEncryption, nullslice);
        }
    }


#pragma mark - ACCESSORS:


    FilePath Database::path() const {
        FilePath path = _db->filePath();
        if (config.flags & kC4DB_Bundled)
            path = path.dir();
        return path;
    }


    uint64_t Database::countDocuments() {
        return defaultKeyStore().recordCount();
    }


    uint32_t Database::maxRevTreeDepth() {
        if (_maxRevTreeDepth == 0) {
            auto &info = _db->getKeyStore(DataFile::kInfoKeyStoreName);
            _maxRevTreeDepth = (uint32_t)info.get(kMaxRevTreeDepthKey).bodyAsUInt();
            if (_maxRevTreeDepth == 0)
                _maxRevTreeDepth = kDefaultMaxRevTreeDepth;
        }
        return _maxRevTreeDepth;
    }

    void Database::setMaxRevTreeDepth(uint32_t depth) {
        if (depth == 0)
            depth = kDefaultMaxRevTreeDepth;
        KeyStore &info = _db->getKeyStore(DataFile::kInfoKeyStoreName);
        Record rec = info.get(kMaxRevTreeDepthKey);
        if (depth != rec.bodyAsUInt()) {
            rec.setBodyAsUInt(depth);
            Transaction t(*_db);
            info.write(rec, t);
            t.commit();
        }
        _maxRevTreeDepth = depth;
    }


    KeyStore& Database::defaultKeyStore()                         {return _db->defaultKeyStore();}
    KeyStore& Database::getKeyStore(const string &name) const     {return _db->getKeyStore(name);}


    BlobStore* Database::blobStore() {
        if (!_blobStore) {
            if (!(config.flags & kC4DB_Bundled))
                error::_throw(error::UnsupportedOperation);
            FilePath blobStorePath = path().subdirectoryNamed("Attachments");
            auto options = BlobStore::Options::defaults;
            options.create = options.writeable = (config.flags & kC4DB_ReadOnly) == 0;
            options.encryptionAlgorithm =(EncryptionAlgorithm)config.encryptionKey.algorithm;
            if (options.encryptionAlgorithm != kNoEncryption) {
                options.encryptionKey = alloc_slice(config.encryptionKey.bytes,
                                                    sizeof(config.encryptionKey.bytes));
            }
            _blobStore.reset(new BlobStore(blobStorePath, &options));
        }
        return _blobStore.get();
    }


    SequenceTracker& Database::sequenceTracker() {
        if (!_sequenceTracker)
            error::_throw(error::UnsupportedOperation);
        return *_sequenceTracker;
    }



    Database::UUID Database::getUUID(slice key) {
        auto &store = getKeyStore((string)kC4InfoStore);
        Record r = store.get(key);
        if (r.exists())
            return *(UUID*)r.body().buf;

        UUID uuid;
        beginTransaction();
        try {
            Record r2 = store.get(key);
            if (r2.exists()) {
                uuid = *(UUID*)r2.body().buf;
            } else {
                // Create the UUIDs:
                slice uuidSlice{&uuid, sizeof(uuid)};
                GenerateUUID(uuidSlice);
                store.set(key, uuidSlice, transaction());
            }
        } catch (...) {
            endTransaction(false);
            throw;
        }
        endTransaction(true);
        return uuid;
    }
    
    
#pragma mark - TRANSACTIONS:


    // NOTE: The lock order is always: first _transactionMutex, then _mutex.
    // The transaction methods below acquire _transactionMutex;
    // so do not call them if _mutex is already locked (after WITH_LOCK) or deadlock may occur!


    void Database::beginTransaction() {
        if (++_transactionLevel == 1) {
            _transaction = new Transaction(_db.get());
            if (_sequenceTracker) {
                lock_guard<mutex> lock(_sequenceTracker->mutex());
                _sequenceTracker->beginTransaction();
            }
        }
    }

    bool Database::inTransaction() noexcept {
        return _transactionLevel > 0;
    }


    void Database::endTransaction(bool commit) {
        if (_transactionLevel == 0)
            error::_throw(error::NotInTransaction);
        if (--_transactionLevel == 0) {
            auto t = _transaction;
            try {
                if (commit)
                    t->commit();
                else
                    t->abort();
            } catch (...) {
                _cleanupTransaction(false);
                throw;
            }
            _cleanupTransaction(commit);
        }
    }


    // The cleanup part of endTransaction
    void Database::_cleanupTransaction(bool committed) {
        if (_sequenceTracker) {
            lock_guard<mutex> lock(_sequenceTracker->mutex());
            if (committed) {
                // Notify other Database instances on this file:
                _db->forOtherDataFiles([&](DataFile *other) {
                    auto otherDatabase = (Database*)other->owner();
                    if (otherDatabase)
                        otherDatabase->externalTransactionCommitted(*_sequenceTracker);
                });
            }
            _sequenceTracker->endTransaction(committed);
        }
        delete _transaction;
        _transaction = nullptr;
    }


    void Database::externalTransactionCommitted(const SequenceTracker &sourceTracker) {
        if (_sequenceTracker) {
            lock_guard<mutex> lock(_sequenceTracker->mutex());
            _sequenceTracker->addExternalTransaction(sourceTracker);
        }
    }


    void Database::mustBeInTransaction() {
        if (!inTransaction())
            error::_throw(error::NotInTransaction);
    }

    void Database::mustNotBeInTransaction() {
        if (inTransaction())
            error::_throw(error::TransactionNotClosed);
    }


    Transaction& Database::transaction() const {
        auto t = _transaction;
        if (!t) error::_throw(error::NotInTransaction);
        return *t;
    }


#pragma mark - DOCUMENTS:

    
    bool Database::purgeDocument(slice docID) {
        return defaultKeyStore().del(docID, transaction());
    }


    Record Database::getRawDocument(const string &storeName, slice key) {
        return getKeyStore(storeName).get(key);
    }


    void Database::putRawDocument(const string &storeName, slice key, slice meta, slice body) {
        KeyStore &localDocs = getKeyStore(storeName);
        auto &t = transaction();
        if (body.buf || meta.buf)
            localDocs.set(key, meta, body, DocumentFlags::kNone, t);
        else
            localDocs.del(key, t);
    }


    fleece::Encoder& Database::sharedEncoder() {
        _encoder->reset();
        return *_encoder.get();
    }


    void Database::saved(Document* doc) {
        if (_sequenceTracker) {
            lock_guard<mutex> lock(_sequenceTracker->mutex());
            Assert(doc->selectedRev.sequence == doc->sequence); // The new revision must be selected
            _sequenceTracker->documentChanged(doc->_docIDBuf,
                                              doc->_selectedRevIDBuf,
                                              doc->selectedRev.sequence,
                                              doc->selectedRev.body.size);
        }
    }

}
