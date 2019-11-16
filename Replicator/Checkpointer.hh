//
// Checkpointer.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "ReplicatorOptions.hh"
#include "ReplicatorTypes.hh"
#include "Checkpoint.hh"
#include "Error.hh"
#include "Logging.hh"
#include "RefCounted.hh"
#include "Timer.hh"
#include "c4Base.h"
#include "fleece/slice.hh"
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_set>

namespace litecore { namespace repl {
    using namespace fleece;


    /** Manages a Replicator's checkpoint, including local storage (but not remote). */
    class Checkpointer {
    public:
        Checkpointer(const Options&, fleece::slice remoteURL);

        // Checkpoint:

        /** Compares my state with another Checkpoint.
            If the local sequences differ, mine will be reset to 0;
            if the remote sequences differ, mine will be reset to empty. */
        bool validateWith(const Checkpoint&);

        /** The checkpoint's local sequence. All sequences up to this are pushed. */
        C4SequenceNumber localMinSequence() const       {return _checkpoint.localMinSequence();}

        void addPendingSequence(C4SequenceNumber);
        void addPendingSequences(RevToSendList &sequences,
                                 C4SequenceNumber firstInRange,
                                 C4SequenceNumber lastInRange);
        void completedSequence(C4SequenceNumber);
        size_t numPendingDocs() const;

        /** The checkpoint's remote sequence, the last one up to which all is pulled. */
        fleece::alloc_slice remoteMinSequence() const   {return _checkpoint.remoteMinSequence();}

        /** Updates the checkpoint's remote sequence. */
        void setRemoteMinSequence(fleece::slice s);

        // Checkpoint IDs:

        /** Returns the doc ID where the checkpoint should initially be read from.
            This is usually the same as \ref checkpointID, but not in the case of a copied
            database that's replicating for the first time. */
        slice initialCheckpointID() const       {Assert(_initialDocID); return _initialDocID;}

        /** Returns the doc ID where the checkpoint is to be stored. */
        alloc_slice checkpointID() const        {Assert(_docID); return _docID;}

        // Database I/O:

        /** Reads the checkpoint state from the local database. This needs to happen first. */
        bool read(C4Database *db NONNULL, C4Error *outError);

        /** Updates the checkpoint from the database if it's changed. */
        bool reread(C4Database *db NONNULL, C4Error *outError);

        /** Writes serialized checkpoint state to the local database.
            Does not write the current checkpoint state, because it may have changed since the
            remote save. It's important that the saved data be the same as what was saved on
            the remote peer. */
        bool write(C4Database *db NONNULL, slice checkpointData, C4Error *outError);

        // Autosave:

        using duration = std::chrono::nanoseconds;
        using SaveCallback = std::function<void(fleece::alloc_slice jsonToSave)>;

        /** Enables autosave: at about the given duration after the first change is made,
            the callback will be invoked, and passed a JSON representation of my state. */
        void enableAutosave(duration saveTime, SaveCallback cb);

        /** Disables autosave. Returns true if no more calls to save() will be made. The only
            case where another call to save() might be made is if a save is currently in
            progress, and the checkpoint has been changed since the save began. In that case,
            another save will have to be triggered immediately when the current one finishes. */
        void stopAutosave();

        /** Triggers an immediate save, if the checkpoint has changed. */
        bool save();

        /** The client should call this as soon as its save completes, which can be after the
            SaveCallback returns. */
        void saveCompleted();

        /** Returns true if the checkpoint has changes that haven't been saved yet. */
        bool isUnsaved() const;

        // Pending documents:

        using PendingDocCallback = function_ref<void(const C4DocumentInfo&)>;

        /** Returns a fleece encoded list of the IDs of documents which have revisions pending push */
        bool pendingDocumentIDs(C4Database* NONNULL, PendingDocCallback, C4Error* outErr);

        /** Checks if the document with the given ID has any pending revisions to push*/
        bool isDocumentPending(C4Database* NONNULL, slice docId, C4Error* outErr);

        bool isDocumentAllowed(C4Document* doc NONNULL);
        bool isDocumentIDAllowed(slice docID);

    private:
        void checkpointIsInvalid();
        std::string docIDForUUID(const C4UUID&);
        slice remoteDocID(C4Database *db NONNULL, C4Error* err);
        alloc_slice _read(C4Database *db NONNULL, slice, C4Error*);
        void initializeDocIDs();
        void saveSoon();

        Logging*                        _logger;
        const Options&                  _options;
        alloc_slice const               _remoteURL;
        std::unordered_set<std::string> _docIDs;
        bool                            _resetCheckpoint;

        // Checkpoint state:
        mutable std::mutex              _mutex;
        Checkpoint                      _checkpoint {};

        // Document IDs:
        alloc_slice                     _initialDocID;      // DocID checkpoints are read from
        alloc_slice                     _docID;             // Actual checkpoint docID

        // Autosave:
        bool                            _changed  {false};
        bool                            _saving {false};
        bool                            _overdueForSave {false};
        std::unique_ptr<actor::Timer>   _timer;
        SaveCallback                    _saveCallback;
        duration                        _saveTime;
    };

} }