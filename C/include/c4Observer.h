//
// c4Observer.h
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once

#include "c4Database.h"

#ifdef __cplusplus
extern "C" {
#endif


    /** \defgroup Observer  Database and Document Observers
        @{ */

    /** \name Database Observer
        @{ */

    /** Represents a change to a document in a database. */
    typedef struct {
        C4HeapString docID;         ///< The document's ID
        C4HeapString revID;         ///< The latest revision ID (or null if doc was purged)
        C4SequenceNumber sequence;  ///< The latest sequence number (or 0 if doc was purged)
        uint32_t bodySize;          ///< The size of the revision body in bytes
    } C4DatabaseChange;

    /** A database-observer reference. */
    typedef struct c4DatabaseObserver C4DatabaseObserver;

    /** Callback invoked by a database observer.
     
        CAUTION: This callback is called when a transaction is committed, even one made by a
        different connection (C4Database instance) on the same file. This means that, if your
        application is multithreaded, the callback may be running on a different thread than the
        one this database instance uses. It is your responsibility to ensure thread safety.

        In general, it is best to make _no_ LiteCore calls from within this callback. Instead,
        use your platform event-handling API to schedule a later call from which you can read the
        changes. Since this callback may be invoked many times in succession, make sure you
        schedule only one call at a time.

        @param observer  The observer that initiated the callback.
        @param context  user-defined parameter given when registering the callback. */
    typedef void (*C4DatabaseObserverCallback)(C4DatabaseObserver* observer C4NONNULL,
                                               void *context);

    /** Creates a new database observer, with a callback that will be invoked after the database
        changes. The callback will be called _once_, after the first change. After that it won't
        be called again until all of the changes have been read by calling `c4dbobs_getChanges`.
        @param database  The database to observer.
        @param callback  The function to call after the database changes.
        @param context  An arbitrary value that will be passed to the callback.
        @return  The new observer reference. */
    C4DatabaseObserver* c4dbobs_create(C4Database* database C4NONNULL,
                                       C4DatabaseObserverCallback callback C4NONNULL,
                                       void *context) C4API;

    /** Identifies which documents have changed since the last time this function was called, or
        since the observer was created. This function effectively "reads" changes from a stream,
        in whatever quantity the caller desires. Once all of the changes have been read, the
        observer is reset and ready to notify again.

        IMPORTANT: After calling this function, you must call `c4dbobs_releaseChanges` to release
        memory that's being referenced by the `C4DatabaseChange`s.

        @param observer  The observer.
        @param outChanges  A caller-provided buffer of structs into which changes will be
                            written.
        @param maxChanges  The maximum number of changes to return, i.e. the size of the caller's
                            outChanges buffer.
        @param outExternal  Will be set to true if the changes were made by a different C4Database.
        @return  The number of changes written to `outChanges`. If this is less than `maxChanges`,
                            the end has been reached and the observer is reset. */
    uint32_t c4dbobs_getChanges(C4DatabaseObserver *observer C4NONNULL,
                                C4DatabaseChange outChanges[] C4NONNULL,
                                uint32_t maxChanges,
                                bool *outExternal C4NONNULL) C4API;

    /** Releases the memory used by the C4DatabaseChange structs (to hold the docID and revID
        strings.) This must be called after `c4dbobs_getChanges().
        @param changes  The same array of changes that was passed to `c4dbobs_getChanges`.
        @param numChanges  The number of changes returned by `c4dbobs_getChanges`, i.e. the number
                            of valid items in `changes`. */
    void c4dbobs_releaseChanges(C4DatabaseChange changes[],
                                uint32_t numChanges) C4API;

    /** Stops an observer and frees the resources it's using.
        It is safe to pass NULL to this call. */
    void c4dbobs_free(C4DatabaseObserver*) C4API;

    /** @} */


    /** \name Document Observer
     @{ */

    /** A document-observer reference. */
    typedef struct c4DocumentObserver C4DocumentObserver;

    /** Callback invoked by a document observer.
        @param observer  The observer that initiated the callback.
        @param docID  The ID of the document that changed.
        @param sequence  The sequence number of the change.
        @param context  user-defined parameter given when registering the callback. */
    typedef void (*C4DocumentObserverCallback)(C4DocumentObserver* observer C4NONNULL,
                                               C4String docID,
                                               C4SequenceNumber sequence,
                                               void *context);

    /** Creates a new document observer, with a callback that will be invoked when the document
        changes. The callback will be called every time the document changes.
        @param database  The database to observer.
        @param docID  The ID of the document to observe.
        @param callback  The function to call after the database changes.
        @param context  An arbitrary value that will be passed to the callback.
        @return  The new observer reference. */
    C4DocumentObserver* c4docobs_create(C4Database* database C4NONNULL,
                                        C4String docID,
                                        C4DocumentObserverCallback callback,
                                        void *context) C4API;

    /** Stops an observer and frees the resources it's using.
        It is safe to pass NULL to this call. */
    void c4docobs_free(C4DocumentObserver*) C4API;

    /** @} */


    /** \name Document Observer
     @{ */

    /** A query-observer reference. */
    typedef struct c4QueryObserver C4QueryObserver;

    /** Callback invoked by a query observer. */
    typedef void (*C4QueryObserverCallback)(C4QueryObserver* C4NONNULL,
                                            C4QueryEnumerator*,
                                            C4Error,
                                            void *context);

    /** Creates a new query observer, with a callback that will be invoked when the query
        results change, with an enumerator containing the new results.
        The callback won't be invoked immediately after a change, and won't be invoked after
        every change, to avoid performance problems. */
    C4QueryObserver* c4queryobs_create(C4Query *query C4NONNULL,
                                       C4QueryObserverCallback callback,
                                       void *context) C4API;

    /** Stops an observer and frees the resources it's using.
        It is safe to pass NULL to this call. */
    void c4queryobs_free(C4QueryObserver*) C4API;

    /** @} */

    /** @} */
#ifdef __cplusplus
}
#endif
