//
// CoreMLPredictiveModel.mm
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

#include "CoreMLPredictiveModel.hh"
#include "c4PredictiveQuery.h"
#include "fleece/Fleece.hh"
#include "fleece/Fleece+CoreFoundation.h"
#include <CoreML/CoreML.h>
#include <Vision/Vision.h>
#include <stdarg.h>

#ifdef COUCHBASE_ENTERPRISE

namespace cbl {
    using namespace std;
    using namespace fleece;


    void PredictiveModel::registerWithName(const char *name) {
        auto callback = [](void* modelInternal, FLValue input, C4Error *outError) {
            @autoreleasepool {
                try {
                    Encoder enc;
                    enc.beginDict();
                    auto self = (PredictiveModel*)modelInternal;
                    if (!self->predict(Value(input).asDict(), enc, outError))
                        return C4SliceResult{};
                    enc.endDict();
                    return C4SliceResult(enc.finish());
                } catch (const std::exception &x) {
                    reportError(outError, "prediction() threw an exception: %s", x.what());
                    return C4SliceResult{};
                }
            }
        };
        c4pred_registerModel(name, {this, callback});
        _name = name;
    }


    void PredictiveModel::unregister() {
        if (!_name.empty()) {
            c4pred_unregisterModel(_name.c_str());
            _name = "";
        }
    }


    bool PredictiveModel::reportError(C4Error *outError, const char *format, ...) {
        va_list args;
        va_start(args, format);
        char *message = nullptr;
        vasprintf(&message, format, args);
        va_end(args);
        C4LogToAt(kC4QueryLog, kC4LogError, "prediction() failed: %s", message);
        if (outError)
            *outError = c4error_make(LiteCoreDomain, kC4ErrorInvalidQuery, slice(message));
        free(message);
        return false;
    }


#pragma mark - CoreMLPredictiveModel:


    static NSDictionary* convertToMLDictionary(Dict);


    CoreMLPredictiveModel::CoreMLPredictiveModel(MLModel *model)
    :_model(model)
    ,_featureDescriptions(_model.modelDescription.inputDescriptionsByName)
    {
        // Check for image inputs:
        for (NSString *inputName in _featureDescriptions) {
            if (_featureDescriptions[inputName].type == MLFeatureTypeImage) {
                _imagePropertyName = nsstring_slice(inputName);
                break;
            }
        }
    }


    // The main prediction function!
    bool CoreMLPredictiveModel::predict(Dict inputDict, fleece::Encoder &enc, C4Error *outError) {
        if (_imagePropertyName) {
            if (!_visionModel) {
                NSError* error;
                _visionModel = [VNCoreMLModel modelForMLModel: _model error: &error];
                if (!_visionModel)
                    return reportError(outError, "Failed to create Vision model: %s",
                                       error.localizedDescription.UTF8String);
            }
            return predictViaVision(inputDict, enc, outError);
        } else {
            return predictViaCoreML(inputDict, enc, outError);
        }
    }


    // Uses CoreML API to generate prediction:
    bool CoreMLPredictiveModel::predictViaCoreML(Dict inputDict, fleece::Encoder &enc, C4Error *outError) {
        // Convert the input dictionary into an MLFeatureProvider:
        auto featureDict = [NSMutableDictionary new];
        for (NSString *name in _featureDescriptions) {
            Value value = Dict(inputDict)[nsstring_slice(name)];
            if (value) {
                MLFeatureValue *feature = featureFromDict(name, value, outError);
                if (!feature)
                    return {};
                featureDict[name] = feature;
            } else if (!_featureDescriptions[name].optional) {
                return reportError(outError, "required input property '%s' is missing", name.UTF8String);
            }
        }
        NSError *error;
        auto features = [[MLDictionaryFeatureProvider alloc] initWithDictionary: featureDict
                                                                          error: &error];
        NSCAssert(features, @"Failed to create MLDictionaryFeatureProvider");

        // Run the model!
        id<MLFeatureProvider> result = [_model predictionFromFeatures: features error: &error];
        if (!result) {
            const char *msg = error.localizedDescription.UTF8String;
            return reportError(outError, "CoreML error: %s", msg);
        }

        // Decode the result to Fleece:
        for (NSString* name in result.featureNames) {
            enc.writeKey(nsstring_slice(name));
            encodeMLFeature(enc, [result featureValueForName: name]);
        }
        return true;
    }


