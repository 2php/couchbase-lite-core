//
// SQLiteKeyStore+PredictiveIndexes.cc
//
// Copyright © 2018 Couchbase. All rights reserved.
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

#include "SQLiteKeyStore.hh"
#include "SQLiteDataFile.hh"
#include "QueryParser.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "MutableArray.hh"
#include "SQLiteCpp/SQLiteCpp.h"

#ifdef COUCHBASE_ENTERPRISE

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace litecore {

    bool SQLiteKeyStore::createPredictiveIndex(string indexName,
                                               const Array *expressions,
                                               const IndexOptions *options)
    {
        if (expressions->count() != 1)
            error::_throw(error::InvalidQuery, "Predictive index requires exactly one expression");
        const Array *expression = expressions->get(0)->asArray();
        if (!expression)
            error::_throw(error::InvalidQuery, "Predictive index requires a PREDICT() expression");

        // Create a table of the PREDICTION results:
        auto pred = MutableArray::newArray(expression);
        if (pred->count() > 3)
            pred->remove(3, 1);
        string predTableName = createPredictionTable(pred, options);

        // The final parameter is the result property to create a SQL index on:
        Array::iterator i(expression);
        i += 3;
        if (!i) {
            error::_throw(error::InvalidParameter,
                          "Missing result property name for predictive index");
        }
        return createValueIndex(kPredictiveIndex, predTableName, indexName, i, options);
    }


    string SQLiteKeyStore::createPredictionTable(const Value *expression,
                                                 const IndexOptions *options)
    {
        // Derive the table name from the expression (path) it unnests:
        QueryParser qp(*this);
        auto kvTableName = tableName();
        auto predTableName = qp.predictiveTableName(expression);

        // Create the index table, unless an identical one already exists:
        string sql = CONCAT("CREATE TABLE \"" << predTableName << "\" "
                            "(docid INTEGER PRIMARY KEY REFERENCES " << kvTableName << "(rowid), "
                            " body BLOB NOT NULL ON CONFLICT IGNORE) "
                            "WITHOUT ROWID");
        if (!_schemaExistsWithSQL(predTableName, "table", predTableName, sql)) {
            LogTo(QueryLog, "Creating predictive table '%s'", predTableName.c_str());
            db().exec(sql);

            // Populate the index-table with data from existing documents:
            string predictExpr = qp.expressionSQL(expression);
            db().exec(CONCAT("INSERT INTO \"" << predTableName << "\" (docid, body) "
                             "SELECT rowid, " << predictExpr <<
                             "FROM " << kvTableName << " WHERE (flags & 1) = 0"));

            // Set up triggers to keep the index-table up to date
            // ...on insertion:
            qp.setBodyColumnName("new.body");
            predictExpr = qp.expressionSQL(expression);
            string insertTriggerExpr = CONCAT("INSERT INTO \"" << predTableName <<
                                              "\" (docid, body) "
                                              "VALUES (new.rowid, " << predictExpr << ")");
            createTrigger(predTableName, "ins",
                          "AFTER INSERT",
                          "WHEN (new.flags & 1) = 0",
                          insertTriggerExpr);

            // ...on delete:
            string deleteTriggerExpr = CONCAT("DELETE FROM \"" << predTableName << "\" "
                                              "WHERE docid = old.rowid");
            createTrigger(predTableName, "del",
                          "BEFORE DELETE",
                          "WHEN (old.flags & 1) = 0",
                          deleteTriggerExpr);

            // ...on update:
            createTrigger(predTableName, "preupdate",
                          "BEFORE UPDATE OF body, flags",
                          "WHEN (old.flags & 1) = 0",
                          deleteTriggerExpr);
            createTrigger(predTableName, "postupdate",
                          "AFTER UPDATE OF body, flags",
                          "WHEN (new.flags) & 1 = 0",
                          insertTriggerExpr);
        }
        return predTableName;
    }


    string SQLiteKeyStore::predictiveTableName(const std::string &property) const {
        return tableName() + ":predict:" + property;
    }


    // Drops unnested-array tables that no longer have any indexes on them.
    void SQLiteKeyStore::garbageCollectPredictiveIndexes() {
        vector<string> garbageTableNames;
        {
            SQLite::Statement st(db(),
                 "SELECT predTbl.name FROM sqlite_master as predTbl "
                  "WHERE predTbl.type='table' and predTbl.name like (?1 || ':predict:%') "
                        "and not exists (SELECT * FROM sqlite_master "
                                         "WHERE type='index' and tbl_name=predTbl.name "
                                               "and sql not null)");
            st.bind(1, tableName());
            while(st.executeStep())
                garbageTableNames.push_back(st.getColumn(0));
        }
        for (string &tableName : garbageTableNames) {
            LogTo(QueryLog, "Dropping unused predictive table '%s'", tableName.c_str());
            db().exec(CONCAT("DROP TABLE \"" << tableName << "\""));
            dropTrigger(tableName, "ins");
            dropTrigger(tableName, "del");
            dropTrigger(tableName, "preupdate");
            dropTrigger(tableName, "postupdate");
        }
    }

}

#endif // COUCHBASE_ENTERPRISE
