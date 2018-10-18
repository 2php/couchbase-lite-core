//
// CoreMLPredictiveModel.hh
//
// Copyright © 2018 Couchbase. All rights reserved.
//

#pragma once
#include "c4PredictiveQuery.h"
#include <Foundation/Foundation.h>
#include <string>
@class MLModel, MLFeatureDescription, MLFeatureValue, NSLinguisticTagger;

namespace cbl {

    /** An Objective-C++ adapter class that registers a CoreML model with LiteCore
        for predictive queries. */
    class API_AVAILABLE(macos(10.13), ios(11)) CoreMLPredictiveModel {
    public:
        CoreMLPredictiveModel(MLModel* C4NONNULL model);
        ~CoreMLPredictiveModel()                                {unregister();}

        void registerWithName(const char* C4NONNULL name);
        void unregister();

    private:
        static C4SliceResult predictCallback(void* modelInternal, FLValue input, C4Error *error);
        C4SliceResult predict(FLValue input, C4Error *error);
        MLFeatureValue* featureFromDict(NSString* name, FLValue, C4Error *outError);
        NSDictionary* convertToMLDictionary(FLDict);
        NSDictionary* convertToMLDictionary(NSString*);

        CoreMLPredictiveModel(CoreMLPredictiveModel&) =delete;
        CoreMLPredictiveModel& operator=(const CoreMLPredictiveModel&) =delete;

        MLModel* _model;
        NSDictionary<NSString*,MLFeatureDescription*>* _featureDescriptions;
        std::string _name;
        NSLinguisticTagger* _tagger {nil};
    };

}