    // Uses Vision API to generate prediction:
    bool CoreMLPredictiveModel::predictViaVision(fleece::Dict input, Encoder &enc, C4Error *outError) {
        if (!_model)
            return reportError(outError, "Couldn't register Vision model");

        // Get the image data and create a Vision handler:
        slice image = input[_imagePropertyName].asData();
        if (!image)
            return reportError(outError, "Image input property '%.*s' missing or not a blob",
                               FMTSLICE(_imagePropertyName));
        NSData* imageData = image.uncopiedNSData();
        auto handler = [[VNImageRequestHandler alloc] initWithData: imageData options: @{}];

        // Process the model:
        NSError* error;
        auto request = [[VNCoreMLRequest alloc] initWithModel: _visionModel];
        request.imageCropAndScaleOption = VNImageCropAndScaleOptionCenterCrop;
        if (![handler performRequests: @[request] error: &error]) {
            return reportError(outError, "Image processing failed: %s",
                               error.localizedDescription.UTF8String);
        }
        auto results = request.results;
        if (!results)
            return reportError(outError, "Image processing returned no results");

        unsigned nClassifications = 0;
        double maxConfidence = 0.0;
        for (VNObservation* result in results) {
            if ([result isKindOfClass: [VNClassificationObservation class]]) {
                NSString* identifier = ((VNClassificationObservation*)result).identifier;
                double confidence = result.confidence;
                if (maxConfidence == 0.0)
                    maxConfidence = confidence;
                else if (confidence < maxConfidence * 0.5)
                    break;
                enc.writeKey(nsstring_slice(identifier));
                enc.writeDouble(confidence);
                if (++nClassifications >= kMaxClassifications)
                    break;
            } else if ([result isKindOfClass: [VNCoreMLFeatureValueObservation class]]) {
                auto feature = ((VNCoreMLFeatureValueObservation*)result).featureValue;
                enc.writeKey("output"_sl);      //???? How do I find the feature name?
                encodeMLFeature(enc, feature);
            } else {
                C4LogToAt(kC4QueryLog, kC4LogWarning,
                          "Image processing returned result of unsupported class %s",
                          result.className.UTF8String);
            }
        }
        return true;
    }


#pragma mark - INPUT FEATURE CONVERSION:


    // Creates an MLFeatureValue from the value of the same name in a Fleece dictionary.
    MLFeatureValue* CoreMLPredictiveModel::featureFromDict(NSString* name,
                                                           FLValue flValue,
                                                           C4Error *outError)
    {
        Value value(flValue);
        MLFeatureDescription *desc = _featureDescriptions[name];
        auto valueType = value.type();
        MLFeatureValue* feature = nil;
        switch (desc.type) {
            case MLFeatureTypeInt64:
                if (valueType == kFLNumber || valueType == kFLBoolean)
                    feature = [MLFeatureValue featureValueWithInt64: value.asInt()];
                break;
            case MLFeatureTypeDouble:
                if (valueType == kFLNumber)
                    feature = [MLFeatureValue featureValueWithDouble: value.asDouble()];
                break;
            case MLFeatureTypeString: {
                slice str = value.asString();
                if (str)
                    feature = [MLFeatureValue featureValueWithString: str.asNSString()];
                break;
            }
            case MLFeatureTypeDictionary: {
                NSDictionary *dict;
                if (valueType == kFLDict) {
                    dict = convertToMLDictionary(value.asDict());
                    if (!dict) {
                        reportError(outError, "input dictionary '%s' contains a non-numeric value",
                                    name.UTF8String);
                        return nil;
                    }
                } else if (valueType == kFLString) {
                    dict = convertWordsToMLDictionary(value.asString().asNSString());
                }
                if (dict)
                    feature = [MLFeatureValue featureValueWithDictionary: dict error: nullptr];
                break;
            }
            case MLFeatureTypeImage:
            case MLFeatureTypeMultiArray:
            case MLFeatureTypeSequence:
            case MLFeatureTypeInvalid:
                reportError(outError, "model input feature '%s' is of unsupported type %d; sorry!",
                            name.UTF8String, desc.type);
                return nil;
        }
        if (!feature) {
            reportError(outError, "input property '%s' has wrong type", name.UTF8String);
        } else if (![desc isAllowedValue: feature]) {
            reportError(outError, "input property '%s' has an invalid value", name.UTF8String);
            feature = nil;
        }
        return feature;
    }


