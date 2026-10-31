// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"

class FJsonObject;
class FJsonValue;
class FObjectPropertyBase;
class FProperty;
class UObject;
class UEnum;
struct FInstancedPropertyBag;
class UScriptStruct;

namespace UeAgentPropertyValueCodec {
using UE::UAI::Contract::FDiagnostic;
using UE::UAI::StateSemantic::CanonicalJson::FValueComparison;
using FObjectReferenceEncoder = TFunction<bool(
    const FObjectPropertyBase *Property, const void *ValuePtr, UObject *Owner,
    int32 Depth, TSharedPtr<FJsonValue> &OutValue,
    TSharedPtr<FJsonObject>* OutValueDefinition)>;
using FStructFieldFilter =
    TFunction<bool(const UScriptStruct *Struct, const FProperty *Field)>;
// Seed native state selected by the preparation owner before recursive decoding.
// This is not a conversion hook: types, fields and references still use this codec.
using FScopedValueInitializer = TFunction<bool(FProperty*, const TSharedPtr<FJsonValue>&,
    void*, bool, FDiagnostic&)>;
// Optional reference scope for transient consumers. Type validation and recursive
// container decoding remain in this codec; the scope only maps references.
// Decoding maps the scoped spelling to the ordinary reference value, not to a
// loaded UObject. Soft paths must retain their lazy-loading semantics.
using FScopedReferenceEncoder = TFunction<TSharedPtr<FJsonValue>(UObject*, const FString&)>;
using FScopedReferenceDecoder = TFunction<bool(const TSharedPtr<FJsonValue>&, TSharedPtr<FJsonValue>&, FDiagnostic&)>;

struct FOptions {
  bool bAllowPartialStructFields = false;
  FObjectReferenceEncoder ObjectReferenceEncoder;
  bool bCaptureOwnedObjectDefinitions = false;
  FStructFieldFilter StructFieldFilter;
  // Opt-in native field type/candidate descriptors, consumed by public definitions.
  bool bCaptureStructFieldDescriptors = false;
  // Encode-only traversal guard for native Struct element templates. Captured
  // instances retain their existing traversal and identity semantics.
  TSet<const UScriptStruct*> ElementDefinitionStack;
  FScopedReferenceEncoder ScopedReferenceEncoder;
  FScopedReferenceDecoder ScopedReferenceDecoder;
  FScopedValueInitializer ScopedValueInitializer;
  // Stored-value equality is used for write elision and identities. Intent
  // verification additionally permits the codec's native effective-value semantics.
  bool bCompareStoredValues = false;
  // Verification callers with a freshly resolved live member may provide its
  // containing struct. Never inferred from detached/standalone property storage.
  const void* NativeMemberContainer = nullptr;
};

// Shared enum name/value contract; unnamed native values encode as an empty name.
UEAGENTINTERFACE_API FString EncodeEnumValueName(const UEnum* Enum, int64 Raw);
UEAGENTINTERFACE_API bool DecodeEnumValue(const UEnum* Enum, const TSharedPtr<FJsonValue>& Value, int64& OutRaw, FDiagnostic& OutError);

// Shared ordinary reference shape and hard-reference resolution. Null has no path.
UEAGENTINTERFACE_API TSharedPtr<FJsonValue> EncodeReferencePath(const FString& Path);
UEAGENTINTERFACE_API bool DecodeReferencePath(const TSharedPtr<FJsonValue>& Value, TOptional<FString>& OutPath, FDiagnostic& OutError);
UEAGENTINTERFACE_API bool DecodeObjectReference(const TSharedPtr<FJsonValue>& Value, UClass* PropertyClass,
                                               UClass* MetaClass, UObject*& OutObject, FDiagnostic& OutError,
                                               const FScopedReferenceDecoder& Resolver = {});

UEAGENTINTERFACE_API FString GetValueKind(const FProperty *Property);
UEAGENTINTERFACE_API bool IsSupported(const FProperty *Property,
                                      FDiagnostic& OutReason);
UEAGENTINTERFACE_API bool IsSupported(const FProperty *Property,
                                      const void* ValuePtr, FDiagnostic& OutReason);
UEAGENTINTERFACE_API bool IsAuthoringSupported(const FProperty *Property,
                                               const void *ValuePtr,
                                               FDiagnostic& OutReason);
UEAGENTINTERFACE_API bool IsStructFieldAuthoringEditable(const FProperty *Property, const UScriptStruct* Scope = nullptr);
// Type-level protocol qualification, independent of current values/container size.
UEAGENTINTERFACE_API bool RequiresHashSource(const FProperty *Property);
// Ordinary authoring projection only. Complete captured values remain unchanged.
// Returns false for a fixed shape with no actionable members in its captured schema.
// Projected Struct outputs retain an empty fields shell when no members remain.
UEAGENTINTERFACE_API bool ProjectAuthoringValue(
    const TSharedPtr<FJsonObject>& Type, const TSharedPtr<FJsonValue>& Value,
    const TSharedPtr<FJsonValue>& Definition, TSharedPtr<FJsonValue>& OutValue,
    TSharedPtr<FJsonValue>& OutDefinition);
UEAGENTINTERFACE_API TSharedPtr<FJsonObject>
BuildTypeDescriptor(const FProperty *Property, bool bIgnoreFixedArray = false);
UEAGENTINTERFACE_API void
AddTypeMetadata(const FProperty *Property,
                const TSharedPtr<FJsonObject> &Target);
UEAGENTINTERFACE_API bool
ValidateTypeMetadata(const FProperty *Property,
                     const TSharedPtr<FJsonObject> &Descriptor,
                     FDiagnostic& OutError);
UEAGENTINTERFACE_API bool
ValidateTypeMetadata(const FProperty *Property,
                     const TSharedPtr<FJsonObject> &Descriptor,
                     FDiagnostic& OutError, bool bSingleFixedElement);

UEAGENTINTERFACE_API TSharedPtr<FJsonValue>
Encode(FProperty *Property, const void *ValuePtr, UObject *Owner,
       const FOptions &Options, int32 Depth = 0,
       TSharedPtr<FJsonObject> *OutValueDefinition = nullptr,
       bool bIgnoreFixedArray = false);

// Observe ordinary asset/soft-reference values through this codec without
// rebuilding reflection descriptors. No scoped/custom encoder is applied.
// Owned/inline objects, non-asset hard references and other shapes return false.
// The result owns only JSON paths; it never retains native object storage.
UEAGENTINTERFACE_API bool TryEncodePathOnlyAssetReferences(FProperty* Property,
    const void* ValuePtr, UObject* Owner, TSharedPtr<FJsonValue>& OutValue);

UEAGENTINTERFACE_API TSharedPtr<FJsonValue>
EncodeStructValue(UScriptStruct *Struct, const void *ValuePtr, UObject *Owner,
                  const FOptions &Options, int32 Depth = 0,
                  TSharedPtr<FJsonObject>* OutValueDefinition = nullptr);

// Derive sparse reflected fields from a complete capture and an actual native
// default instance. Does not alter Fields or infer defaults from schema examples.
// Omitted values retain their captured type and comparison provenance.
UEAGENTINTERFACE_API TSharedPtr<FJsonObject> ProjectNativeDefaultStructFields(
    UScriptStruct* Struct, const void* NativeData, const void* DefaultData,
    const TSharedPtr<FJsonObject>& Fields);

UEAGENTINTERFACE_API bool Decode(FProperty *Property,
                                 const TSharedPtr<FJsonValue> &Value,
                                 void *OutValuePtr, UObject *Owner,
                                 const FOptions &Options, FDiagnostic& OutError,
                                 int32 Depth = 0, bool bSingleFixedElement = false);

// Native float conversion shares scalar/struct range rules and preserves output on failure.
UEAGENTINTERFACE_API bool TryConvertFloat32(double Number, float& OutValue, FDiagnostic& OutError);
// Numeric selectors use the same exact number lexeme conversion as integer properties.
UEAGENTINTERFACE_API bool TryReadIntegerNumber(const TSharedPtr<FJsonValue>& Value,
                                             int64 Min, int64 Max, int64& OutValue, FDiagnostic& OutError);
// Native layout coordinates keep their existing truncation or nearest rounding.
// Check the normalized number before integer narrowing; failure preserves output.
UEAGENTINTERFACE_API bool TryConvertInt32(double Number, int32& OutValue, FDiagnostic& OutError,
                                        bool bRoundToNearest = false);

// Orientation equality uses an angular tolerance of KINDA_SMALL_NUMBER degrees,
// not a quaternion dot-product tolerance. Shared by native and canonical values.
// Finite nonzero orientations normalize before native narrowing or squared-length calculations.
UEAGENTINTERFACE_API bool TryNormalizeQuaternion(FQuat& Value, FDiagnostic& OutError);
UEAGENTINTERFACE_API bool AreRotationsEquivalent(const FQuat& Left, const FQuat& Right);

// Explicit intent semantics for a partial native transform's translation/scale.
// Ordinary vectors and stored-value comparisons retain exact native identity.
UEAGENTINTERFACE_API TSharedRef<FJsonObject> MakeTransformVectorComparisonDefinition();

// Uses the same formal type and native decoder as HJSON authoring. Unset means
// a recursive record/container, whose child descriptors are visited by the caller.
UEAGENTINTERFACE_API TOptional<FValueComparison> CompareCanonicalValues(
    const TSharedPtr<FJsonObject>& Type, const TSharedPtr<FJsonObject>& Definition,
    const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right,
    bool bStoredValues = false);

UEAGENTINTERFACE_API FValueComparison ComparePropertyValues(FProperty* Property,
    const void* ExpectedValuePtr, const void* ActualValuePtr, UObject* Owner,
    const FOptions& Options);

UEAGENTINTERFACE_API FValueComparison CompareReflectedStructFields(const UScriptStruct* Struct,
    const void* Expected, const void* Actual, UObject* Owner, const FOptions& Options);

UEAGENTINTERFACE_API bool AreSemanticallyEquivalent(FProperty *Property,
                                                    const void *LeftValuePtr,
                                                    const void *RightValuePtr,
                                                    UObject *Owner,
                                                    const FOptions &Options);

// Check explicit decoded intent against current native storage. Unspecified
// struct members inherit the actual value when partial fields are enabled;
// the caller supplies the original input and its reference/member scope.
UEAGENTINTERFACE_API bool VerifyValueIntent(FProperty* Property,
    const TSharedPtr<FJsonValue>& Intent, const void* ActualValuePtr, UObject* Owner,
    const FOptions& Options, FDiagnostic& OutError, bool bSingleFixedElement = false);

// Compare a reflected field record using the same exportable-member boundary as
// nested struct properties. Transient/deprecated fields are not authored intent.
UEAGENTINTERFACE_API bool AreReflectedStructFieldsEquivalent(const UScriptStruct* Struct,
    const void* Left, const void* Right, UObject* Owner, const FOptions& Options);

UEAGENTINTERFACE_API bool DecodeStructValue(UScriptStruct *Struct,
                                            const TSharedPtr<FJsonValue> &Value,
                                            void *OutValuePtr,
                                            FDiagnostic& OutError);

UEAGENTINTERFACE_API bool DecodeStructValue(UScriptStruct *Struct,
                                            const TSharedPtr<FJsonValue> &Value,
                                            void *OutValuePtr, UObject *Owner,
                                            const FOptions &Options,
                                            FDiagnostic& OutError, int32 Depth = 0);

UEAGENTINTERFACE_API bool
IsReliableCanonicalValue(const FProperty *Property,
                         const TSharedPtr<FJsonValue> &Value,
                         FDiagnostic* OutReason = nullptr);

UEAGENTINTERFACE_API TSharedPtr<FJsonValue>
EncodePropertyBag(const FInstancedPropertyBag &Bag, UObject *Owner,
                  const FOptions &Options, int32 Depth = 0,
                  TSharedPtr<FJsonObject>* OutValueDefinition = nullptr);

UEAGENTINTERFACE_API bool DecodePropertyBag(const TSharedPtr<FJsonValue> &Value,
                                            FInstancedPropertyBag &OutBag,
                                            UObject *Owner,
                                            const FOptions &Options,
                                            FDiagnostic& OutError, int32 Depth = 0);

// Validate the complete public shape and seed the desired schema from native
// identities/values, without decoding authored fields. OutBag is unchanged on failure.
UEAGENTINTERFACE_API bool PreparePropertyBagSchema(const TSharedPtr<FJsonValue>& Value,
                                                  FInstancedPropertyBag& OutBag,
                                                  const FOptions& Options,
                                                  FDiagnostic& OutError, int32 Depth = 0);
} // namespace UeAgentPropertyValueCodec
