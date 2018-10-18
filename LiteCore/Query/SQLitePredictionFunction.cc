//
// SQLitePredictFunction.cc
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

#include "SQLiteFleeceUtil.hh"
#include "PredictiveModel.hh"
#include "StringUtil.hh"
#include "HeapValue.hh"
#include <sqlite3.h>
#include <string>

namespace litecore {
    using namespace std;
    using namespace fleece;
    using namespace fleece::impl;

    static void predictionFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv){
        auto name = (const char*)sqlite3_value_text(argv[0]);
        auto model = PredictiveModel::named(name);
        if (!model) {
            string msg = format("Unknown ML model name '%s'", name);
            sqlite3_result_error(ctx, msg.c_str(), -1);
            return;
        }

        RetainedConst<Value> input;
        switch(sqlite3_value_type(argv[1])) {
            case SQLITE_INTEGER:
                input = NewValue(sqlite3_value_int64(argv[1]));
                break;
            case SQLITE_FLOAT:
                input = NewValue(sqlite3_value_double(argv[1]));
                break;
            case SQLITE_TEXT: {
                auto bytes = sqlite3_value_text(argv[1]);
                auto len = sqlite3_value_bytes(argv[1]);
                input = NewValue(slice(bytes, len));
                break;
            }
            case SQLITE_BLOB:
                input = fleeceParam(ctx, argv[1]);
                break;
            default:
                break;
        }
        if (!input) {
            sqlite3_result_error(ctx, "Invalid input to predict()", -1);
            return;
        }

        if (QueryLog.willLog(LogLevel::Verbose)) {
            auto json = input->toJSONString();
            LogVerbose(QueryLog, "calling predict(\"%s\", %s)", name, json.c_str());
        }

        alloc_slice result = model->predict(input);
        if (!result) {
            sqlite3_result_error(ctx, "ML model error", -1);
            return;
        }
        setResultBlobFromFleeceData(ctx, result);
    }


    const SQLiteFunctionSpec kPredictFunctionsSpec[] = {
        { "prediction",       2, predictionFunc  },
        { }
    };


}
