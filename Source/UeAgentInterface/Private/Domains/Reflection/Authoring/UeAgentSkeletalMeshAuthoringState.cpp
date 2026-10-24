#include "Domains/Reflection/Authoring/UeAgentSkeletalMeshAuthoringState.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileState.h"

#include "Animation/Skeleton.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectStructuredAssetMechanics.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshLODSettings.h"
#include "SkinnedAssetCompiler.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentNativeNormalizationIntent.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

namespace UeAgentReflectionAuthoring::SkeletalMeshState
{
using namespace UE::UAI;
namespace
{
using FDiagnostic = UE::UAI::Contract::FDiagnostic;

FDiagnostic ValueFailure(const FString& Code, const FString& Reason, const FString& Path,
    const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
{
    auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Path, Expected, Actual, TEXT("skeletal_mesh_state"), Reason);
    Error.Source.InternalJsonPath = Path;
    Error.ActualValuePresent = Actual.IsValid();
    Error.ExpectedValuePresent = Expected.IsValid();
    return Error;
}

bool InputFailure(FDiagnostic& Error, const TSharedPtr<FJsonValue>& Input, const FString& Code,
    const FString& Reason, const FString& Path, const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
{
    Error = ValueFailure(Code, Reason, Path, Actual, Expected);
    if (Input && Input->Type == EJson::Object) ProfileState::BindInput(Error, Input);
    else Error.SourceKind = TEXT("authored_value");
    return false;
}

struct FSettingsPlan
{
    UeAgentAuthoringObjectApply::FPreparedApply Apply;
    TSharedPtr<FJsonValue> Settings;
    FString Pointer;
};

UScriptStruct* BoundsType()
{
    const auto* Property = FindFProperty<FStructProperty>(USkeletalMesh::StaticClass(), TEXT("ImportedBounds"));
    return Property ? Property->Struct : nullptr;
}

bool Capture(UScriptStruct* Type, const void* Data, USkeletalMesh& Mesh, const FString& Field,
    UeAgentAuthoringState::FStateSnapshot& Snapshot, TSharedPtr<FJsonObject>& Categories, UE::UAI::Contract::FDiagnostic& Error)
{
    if (!Type)
    {
        Error = ValueFailure(TEXT("skeletal_mesh_native_struct_missing"),
            TEXT("The native reflected struct type required to read these skeletal mesh settings is unavailable."),
            Field, MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("a valid UScriptStruct for this field")));
        Error.SourceKind = TEXT("native_result");
        Error.TargetPath = Mesh.GetPathName();
        return false;
    }
    return UeAgentDetailsAuthoringService::CaptureStructProperties(Type, Data, Mesh.GetPathName(), &Mesh,
        {}, Snapshot, Categories, Error)
        && UeAgentDetailsAuthoringService::ProjectReadableProperties(Snapshot, Categories, Error);
}

bool Prepare(UScriptStruct* Type, void* Data, USkeletalMesh& Mesh, const TSharedPtr<FJsonValue>& Value,
    const FString& Pointer, const TSharedPtr<FJsonValue>& Input, FSettingsPlan& Plan, FDiagnostic& Error)
{
    UeAgentAuthoringState::FStateSnapshot Snapshot;
    TSharedPtr<FJsonObject> Categories;
    TArray<TSharedPtr<FJsonValue>> Descriptors;
    if (!Capture(Type, Data, Mesh, Pointer, Snapshot, Categories, Error)) return false;
    if (!UeAgentDetailsAuthoringService::DecodeReadableProperties(Snapshot, Value, Descriptors, Error))
    {
        ProfileState::PrefixInputLocation(Error, Pointer);
        ProfileState::BindInput(Error, Input);
        return false;
    }
    Plan.Settings = Value; Plan.Pointer = Pointer;
    UeAgentAuthoringObjectApply::FOptions Options;
    Options.bCompactValues = true;
    Options.bEmitEditorNotifications = false;
    Options.bMarkPackageDirty = false;
    Options.bAllowDomainAuthorizedOwners = true;
    Options.DiagnosticTargetPath = Mesh.GetPathName();
    UeAgentAuthoringObjectApply::FFailure Failure;
    if (!UeAgentAuthoringObjectApply::PrepareStruct(Type, Data, &Mesh, Descriptors, Options, Plan.Apply, Failure))
    {
        Error = MoveTemp(Failure.Diagnostic);
        ProfileState::LocateSettingsFailure(Error, Plan.Settings, Plan.Pointer, Input);
        return false;
    }
    return true;
}
}

bool Validate(const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error)
{
    if (!Value || Value->Type != EJson::Object)
        return InputFailure(Error, Value, TEXT("skeletal_mesh_structure_object_required"),
            TEXT("Skeletal mesh structure must be an object."), TEXT("/"), Value, MakeShared<FJsonValueString>(TEXT("an object")));
    for (const auto& Field : Value->AsObject()->Values)
        if (Field.Key != TEXT("materials") && Field.Key != TEXT("bounds")
            && Field.Key != TEXT("lod_settings_ref") && Field.Key != TEXT("skeleton_ref"))
            return InputFailure(Error, Value, TEXT("skeletal_mesh_structure_unknown_field"),
                TEXT("This field is not supported in skeletal mesh structure."),
                TEXT("/") + Field.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")),
                MakeShared<FJsonValueString>(Field.Key),
                MakeShared<FJsonValueString>(TEXT("materials, bounds, lod_settings_ref, or skeleton_ref")));
    if (const auto Materials = Value->AsObject()->TryGetField(TEXT("materials")))
    {
        if (Materials->Type != EJson::Array)
            return InputFailure(Error, Value, TEXT("skeletal_mesh_material_array_required"),
                TEXT("Skeletal mesh material entries must be an array."), TEXT("/materials"),
                Materials, MakeShared<FJsonValueString>(TEXT("an array")));
        for (int32 Index = 0; Index < Materials->AsArray().Num(); ++Index)
        {
            const auto& Material = Materials->AsArray()[Index];
            if (!Material || Material->Type != EJson::Object)
                return InputFailure(Error, Value, TEXT("skeletal_mesh_material_properties_required"),
                    TEXT("Each skeletal mesh material entry must be an object."),
                    TEXT("/materials/") + FString::FromInt(Index), Material, MakeShared<FJsonValueString>(TEXT("an object")));
        }
    }
    return true;
}

bool Read(USkeletalMesh& Mesh, const FString&, TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error)
{
    FSkinnedAssetCompilingManager::Get().FinishCompilation({&Mesh});
    auto Result = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Materials;
    UeAgentAuthoringState::FStateSnapshot Snapshot;
    TSharedPtr<FJsonObject> Categories;
    for (int32 Index = 0; Index < Mesh.GetMaterials().Num(); ++Index)
    {
        const auto& Material = Mesh.GetMaterials()[Index];
        if (!Capture(FSkeletalMaterial::StaticStruct(), &Material, Mesh,
            TEXT("/materials/") + FString::FromInt(Index), Snapshot, Categories, Error)) return false;
        FString Key;
        HjsonWorkspace::FWorkspaceError Failure;
        if (!HjsonWorkspace::TryFormatReadableKeyedItemOrdinal(TEXT("Entry"), Index, Key, Failure))
        {
            Error = Failure.GetDiagnostic();
            ProfileState::MarkNative(Error);
            return false;
        }
        Categories->SetStringField(TEXT("instance"), Key);
        Materials.Add(MakeShared<FJsonValueObject>(Categories));
    }
    Result->SetArrayField(TEXT("materials"), Materials);
    const FBoxSphereBounds Bounds = Mesh.GetImportedBounds();
    if (!Capture(BoundsType(), &Bounds, Mesh, TEXT("/bounds"), Snapshot, Categories, Error)) return false;
    Result->SetObjectField(TEXT("bounds"), Categories);
    Result->SetField(TEXT("lod_settings_ref"), UeAgentPropertyValueCodec::EncodeReferencePath(Mesh.GetLODSettings() ? Mesh.GetLODSettings()->GetPathName() : FString()));
    Result->SetField(TEXT("skeleton_ref"), UeAgentPropertyValueCodec::EncodeReferencePath(Mesh.GetSkeleton() ? Mesh.GetSkeleton()->GetPathName() : FString()));
    Value = MakeShared<FJsonValueObject>(Result);
    return true;
}

bool Apply(USkeletalMesh& Mesh, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error)
{
    if (!Validate(Value, Error)) return false;
    FSkinnedAssetCompilingManager::Get().FinishCompilation({&Mesh});
    auto Materials = Mesh.GetMaterials();
    auto Bounds = Mesh.GetImportedBounds();
    UObject* LodSettings = Mesh.GetLODSettings();
    UObject* Skeleton = Mesh.GetSkeleton();
    const auto Object = Value->AsObject();
    if (const auto Ref = Object->TryGetField(TEXT("lod_settings_ref")); Ref
        && !UeAgentPropertyValueCodec::DecodeObjectReference(Ref, USkeletalMeshLODSettings::StaticClass(), nullptr, LodSettings, Error))
    {
        ProfileState::PrefixInputLocation(Error, TEXT("/lod_settings_ref"));
        ProfileState::BindInput(Error, Value);
        return false;
    }
    if (const auto Ref = Object->TryGetField(TEXT("skeleton_ref")); Ref
        && !UeAgentPropertyValueCodec::DecodeObjectReference(Ref, USkeleton::StaticClass(), nullptr, Skeleton, Error))
    {
        ProfileState::PrefixInputLocation(Error, TEXT("/skeleton_ref"));
        ProfileState::BindInput(Error, Value);
        return false;
    }
    TArray<FSettingsPlan> Plans;
    TArray<int32> MaterialSources;
    const bool bHasMaterials = Object->HasField(TEXT("materials"));
    if (const auto Desired = Object->TryGetField(TEXT("materials")))
    {
        auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
        StateSemantic::FUeAgentCanonicalValuePtr Canonical;
        TArray<HjsonWorkspace::FOrderedStructuredItemBinding> Bindings;
        HjsonWorkspace::FWorkspaceError Failure;
        if (!StateSemantic::CanonicalJson::TryFromJsonValue(Desired, Resources, Canonical, Error))
        {
            ProfileState::PrefixInputLocation(Error, TEXT("/materials"));
            ProfileState::BindInput(Error, Value);
            return false;
        }
        if (!HjsonWorkspace::TryResolveOrderedStructuredItems(*Canonical, Materials.Num(), Resources, Bindings, Failure))
        {
            Error = Failure.GetDiagnostic();
            ProfileState::PrefixInputLocation(Error, TEXT("/materials"));
            ProfileState::BindInput(Error, Value);
            return false;
        }
        const auto Before = MoveTemp(Materials);
        // Finish allocating storage before preparing plans that retain its addresses.
        Materials.SetNum(Bindings.Num());
        for (int32 Index = 0; Index < Bindings.Num(); ++Index)
        {
            const auto& Binding = Bindings[Index];
            if (Binding.CapturedIndex != INDEX_NONE) Materials[Index] = Before[Binding.CapturedIndex];
            MaterialSources.Add(Binding.CapturedIndex);
            TSharedPtr<FJsonValue> Row;
            if (!StateSemantic::CanonicalJson::TryToJsonValue(*Binding.Value, Row, Error))
            { ProfileState::MarkNative(Error); return false; }
            // Binding.Value excludes instance metadata for the category decoder.
            // Its index still identifies the original input row for diagnostics.
            if (!Prepare(FSkeletalMaterial::StaticStruct(), &Materials[Index], Mesh, Row,
                TEXT("/materials/") + FString::FromInt(Index), Value, Plans.AddDefaulted_GetRef(), Error)) return false;
        }
    }
    if (const auto Desired = Object->TryGetField(TEXT("bounds")); Desired
        && !Prepare(BoundsType(), &Bounds, Mesh, Desired, TEXT("/bounds"), Value, Plans.AddDefaulted_GetRef(), Error)) return false;
    for (auto& Plan : Plans)
    {
        UeAgentAuthoringObjectApply::FApplyResult Result;
        if (!UeAgentAuthoringObjectApply::ApplyPrepared(Plan.Apply, Result))
        {
            Error = MoveTemp(Result.Diagnostic);
            ProfileState::LocateSettingsFailure(Error, Plan.Settings, Plan.Pointer, Value);
            return false;
        }
    }
    const auto RejectBounds = [&](const FString& Field, double Actual)
    {
        Error = ValueFailure(TEXT("skeletal_mesh_bounds_invalid"),
            TEXT("A skeletal mesh bounds extent or sphere radius is negative."),
            Field, MakeShared<FJsonValueNumber>(Actual), MakeShared<FJsonValueString>(TEXT("a value greater than or equal to zero")));
        Error.TargetPath = Mesh.GetPathName();
        ProfileState::LocateSettingsFailure(Error, Object->TryGetField(TEXT("bounds")), TEXT("/bounds"), Value);
        return false;
    };
    if (Bounds.BoxExtent.X < 0) return RejectBounds(TEXT("BoxExtent.X"), Bounds.BoxExtent.X);
    if (Bounds.BoxExtent.Y < 0) return RejectBounds(TEXT("BoxExtent.Y"), Bounds.BoxExtent.Y);
    if (Bounds.BoxExtent.Z < 0) return RejectBounds(TEXT("BoxExtent.Z"), Bounds.BoxExtent.Z);
    if (Bounds.SphereRadius < 0) return RejectBounds(TEXT("SphereRadius"), Bounds.SphereRadius);
    // An unchanged reference must not overwrite explicit root LODInfo edits with
    // the shared LOD asset's defaults. A changed reference retains native setter semantics.
    const bool bApplyLodSettings = LodSettings != Mesh.GetLODSettings();
    UE::UAI::DomainAdapters::AssetDataSimpleEffect::ReplaceSkeletalMeshSettings(Mesh, Materials,
        Cast<USkeletalMeshLODSettings>(LodSettings), Bounds, Cast<USkeleton>(Skeleton), bApplyLodSettings,
        bHasMaterials ? &MaterialSources : nullptr);
    if (bHasMaterials)
    {
        // Rebind the existing native property intentions to the published array.
        // Its storage differs from the prepared local arguments after SetMaterials.
        if (Mesh.GetMaterials().Num() != MaterialSources.Num())
        {
            auto Warning = ValueFailure(TEXT("skeletal_mesh_material_count_not_retained"),
                TEXT("The skeletal mesh did not retain the requested material count."), TEXT("materials.count"),
                MakeShared<FJsonValueNumber>(Mesh.GetMaterials().Num()), MakeShared<FJsonValueNumber>(MaterialSources.Num()));
            Warning.Stage = TEXT("skeletal_mesh_observation");
            Warning.SourceKind = TEXT("native_result");
            Warning.TargetPath = Mesh.GetPathName();
            ProfileState::AddRelatedInput(Warning, Value, TEXT("/materials"));
            UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Warning);
        }
        for (int32 Index = 0; Index < FMath::Min(MaterialSources.Num(), Mesh.GetMaterials().Num()); ++Index)
        {
            UeAgentAuthoringObjectApply::FApplyResult Result;
            if (!UeAgentAuthoringObjectApply::VerifyPreparedStruct(Plans[Index].Apply, FSkeletalMaterial::StaticStruct(),
                &Mesh.GetMaterials()[Index], &Mesh, Result))
            {
                ProfileState::LocateSettingsFailure(Result.Diagnostic, Plans[Index].Settings, Plans[Index].Pointer, Value);
                UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Result.Diagnostic.Code, Result.Diagnostic, TEXT("skeletal_mesh_observation"));
            }
        }
        UeAgentAuthoringTypeDescriptors::FDescriptor Type;
        Type.Kind = TEXT("array"); Type.ElementType = MakeShared<UeAgentAuthoringTypeDescriptors::FDescriptor>();
        Type.ElementType->Kind = TEXT("object");
        // Every readable source field has independently passed the common
        // native property intention check, before and after the native setter.
        // FinalizeValues then binds generated Entry metadata/defaults to the
        // isolated proposal, rather than to the actual written asset.
        const auto Source = Object->TryGetField(TEXT("materials"));
        if (!UeAgentNativeNormalizationIntent::RecordValue(TEXT("/materials"), Source, Source, Error, &Type)) {
            if (Error.PropertyPath.IsEmpty()) Error.PropertyPath = TEXT("/materials");
            if (Error.TargetPath.IsEmpty()) Error.TargetPath = Mesh.GetPathName();
            UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Object, TEXT("materials"));
            UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Error);
            Error.Reset();
        }
    }
    return true;
}

void Settle(USkeletalMesh& Mesh)
{
    FSkinnedAssetCompilingManager::Get().FinishCompilation({&Mesh});
    Mesh.CalculateExtendedBounds();
}
}