    // Converts a Fleece dictionary to an NSDictionary. All values must be numeric.
    static NSDictionary* convertToMLDictionary(Dict dict) {
        auto nsdict = [[NSMutableDictionary alloc] initWithCapacity: dict.count()];
        for (Dict::iterator i(dict); i; ++i) {
            // Apparently dictionary features can only contain numbers...
            if (i.value().type() != kFLNumber)
                return nil;
            nsdict[i.keyString().asNSString()] = @(i.value().asDouble());
        }
        return nsdict;
    }


    // Converts a string into a dictionary that maps its words to the number of times they appear.
    NSDictionary* CoreMLPredictiveModel::convertWordsToMLDictionary(NSString* input) {
        constexpr auto options = NSLinguisticTaggerOmitWhitespace |
                                 NSLinguisticTaggerOmitPunctuation |
                                 NSLinguisticTaggerOmitOther;
        if (!_tagger) {
            auto schemes = [NSLinguisticTagger availableTagSchemesForLanguage: @"en"]; //FIX: L10N
            _tagger = [[NSLinguisticTagger alloc] initWithTagSchemes: schemes options: options];
        }

        auto words = [NSMutableDictionary new];
        _tagger.string = input;
        [_tagger enumerateTagsInRange: NSMakeRange(0, input.length)
                              scheme: NSLinguisticTagSchemeNameType
                             options: options
                          usingBlock: ^(NSLinguisticTag tag, NSRange tokenRange,
                                        NSRange sentenceRange, BOOL *stop)
         {
             if (tokenRange.length >= 3) {  // skip 1- and 2-letter words
                 NSString *token = [input substringWithRange: tokenRange].localizedLowercaseString;
                 NSNumber* count = words[token];
                 words[token] = @(count.intValue + 1);
             }
         }];
        return words;
    }


#pragma mark - OUTPUT FEATURE CONVERSION:


    static void encodeMultiArray(Encoder &enc, MLMultiArray* array,
                                 NSUInteger dimension, const uint8_t *data)
    {
        bool outer = (dimension + 1 < array.shape.count);
        auto n = array.shape[dimension].unsignedIntegerValue;
        auto stride = array.strides[dimension].unsignedIntegerValue;
        auto dataType = array.dataType;
        enc.beginArray();
        for (NSUInteger i = 0; i < n; i++) {
            if (outer) {
                encodeMultiArray(enc, array, dimension + 1, data);
            } else {
                switch (dataType) {
                    case MLMultiArrayDataTypeInt32:
                        enc.writeInt(*(const int32_t*)data);
                    case MLMultiArrayDataTypeFloat32:
                        enc.writeFloat(*(const float*)data);
                    case MLMultiArrayDataTypeDouble:
                        enc.writeDouble(*(const double*)data);
                }
            }
            data += stride;
        }
        enc.endArray();
    }

    static void encodeMultiArray(Encoder &enc, MLMultiArray* array) {
        encodeMultiArray(enc, array, 0, (const uint8_t*)array.dataPointer);
    }


    API_AVAILABLE(macos(10.14))
    static void encodeSequence(Encoder &enc, MLSequence *sequence) {
        switch (sequence.type) {
            case MLFeatureTypeString:
                FLEncoder_WriteNSObject(enc, sequence.stringValues); break;
            case MLFeatureTypeInt64:
                FLEncoder_WriteNSObject(enc, sequence.int64Values); break;
            default:
                enc.writeNull(); break;     // MLSequence API doesn't support any other types...
        }
    }


    void PredictiveModel::encodeMLFeature(Encoder &enc, MLFeatureValue *feature) {
        switch (feature.type) {
            case MLFeatureTypeInt64:
                enc.writeInt(feature.int64Value);
                break;
            case MLFeatureTypeDouble:
                enc.writeDouble(feature.doubleValue);
                break;
            case MLFeatureTypeString:
                enc.writeString(nsstring_slice(feature.stringValue));
                break;
            case MLFeatureTypeDictionary:
                FLEncoder_WriteNSObject(enc, feature.dictionaryValue);
                break;
            case MLFeatureTypeMultiArray:
                encodeMultiArray(enc, feature.multiArrayValue);
                break;
            case MLFeatureTypeSequence:
                if (@available(macOS 10.14, *))
                    encodeSequence(enc, feature.sequenceValue);
                else
                    enc.writeNull();
                break;
            case MLFeatureTypeImage:
                C4Warn("predict(): Don't know how to convert result MLFeatureTypeImage");//TODO
                enc.writeNull();
                break;
            case MLFeatureTypeInvalid:
                enc.writeNull();
                break;
        }
    }
}

#endif // COUCHBASE_ENTERPRISE
