// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/UeAgentNiagaraRendererBindings.h"

#include "NiagaraEmitter.h"
#include "NiagaraRendererProperties.h"
#include "UObject/UnrealType.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"

namespace UeAgentNiagaraRendererBindings
{
    namespace
    {
        void RefreshValue(FProperty& Property, void* Value,
                          const FVersionedNiagaraEmitter& Emitter,
                          ENiagaraRendererSourceDataMode SourceMode)
        {
            if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
            {
                if (Struct->Struct == FNiagaraVariableAttributeBinding::StaticStruct())
                {
                    static_cast<FNiagaraVariableAttributeBinding*>(Value)->CacheValues(Emitter, SourceMode);
                }
                else if (Struct->Struct == FNiagaraMaterialAttributeBinding::StaticStruct())
                {
                    static_cast<FNiagaraMaterialAttributeBinding*>(Value)->CacheValues(Emitter.Emitter);
                }
                else
                {
                    for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
                        for (int32 Index = 0; Index < It->ArrayDim; ++Index)
                            RefreshValue(**It, It->ContainerPtrToValuePtr<void>(Value, Index), Emitter, SourceMode);
                }
            }
            else if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
            {
                FScriptArrayHelper Items(Array, Value);
                for (int32 Index = 0; Index < Items.Num(); ++Index)
                    RefreshValue(*Array->Inner, Items.GetRawPtr(Index), Emitter, SourceMode);
            }
        }
    }

    bool IncludeAuthoringField(const UScriptStruct* Struct, const FProperty* Field)
    {
        if (Struct == FNiagaraVariableAttributeBinding::StaticStruct())
            return Field->GetFName() == TEXT("RootVariable");
        if (Struct == FNiagaraVariable::StaticStruct())
            return Field->GetFName() == TEXT("Name");
        return true;
    }

    bool AuthorizeAuthoringField(const FProperty* Field)
    {
        return Field && Field->GetOwnerStruct() == FNiagaraVariableAttributeBinding::StaticStruct()
            && Field->GetFName() == TEXT("RootVariable");
    }

    TSharedPtr<FJsonValue> EncodeAuthoringBinding(FStructProperty& Property,
        const FNiagaraVariableAttributeBinding& Binding, UNiagaraRendererProperties& Renderer)
    {
        FNiagaraVariableAttributeBinding View = Binding;
        // Keep implicit source-following bindings implicit. Explicit bindings
        // need the native display namespace to survive export and creation.
        if (View.GetBindingSourceMode() != ENiagaraBindingSource::ImplicitFromSource)
        {
            auto* Root = FindFProperty<FStructProperty>(Property.Struct, TEXT("RootVariable"));
            Root->ContainerPtrToValuePtr<FNiagaraVariable>(&View)->SetName(View.GetName());
        }
        UeAgentPropertyValueCodec::FOptions Options;
        Options.StructFieldFilter = IncludeAuthoringField;
        return UeAgentPropertyValueCodec::Encode(&Property, &View, &Renderer, Options);
    }

    bool PrepareAuthoringBinding(FStructProperty& Property,
        const TSharedPtr<FJsonObject>& Descriptor, UNiagaraRendererProperties& Renderer,
        const FVersionedNiagaraEmitter& Emitter, FName& OutName,
        UeAgentAuthoringObjectApply::FPropertyIntent& OutIntent,
        UeAgentAuthoringObjectApply::FFailure& OutFailure)
    {
        using namespace UeAgentAuthoringObjectApply;
        OutFailure = FFailure();
        auto Fail = [&](const UE::UAI::Contract::FDiagnostic& Error) {
            OutFailure.Diagnostic = Error;
            if (OutFailure.Diagnostic.PropertyPath.IsEmpty()) OutFailure.Diagnostic.PropertyPath = Property.GetName();
            if (Descriptor) UeAgentAuthoringDiagnostics::LocatePropertyValueSource(OutFailure.Diagnostic, *Descriptor);
            return false;
        };
        FNiagaraVariableAttributeBinding Expected = *Property.ContainerPtrToValuePtr<FNiagaraVariableAttributeBinding>(&Renderer);
        UeAgentPropertyValueCodec::FOptions Codec;
        Codec.StructFieldFilter = IncludeAuthoringField;
        UE::UAI::Contract::FDiagnostic Error;
        if (!Descriptor)
        {
            Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_renderer_binding_invalid"),
                TEXT("The renderer binding descriptor is missing."), TEXT("niagara_binding_prepare"));
            Error.PropertyPath = Property.GetName();
            Error.ActualValuePresent = false;
            Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a property binding descriptor"));
            return Fail(Error);
        }
        if (!UeAgentPropertyValueCodec::Decode(&Property,
            Descriptor->TryGetField(TEXT("value")), &Expected, &Renderer, Codec, Error)) return Fail(Error);
        auto* Root = FindFProperty<FStructProperty>(Property.Struct, TEXT("RootVariable"));
        OutName = Root->ContainerPtrToValuePtr<FNiagaraVariable>(&Expected)->GetName();
        Expected.SetValue(OutName, Emitter, Renderer.GetCurrentSourceMode());

