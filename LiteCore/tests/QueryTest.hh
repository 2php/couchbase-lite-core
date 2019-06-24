//
// QueryTest.hh
//
// Copyright © 2018 Couchbase. All rights reserved.
//

#pragma once
#include "DataFile.hh"
#include "Query.hh"
#include "Error.hh"
#include "FleeceImpl.hh"
#include "Benchmark.hh"
#include "StringUtil.hh"

#include "LiteCoreTest.hh"

using namespace litecore;
using namespace fleece::impl;
using namespace std;


// NOTE: This test does not use RevTree or Database, so it stores plain Fleece in record bodies.


class QueryTest : public DataFileTestFixture {
protected:

    string numberString(int n) {
        static const char* kDigit[10] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"};
        string str;
        do {
            str = string(kDigit[n % 10]) + str;
            n /= 10;
            if (n > 0)
                str = string("-") + str;
        } while (n > 0);
        return str;
    }

    sequence_t writeNumberedDoc(int i, slice str, Transaction &t,
                                       DocumentFlags flags =DocumentFlags::kNone) {
        return writeDoc(slice(stringWithFormat("rec-%03d", i)), flags, t, [=](Encoder &enc) {
            enc.writeKey("num");
            enc.writeInt(i);
            if (str) {
                enc.writeKey("str");
                enc.writeString(str);
            }
        });
    }

    // Write 100 docs with Fleece bodies of the form {"num":n} where n is the rec #
    void addNumberedDocs(int first =1, int n =100) {
        Transaction t(store->dataFile());
        for (int i = first; i < first + n; i++)
            REQUIRE(writeNumberedDoc(i, nullslice, t) == (sequence_t)i);
        t.commit();
    }

    sequence_t writeArrayDoc(int i, Transaction &t,
                                    DocumentFlags flags =DocumentFlags::kNone) {
        return writeDoc(slice(stringWithFormat("rec-%03d", i)), flags, t, [=](Encoder &enc) {
            enc.writeKey("numbers");
            enc.beginArray();
            for (int j = max(i-5, 1); j <= i; j++)
                enc.writeString(numberString(j));
            enc.endArray();
        });
    }

    void addArrayDocs(int first =1, int n =100) {
        Transaction t(store->dataFile());
        for (int i = first; i < first + n; i++)
            REQUIRE(writeArrayDoc(i, t) == (sequence_t)i);
        t.commit();
    }

    void writeMultipleTypeDocs(Transaction &t) {
        writeDoc("doc1"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("value");
            enc.beginArray();
            enc.writeInt(1);
            enc.endArray();
        });

        writeDoc("doc2"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("value");
            enc.writeString("cool value");
        });

        writeDoc("doc3"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("value");
            enc.writeDouble(4.5);
        });

        writeDoc("doc4"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("value");
            enc.beginDictionary(1);
            enc.writeKey("subvalue");
            enc.writeString("FTW");
            enc.endDictionary();
        });

        writeDoc("doc5"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("value");
            enc.writeBool(true);
        });
    }

    void writeFalselyDocs(Transaction &t) {
        writeDoc("doc6"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("value");
            enc.beginArray();
            enc.endArray();
        });

        writeDoc("doc7"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("value");
            enc.beginDictionary();
            enc.endDictionary();
        });

        writeDoc("doc81"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("value");
            enc.writeBool(false);
        });
    }

    void deleteDoc(DocID docID, bool hardDelete) {
        Transaction t(store->dataFile());
        if (hardDelete) {
            store->del(docID, t);
        } else {
            Record doc = store->get(docID);
            CHECK(doc.exists());
            doc.setFlag(DocumentFlags::kDeleted);
            store->write(doc, t);
        }
        t.commit();
    }

    void undeleteDoc(DocID docID) {
        Transaction t(store->dataFile());
        Record doc = store->get(docID);
        CHECK(doc.exists());
        doc.clearFlag(DocumentFlags::kDeleted);
        store->write(doc, t);
        t.commit();
    }

    vector<string> extractIndexes(vector<KeyStore::IndexSpec> indexes) {
        set<string> retVal;
        for (auto &i : indexes)
            retVal.insert(i.name);
        return vector<string>(retVal.begin(), retVal.end());
    }

    int64_t rowsInQuery(string json) {
        Retained<Query> query = store->compileQuery(json);
        Retained<QueryEnumerator> e(query->createEnumerator());
        return e->getRowCount();
    }

    void testExpressions(const vector<pair<string,string>> &tests) {
        {
            Transaction t(store->dataFile());
            writeNumberedDoc(1, nullslice, t);
            t.commit();
        }
        for(auto &test : tests) {
            INFO("Testing " << test.first);
            auto query = store->compileQuery(json5("{'WHAT': [" + test.first + "]}"));
            Retained<QueryEnumerator> e(query->createEnumerator());
            REQUIRE(e->getRowCount() == 1);
            REQUIRE(e->next());
            CHECK(e->columns()[0]->toString() == slice(test.second));
        }
    }

};
