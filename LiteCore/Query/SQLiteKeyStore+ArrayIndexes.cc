//
// SQLiteKeyStore+ArrayIndexes.cc
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
#include "StringUtil.hh"
#include "SQLiteCpp/SQLiteCpp.h"

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace litecore {

    bool SQLiteKeyStore::createArrayIndex(string indexName,
                                          const Array *expressions,
                                          const IndexOptions *options)
    {
        Array::iterator iExprs(expressions);
        string arrayTableName = createUnnestedTable(iExprs.value(), options);
        return createValueIndex(kArrayIndex, arrayTableName, indexName, ++iExprs, options);
    }


    string SQLiteKeyStore::createUnnestedTable(const Value *expression, const IndexOptions *options) {
        // Derive the table name from the expression it unnests:
        auto kvTableName = tableName();
        auto unnestTableName = QueryParser(*this).unnestedTableName(expression);

        // Create the index table, unless an identical one already exists:
        string sql = CONCAT("CREATE TABLE \"" << unnestTableName << "\" "
                            "(docid INTEGER NOT NULL REFERENCES " << kvTableName << "(rowid), "
                            " i INTEGER NOT NULL,"
                            " body BLOB NOT NULL, "
                            " CONSTRAINT pk PRIMARY KEY (docid, i)) "
                            "WITHOUT ROWID");
        if (!_schemaExistsWithSQL(unnestTableName, "table", unnestTableName, sql)) {
            LogTo(QueryLog, "Creating UNNEST table '%s' on %s", unnestTableName.c_str(),
                  expression->toJSON(true).asString().c_str());
            db().exec(sql);

            QueryParser qp(*this);
            qp.setBodyColumnName("new.body");
            string eachExpr = qp.eachExpressionSQL(expression);

            // Populate the index-table with data from existing documents:
            db().exec(CONCAT("INSERT INTO \"" << unnestTableName << "\" (docid, i, body) "
                             "SELECT new.rowid, _each.rowid, _each.value " <<
                             "FROM " << kvTableName << " as new, " << eachExpr << " AS _each "
                             "WHERE (new.flags & 1) = 0"));

            // Set up triggers to keep the index-table up to date
            // ...on insertion:
            string insertTriggerExpr = CONCAT("INSERT INTO \"" << unnestTableName <<
                                              "\" (docid, i, body) "
                                              "SELECT new.rowid, _each.rowid, _each.value " <<
                                              "FROM " << eachExpr << " AS _each ");
            createTrigger(unnestTableName, "ins",
                          "AFTER INSERT",
                          "WHEN (new.flags & 1) = 0",
                          insertTriggerExpr);

            // ...on delete:
            string deleteTriggerExpr = CONCAT("DELETE FROM \"" << unnestTableName << "\" "
                                              "WHERE docid = old.rowid");
            createTrigger(unnestTableName, "del",
                          "BEFORE DELETE",
                          "WHEN (old.flags & 1) = 0",
                          deleteTriggerExpr);

            // ...on update:
            createTrigger(unnestTableName, "preupdate",
                          "BEFORE UPDATE OF body, flags",
                          "WHEN (old.flags & 1) = 0",
                          deleteTriggerExpr);
            createTrigger(unnestTableName, "postupdate",
                          "AFTER UPDATE OF body, flags",
                          "WHEN (new.flags & 1 = 0)",
                          insertTriggerExpr);
        }
        return unnestTableName;
    }


    string SQLiteKeyStore::unnestedTableName(const std::string &property) const {
        return tableName() + ":unnest:" + property;
    }


    // Drops unnested-array tables that no longer have any indexes on them.
    void SQLiteKeyStore::garbageCollectIndexTables() {
        vector<string> garbageTableNames;
        {
            SQLite::Statement st(db(),
                 "SELECT tbl.name FROM sqlite_master as tbl "
                  "WHERE tbl.type='table' and tbl.name like (?1 || ':_%:%')"
                        "and not exists (SELECT * FROM sqlite_master "
                                         "WHERE type='index' and tbl_name=tbl.name "
                                               "and sql not null)");
            st.bind(1, tableName());
            while(st.executeStep())
                garbageTableNames.push_back(st.getColumn(0));
        }
        for (string &tableName : garbageTableNames) {
            LogTo(QueryLog, "Dropping unused index table '%s'", tableName.c_str());
            db().exec(CONCAT("DROP TABLE \"" << tableName << "\""));
            dropTrigger(tableName, "ins");
            dropTrigger(tableName, "del");
            dropTrigger(tableName, "preupdate");
            dropTrigger(tableName, "postupdate");
        }
    }

}
