//
// c4PredictiveQueryTest+CoreML.mm
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

#include "c4PredictiveQuery.h"
#include "CoreMLPredictiveModel.hh"
#include "c4QueryTest.hh"
#include "fleece/Fleece.hh"
#include <CoreML/CoreML.h>

using namespace fleece;


static NSString* asNSString(const string &str) {
    return [NSString stringWithUTF8String: str.c_str()];
}


// Test class that uses a CoreML model
class API_AVAILABLE(macos(10.13)) CoreMLTest : public QueryTest {
public:
    // The default model file comes from Apple's MarsHabitatPricePredictor sample app.
    CoreMLTest()
    :CoreMLTest("mars.json", "mars", "MarsHabitatPricer.mlmodel")
    { }

    CoreMLTest(const string &jsonFilename, const char *modelName, const string &modelFilename)
    :QueryTest(0, jsonFilename)
    {
        NSURL *url = [NSURL fileURLWithPath: asNSString(sFixturesDir + modelFilename)];
        NSError *error;
        NSURL* compiled = [MLModel compileModelAtURL: url error: &error];
        INFO("Error" << (error.description.UTF8String ?: "none"));
        REQUIRE(compiled);
        auto model = [MLModel modelWithContentsOfURL: compiled error: &error];
        INFO("Error" << (error.description.UTF8String ?: "none"));
        REQUIRE(model);
        _model.reset(new cbl::CoreMLPredictiveModel(model));
        _model->registerWithName(modelName);
    }

    unique_ptr<cbl::CoreMLPredictiveModel> _model;
};


TEST_CASE_METHOD(CoreMLTest, "CoreML Query", "[Query][C]") {
    compileSelect(json5("{'WHAT': [['._id'], ['PREDICTION()', 'mars', "
                        "{solarPanels: ['.panels'], greenhouses: ['.greenhouses'], size: ['.acres']}"
                        "]], 'ORDER_BY': [['._id']]}"));
    auto results = runCollecting<double>(nullptr, [=](C4QueryEnumerator *e) {
        Value val = FLArrayIterator_GetValueAt(&e->columns, 1);
        C4Log("result: %.*s", SPLAT(val.toJSON()));
        return round(val.asDict()["price"].asDouble());
    });
    CHECK(results == (vector<double>{1566, 16455, 28924}));
    // Expected results come from running Apple's MarsHabitatPricePredictor sample app.
}
