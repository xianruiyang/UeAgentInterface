// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class FObjectPropertyBase;
class FProperty;
class UObject;
struct FInstancedPropertyBag;
class UScriptStruct;

namespace UeAgentPropertyValueCodec {
using FObjectReferenceEncoder = TFunction<bool(
    const FObjectPropertyBase *Property, const void *ValuePtr, UObject *Owner,
    int32 Depth, TSharedPtr<FJsonValue> &OutValue)>;
using FStructFieldFilter =
    TFunction<bool(const UScriptStruct *Struct, const FProperty *Field)>;

struct FOptions {
  int32 MaxDepth = -1;
  int32 MaxContainerItems = 256;
  int32 MaxStringChars = 4096;
  bool bAllowPartialStructFields = false;
  FObjectReferenceEncoder ObjectReferenceEncoder;
  FStructFieldFilter StructFieldFilter;
};

UEAGENTINTERFACE_API FString GetValueKind(const FProperty *Property);
UEAGENTINTERFACE_API bool IsSupported(const FProperty *Property,
                                      FString &OutReason);
UEAGENTINTERFACE_API bool IsAuthoringSupported(const FProperty *Property,
                                               const void *ValuePtr,
                                               FString &OutReason);
UEAGENTINTERFACE_API TSharedPtr<FJsonObject>
BuildTypeDescriptor(const FProperty *Property);
UEAGENTINTERFACE_API void
AddTypeMetadata(const FProperty *Property,
                const TSharedPtr<FJsonObject> &Target);
UEAGENTINTERFACE_API bool
ValidateTypeMetadata(const FProperty *Property,
                     const TSharedPtr<FJsonObject> &Descriptor,
                     FString &OutError);

UEAGENTINTERFACE_API TSharedPtr<FJsonValue>
Encode(FProperty *Property, const void *ValuePtr, UObject *Owner,
       const FOptions &Options, int32 Depth = 0);

UEAGENTINTERFACE_API TSharedPtr<FJsonValue>
EncodeStructValue(UScriptStruct *Struct, const void *ValuePtr, UObject *Owner,
                  const FOptions &Options, int32 Depth = 0);

UEAGENTINTERFACE_API bool Decode(FProperty *Property,
                                 const TSharedPtr<FJsonValue> &Value,
                                 void *OutValuePtr, UObject *Owner,
                                 const FOptions &Options, FString &OutError,
                                 int32 Depth = 0);

UEAGENTINTERFACE_API bool AreSemanticallyEquivalent(FProperty *Property,
                                                    const void *LeftValuePtr,
                                                    const void *RightValuePtr,
                                                    UObject *Owner,
                                                    const FOptions &Options);

UEAGENTINTERFACE_API bool DecodeStructValue(UScriptStruct *Struct,
                                            const TSharedPtr<FJsonValue> &Value,
                                            void *OutValuePtr,
                                            FString &OutError);

UEAGENTINTERFACE_API bool DecodeStructValue(UScriptStruct *Struct,
                                            const TSharedPtr<FJsonValue> &Value,
                                            void *OutValuePtr, UObject *Owner,
                                            const FOptions &Options,
                                            FString &OutError, int32 Depth = 0);

UEAGENTINTERFACE_API bool
IsReliableCanonicalValue(const FProperty *Property,
                         const TSharedPtr<FJsonValue> &Value,
                         FString *OutReason = nullptr);

UEAGENTINTERFACE_API TSharedPtr<FJsonValue>
EncodePropertyBag(const FInstancedPropertyBag &Bag, UObject *Owner,
                  const FOptions &Options, int32 Depth = 0);

UEAGENTINTERFACE_API bool DecodePropertyBag(const TSharedPtr<FJsonValue> &Value,
                                            FInstancedPropertyBag &OutBag,
                                            UObject *Owner,
                                            const FOptions &Options,
                                            FString &OutError, int32 Depth = 0);
} // namespace UeAgentPropertyValueCodec
