// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UNiagaraEmitter;
class UNiagaraRendererProperties;
class FProperty;
class FStructProperty;
class UScriptStruct;
class FJsonObject;
class FJsonValue;
struct FVersionedNiagaraEmitter;
struct FNiagaraVariableAttributeBinding;
namespace UeAgentAuthoringObjectApply { class FPropertyIntent; struct FFailure; }

namespace UeAgentNiagaraRendererBindings
{
    // Native Details edits a variable name, not the binding's derived caches.
    bool IncludeAuthoringField(const UScriptStruct* Struct, const FProperty* Field);
    bool AuthorizeAuthoringField(const FProperty* Field);
    UEAGENTINTERFACE_API TSharedPtr<FJsonValue> EncodeAuthoringBinding(FStructProperty& Property,
        const FNiagaraVariableAttributeBinding& Binding, UNiagaraRendererProperties& Renderer);
    UEAGENTINTERFACE_API bool PrepareAuthoringBinding(FStructProperty& Property,
        const TSharedPtr<FJsonObject>& Descriptor, UNiagaraRendererProperties& Renderer,
        const FVersionedNiagaraEmitter& Emitter, FName& OutName,
        UeAgentAuthoringObjectApply::FPropertyIntent& OutIntent,
        UeAgentAuthoringObjectApply::FFailure& OutFailure);

    // Recompute native binding derivatives without changing authored bindings.
    void Refresh(const FVersionedNiagaraEmitter& Emitter);

    // Native merge compares serialized renderer fields, including binding caches.
    void PrepareForMerge(UNiagaraEmitter& Emitter);
}