        // Freeze only authoritative normalized fields on an isolated value.
        // Native caches may change with compilation; they are not user intent.
        TArray<TSharedPtr<FJsonValue>> Values;
        auto Add = [&](const TCHAR* Path, FProperty* Field, const void* Address) {
            auto Value = MakeShared<FJsonObject>();
            Value->SetStringField(TEXT("property_path"), Path);
            Value->SetField(TEXT("value"), UeAgentPropertyValueCodec::Encode(Field, Address, &Renderer, {}));
            const TSharedPtr<FJsonObject>* Source = nullptr;
            if (Descriptor->TryGetObjectField(TEXT("authoring_source"), Source)) Value->SetObjectField(TEXT("authoring_source"), *Source);
            Values.Add(MakeShared<FJsonValueObject>(Value));
        };
        auto* Name = FindFProperty<FNameProperty>(Root->Struct, TEXT("Name"));
        auto* Mode = FindFProperty<FProperty>(Property.Struct, TEXT("BindingSourceMode"));
        Add(TEXT("RootVariable.Name"), Name, Name->ContainerPtrToValuePtr<void>(Root->ContainerPtrToValuePtr<void>(&Expected)));
        Add(TEXT("BindingSourceMode"), Mode, Mode->ContainerPtrToValuePtr<void>(&Expected));
        FOptions Options;
        Options.bAllowDomainAuthorizedOwners = true;
        Options.bEmitEditorNotifications = false;
        Options.bMarkPackageDirty = false;
        Options.AuthorizeProperty = [](const FProperty* Field) {
            return AuthorizeAuthoringField(Field) || (Field && Field->GetOwnerStruct() == FNiagaraVariableAttributeBinding::StaticStruct()
                && Field->GetFName() == TEXT("BindingSourceMode"));
        };
        FPreparedApply Prepared;
        return PrepareStruct(Property.Struct, &Expected, &Renderer, Values, Options, Prepared, OutFailure)
            && Prepared.CaptureUnchangedIntent(OutIntent, OutFailure);
    }

    void Refresh(const FVersionedNiagaraEmitter& Emitter)
    {
        const FVersionedNiagaraEmitterData* Data = Emitter.GetEmitterData();
        if (!Data) return;
        for (UNiagaraRendererProperties* Renderer : Data->GetRenderers())
        {
            if (!Renderer) continue;
            // The renderer's aggregate refresh is protected. Use the native public
            // binding APIs for reflected bindings, including nested component and
            // material arrays which are absent from GetAttributeBindings().
            for (TFieldIterator<FProperty> It(Renderer->GetClass()); It; ++It)
                for (int32 Index = 0; Index < It->ArrayDim; ++Index)
                    RefreshValue(**It, It->ContainerPtrToValuePtr<void>(Renderer, Index),
                                 Emitter, Renderer->GetCurrentSourceMode());
        }
    }

    void PrepareForMerge(UNiagaraEmitter& Emitter)
    {
        for (const FNiagaraAssetVersion& Version : Emitter.GetAllAvailableVersions())
        {
            const FVersionedNiagaraEmitter Instance(&Emitter, Version.VersionGuid);
            const FVersionedNiagaraEmitterData* Data = Instance.GetEmitterData();
            if (!Data) continue;
            // Normalize each object in its own native emitter/source-mode context.
            // Otherwise compilation of only the instance can look like a local edit.
            Refresh(Data->GetParent());
            Refresh(Data->GetParentAtLastMerge());
            Refresh(Instance);
        }
    }
}
