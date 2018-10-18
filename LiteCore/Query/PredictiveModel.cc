//
// PredictiveModel.cc
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

#include "PredictiveModel.hh"
#include <mutex>
#include <unordered_map>

#ifdef COUCHBASE_ENTERPRISE

namespace litecore {
    using namespace std;
    using namespace fleece;

    static unordered_map<string, Retained<PredictiveModel>> sRegistry;
    static mutex sRegistryMutex;


    void PredictiveModel::registerAs(const std::string &name) {
        lock_guard<mutex> lock(sRegistryMutex);
        sRegistry.insert({name, this});
    }

    bool PredictiveModel::unregister(const std::string &name) {
        lock_guard<mutex> lock(sRegistryMutex);
        return sRegistry.erase(name) > 0;
    }

    Retained<PredictiveModel> PredictiveModel::named(const std::string &name) {
        lock_guard<mutex> lock(sRegistryMutex);
        auto i = sRegistry.find(name);
        if (i == sRegistry.end())
            return nullptr;
        return i->second;
    }

}

#endif
