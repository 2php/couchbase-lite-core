//
// Query.hh
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
#include "RefCounted.hh"
#include "KeyStore.hh"
#include "FleeceImpl.hh"
#include "Error.hh"
#include <atomic>

namespace litecore {
    class QueryEnumerator;


    /** Abstract base class of compiled database queries.
        These are created by the factory method KeyStore::compileQuery(). */
    class Query : public RefCounted {
    public:

        /** Info about a match of a full-text query term */
        struct FullTextTerm {
            uint64_t dataSource;              ///< Opaque identifier of where text is stored
            uint32_t keyIndex;                ///< Which index key the match occurred in
            uint32_t termIndex;               ///< Index of the search term in the tokenized query
            uint32_t start, length;           ///< *Byte* range of word in query string
        };


        KeyStore& keyStore() const                                      {return _keyStore;}
        alloc_slice expression() const                                  {return _expression;}

        virtual unsigned columnCount() const noexcept =0;
        
        virtual const std::vector<std::string>& columnTitles() const noexcept =0;

        virtual alloc_slice getMatchedText(const FullTextTerm&) =0;

        virtual std::string explain() =0;


        struct Options {
            Options() { }
            
            Options(const Options &o)
            :paramBindings(o.paramBindings), afterSequence(o.afterSequence) { }

            template <class T>
            Options(T bindings, sequence_t afterSeq =0)
            :paramBindings(bindings), afterSequence(afterSeq) { }

            Options after(sequence_t afterSeq) const {return Options(paramBindings, afterSeq);}

            alloc_slice const paramBindings;
            sequence_t const  afterSequence {0};
        };

        virtual QueryEnumerator* createEnumerator(const Options* =nullptr) =0;

    protected:
        Query(KeyStore &keyStore, slice expression) noexcept
        :_keyStore(keyStore)
        ,_expression(expression)
        { }
        
        virtual ~Query() =default;

    private:
        KeyStore &_keyStore;
        alloc_slice _expression;
    };


    /** Iterator/enumerator of query results. Abstract class created by Query::createEnumerator. */
    class QueryEnumerator {
    public:
        using FullTextTerms = std::vector<Query::FullTextTerm>;

        virtual ~QueryEnumerator() =default;

        Query* query() const                                    {return _query;}
        const Query::Options& options() const                   {return _options;}
        sequence_t lastSequence() const                         {return _lastSequence;}

        virtual bool next() =0;

        virtual fleece::impl::Array::iterator columns() const noexcept =0;
        virtual uint64_t missingColumns() const noexcept =0;
        
        /** Random access to rows. May not be supported by all implementations, but does work with
            the current SQLite query implementation. */
        virtual int64_t getRowCount() const         {return -1;}
        virtual void seek(int64_t rowIndex)         {error::_throw(error::UnsupportedOperation);}

        virtual bool hasFullText() const                        {return false;}
        virtual const FullTextTerms& fullTextTerms()            {return _fullTextTerms;}

        /** If the query results have changed since I was created, returns a new enumerator
            that will return the new results. Otherwise returns null. */
        virtual QueryEnumerator* refresh() =0;

        virtual bool obsoletedBy(const QueryEnumerator*) =0;

    protected:
        QueryEnumerator(Query *query NONNULL, const Query::Options *options, sequence_t lastSeq)
        :_query(query)
        ,_options(options ? *options : Query::Options{})
        ,_lastSequence(lastSeq)
        { }

        Retained<Query> _query;
        Query::Options _options;
        std::atomic<sequence_t> _lastSequence;       // DB's lastSequence at the time the query ran
        // The implementation of fullTextTerms() should populate this and return a reference:
        FullTextTerms _fullTextTerms;
    };

}
