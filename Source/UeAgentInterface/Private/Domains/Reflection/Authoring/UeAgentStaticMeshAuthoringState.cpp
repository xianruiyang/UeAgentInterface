// Copyright Epic Games, Inc. All Rights Reserved.
#include "Domains/Reflection/Authoring/UeAgentStaticMeshAuthoringState.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileState.h"

#include "Dom/JsonObject.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialPhysicsNative.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Misc/ScopeExit.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshCompiler.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"

namespace UeAgentReflectionAuthoring::StaticMeshState
{
namespace
{
bool IsCollisionProperty(const FProperty* Property)
{
    // Match StaticMeshEditor body details. Reflection owns the remaining types;
    // skeletal-only fields and the native hidden bounds flag are not mesh edits.
    static const TSet<FName> Hidden = {TEXT("BoneName"), TEXT("PhysicsType"),
        TEXT("bConsiderForBounds"), TEXT("CollisionReponse")};
    return Property && !Hidden.Contains(Property->GetFName());
}

using FDiagnostic = UE::UAI::Contract::FDiagnostic;

FDiagnostic ValueFailure(const FString& Code, const FString& Reason, const FString& Path,
    const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
{
    auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Path, Expected, Actual, TEXT("static_mesh_state"), Reason);
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

bool ProjectSettings(const UeAgentAuthoringState::FStateSnapshot& Snapshot,
    TSharedPtr<FJsonObject>& Document, UE::UAI::Contract::FDiagnostic& OutError)
{
    return UeAgentDetailsAuthoringService::ProjectReadableProperties(Snapshot, Document, OutError);
}

bool ReadSockets(UStaticMesh& Mesh, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
{
    auto Result = MakeShared<FJsonObject>();
    TSet<FName> Names;
    for (int32 Index = 0; Index < Mesh.Sockets.Num(); ++Index)
    {
        UStaticMeshSocket* Socket = Mesh.Sockets[Index];
        if (!Socket || Socket->SocketName.IsNone() || Names.Contains(Socket->SocketName))
        {
            OutError = ValueFailure(TEXT("static_mesh_socket_identity_invalid"),
                !Socket ? TEXT("The native socket slot is null.")
                    : Socket->SocketName.IsNone() ? TEXT("The native socket has a None name.")
                    : TEXT("The native socket duplicates an earlier socket's FName identity."),
                FString::Printf(TEXT("Sockets[%d].SocketName"), Index),
                Socket ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Socket->SocketName.ToString()))
                    : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
                MakeShared<FJsonValueString>(TEXT("a non-null socket with a unique non-None FName")));
            OutError.SourceKind = TEXT("native_result");
            OutError.TargetPath = Mesh.GetPathName();
            return false;
        }
        Names.Add(Socket->SocketName);
        UeAgentAuthoringState::FStateSnapshot Snapshot;
        TSharedPtr<FJsonObject> Document;
        if (!UeAgentDetailsAuthoringService::CaptureObjectProperties(Socket, {}, Snapshot, Document, OutError)
            || !ProjectSettings(Snapshot, Document, OutError)) return false;
        auto Record = MakeShared<FJsonObject>();
        Record->SetObjectField(TEXT("settings"), Document);
        Result->SetObjectField(Socket->SocketName.ToString(), Record);
    }
    UeAgentAuthoringEncodedTypes::FScope::RecordNameKeyedObject(Result);
    OutValue = MakeShared<FJsonValueObject>(Result);
    return true;
}

bool ResolveSockets(UStaticMesh& Mesh, const TSharedPtr<FJsonValue>& Desired, const TSharedPtr<FJsonValue>& Input,
    TSharedPtr<FJsonValue>& OutResolved, UE::UAI::Contract::FDiagnostic& OutError)
{
    using namespace UE::UAI;
    TSharedPtr<FJsonValue> Captured;
    if (!ReadSockets(Mesh, Captured, OutError)) return false;
    auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
    StateSemantic::FUeAgentCanonicalValuePtr Before, Proposed, Resolved;
    if (!StateSemantic::CanonicalJson::TryFromJsonValue(Captured, Resources, Before, OutError))
    { ProfileState::MarkNative(OutError); return false; }
    if (!StateSemantic::CanonicalJson::TryFromJsonValue(Desired, Resources, Proposed, OutError))
    {
        ProfileState::PrefixInputLocation(OutError, TEXT("/sockets"));
        ProfileState::BindInput(OutError, Input);
        return false;
    }
    HjsonWorkspace::FKeyedItemProjectionContract Contract;
    Contract.RootField = TEXT("sockets");
    Contract.bIdentityFromMapKey = true;
    Contract.bMapKeyUsesNameIdentity = true;
    Contract.bRequiresCreationCandidate = false;
    Contract.VisibleFieldPolicy.VisibleTopLevelFields = {TEXT("settings")};
    HjsonWorkspace::FWorkspaceError Error;
    bool Changed = false;
    if (!HjsonWorkspace::TryResolveKeyedItemDesiredState(*Proposed, *Before, TEXT("sockets"),
        Contract, nullptr, Resources, Resolved, Changed, Error))
    {
        OutError = Error.GetDiagnostic();
        ProfileState::PrefixInputLocation(OutError, TEXT("/sockets"));
        UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Input->AsObject(), TEXT("sockets"));
        return false;
    }
    if (!StateSemantic::CanonicalJson::TryToJsonValue(*Resolved, OutResolved, OutError))
    { ProfileState::MarkNative(OutError); return false; }
    return true;
}
}

TSharedPtr<FJsonObject> BuildValueDefinition()
{
    auto Sockets = MakeShared<FJsonObject>();
    auto Contract = MakeShared<FJsonObject>();
    Contract->SetStringField(TEXT("collection_semantics"), TEXT("keyed_desired_state"));
    Sockets->SetStringField(TEXT("type"), TEXT("object"));
    Sockets->SetObjectField(TEXT("contract"), Contract);
    auto Fields = MakeShared<FJsonObject>();
    Fields->SetObjectField(TEXT("sockets"), Sockets);
    auto Properties = MakeShared<FJsonObject>();
    Properties->SetStringField(TEXT("builtin"), TEXT("property_bag"));
    Fields->SetObjectField(TEXT("collision"), Properties);
    auto BuildSettings = MakeShared<FJsonObject>();
    BuildSettings->SetArrayField(TEXT("items"), TArray<TSharedPtr<FJsonValue>>{});
    BuildSettings->SetObjectField(TEXT("element_definition"), Properties);
    Fields->SetObjectField(TEXT("build_settings"), BuildSettings);
    auto Definition = MakeShared<FJsonObject>();
    Definition->SetObjectField(TEXT("fields"), Fields);
    return Definition;
}

bool Validate(const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError)
{
    if (!Value || Value->Type != EJson::Object)
        return InputFailure(OutError, Value, TEXT("static_mesh_structure_object_required"),
            TEXT("Static mesh structure must be an object."), TEXT("/"), Value, MakeShared<FJsonValueString>(TEXT("an object")));
    for (const auto& Field : Value->AsObject()->Values)
        if (Field.Key != TEXT("collision") && Field.Key != TEXT("build_settings") && Field.Key != TEXT("sockets"))
            return InputFailure(OutError, Value, TEXT("static_mesh_structure_unknown_field"),
                TEXT("This field is not supported in static mesh structure."),
                TEXT("/") + Field.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")),
                MakeShared<FJsonValueString>(Field.Key),
                MakeShared<FJsonValueString>(TEXT("collision, build_settings, or sockets")));
    if (const auto Collision = Value->AsObject()->TryGetField(TEXT("collision")))
    {
        if (Collision->Type != EJson::Object)
            return InputFailure(OutError, Value, TEXT("static_mesh_collision_properties_required"),
                TEXT("Static mesh collision settings must be an object."), TEXT("/collision"),
                Collision, MakeShared<FJsonValueString>(TEXT("an object")));
    }
    if (const auto Build = Value->AsObject()->TryGetField(TEXT("build_settings")))
    {
        if (Build->Type != EJson::Array)
            return InputFailure(OutError, Value, TEXT("static_mesh_build_settings_array_required"),
                TEXT("Static mesh build settings must be an array."), TEXT("/build_settings"),
                Build, MakeShared<FJsonValueString>(TEXT("an array")));
        if (Build->AsArray().Num() > MAX_STATIC_MESH_LODS)
            return InputFailure(OutError, Value, TEXT("static_mesh_build_settings_array_required"),
                TEXT("The number of build-settings entries exceeds the native static mesh LOD limit."), TEXT("/build_settings"),
                MakeShared<FJsonValueNumber>(Build->AsArray().Num()), MakeShared<FJsonValueNumber>(MAX_STATIC_MESH_LODS));
        for (int32 Index = 0; Index < Build->AsArray().Num(); ++Index)
        {
            const auto& Item = Build->AsArray()[Index];
            if (!Item || Item->Type != EJson::Object)
                return InputFailure(OutError, Value, TEXT("static_mesh_lod_properties_required"),
                    TEXT("Each LOD build-settings entry must be an object."),
                    TEXT("/build_settings/") + FString::FromInt(Index), Item, MakeShared<FJsonValueString>(TEXT("an object")));
        }
    }
    return true;
}

bool Read(UStaticMesh& Mesh, const FString& SemanticAssetPath,
    TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
{
    auto Result = MakeShared<FJsonObject>();
    UeAgentDetailsAuthoringService::FCapturePolicy Policy;
    UeAgentAuthoringState::FStateSnapshot Snapshot;
    TSharedPtr<FJsonObject> Document;
    if (const auto* Body = Mesh.GetBodySetup())
    {
        Policy.IncludeProperty = [](const FProperty* Property, const void*, const UObject*) { return IsCollisionProperty(Property); };
        if (!UeAgentDetailsAuthoringService::CaptureObjectProperties(Body, Policy, Snapshot, Document, OutError)
            || !ProjectSettings(Snapshot, Document, OutError)) return false;
        Result->SetObjectField(TEXT("collision"), Document);
    }
    Policy.IncludeProperty = {};
    TArray<TSharedPtr<FJsonValue>> Build;
    for (int32 Lod = 0; Lod < Mesh.GetNumSourceModels(); ++Lod)
    {
        if (!UeAgentDetailsAuthoringService::CaptureStructProperties(FMeshBuildSettings::StaticStruct(),
            &Mesh.GetSourceModel(Lod).BuildSettings, SemanticAssetPath, &Mesh, Policy, Snapshot, Document, OutError)
            || !ProjectSettings(Snapshot, Document, OutError)) return false;
        Build.Add(MakeShared<FJsonValueObject>(Document));
    }
    Result->SetArrayField(TEXT("build_settings"), Build);
    TSharedPtr<FJsonValue> Sockets;
    if (!ReadSockets(Mesh, Sockets, OutError)) return false;
    Result->SetField(TEXT("sockets"), Sockets);
    OutValue = MakeShared<FJsonValueObject>(Result);
    return true;
}

bool Apply(UStaticMesh& Mesh, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError)
{
    if (!Validate(Value, OutError)) return false;
    using namespace UeAgentAuthoringObjectApply;
    FOptions Options;
    // The containing mesh owns notification/build, and the Reflection operation
    // owns its full checkpoint. Bind all child edits before touching native data.
    Options.bEmitEditorNotifications = false;
    Options.bMarkPackageDirty = false;
    Options.bAllowDomainAuthorizedOwners = true;
    Options.bCompactValues = true;
    Options.DiagnosticTargetPath = Mesh.GetPathName();
    TArray<FSettingsPlan> Plans;
    TArray<UStaticMeshSocket*> NewSockets;
    TArray<TObjectPtr<UStaticMeshSocket>> DesiredSockets;
    bool SocketsPublished = false;
    ON_SCOPE_EXIT { if (!SocketsPublished) for (auto* Socket : NewSockets) Socket->MarkAsGarbage(); };
    FFailure Failure;
    const auto Collision = Value->AsObject()->TryGetField(TEXT("collision"));
    if (Collision)
    {
        auto* Body = Mesh.GetBodySetup();
        if (!Body)
        {
            OutError = ValueFailure(TEXT("static_mesh_collision_owner_missing"),
                TEXT("The static mesh has no native body setup to receive collision settings."),
                TEXT("BodySetup"), MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("a valid body setup")));
            OutError.SourceKind = TEXT("native_result");
            OutError.TargetPath = Mesh.GetPathName();
            ProfileState::AddRelatedInput(OutError, Value, TEXT("/collision"));
            return false;
        }
        TArray<TSharedPtr<FJsonValue>> Descriptors;
        UeAgentAuthoringState::FStateSnapshot Snapshot;
        TSharedPtr<FJsonObject> Ignored;
        UeAgentDetailsAuthoringService::FCapturePolicy Policy;
        Policy.IncludeProperty = [](const FProperty* Property, const void*, const UObject*) { return IsCollisionProperty(Property); };
        if (!UeAgentDetailsAuthoringService::CaptureObjectProperties(Body, Policy, Snapshot, Ignored, OutError)) return false;
        if (!UeAgentDetailsAuthoringService::DecodeReadableProperties(Snapshot, Collision, Descriptors, OutError))
        {
            ProfileState::PrefixInputLocation(OutError, TEXT("/collision"));
            ProfileState::BindInput(OutError, Value);
            return false;
        }
        auto CollisionOptions = Options;
        CollisionOptions.AuthorizeArrayResize = UE::UAI::DomainAdapters::RigSimulationSpatial::AuthorizesSimpleCollisionShapeResize;
        CollisionOptions.IncludeRootProperty = IsCollisionProperty;
        auto& Plan = Plans.AddDefaulted_GetRef();
        Plan.Settings = Collision; Plan.Pointer = TEXT("/collision");
        if (!PrepareObject(Body, Descriptors, CollisionOptions, Plan.Apply, Failure))
        {
            OutError = Failure.Diagnostic;
            ProfileState::LocateSettingsFailure(OutError, Plan.Settings, Plan.Pointer, Value);
            return false;
        }
    }
    if (const auto Build = Value->AsObject()->TryGetField(TEXT("build_settings")))
    {
        if (Build->AsArray().Num() != Mesh.GetNumSourceModels())
            return InputFailure(OutError, Value, TEXT("static_mesh_existing_lod_count_required"),
                TEXT("Build settings must contain exactly one entry for each existing source-model LOD."),
                TEXT("/build_settings"), MakeShared<FJsonValueNumber>(Build->AsArray().Num()),
                MakeShared<FJsonValueNumber>(Mesh.GetNumSourceModels()));
        for (int32 Lod = 0; Lod < Build->AsArray().Num(); ++Lod)
        {
            TArray<TSharedPtr<FJsonValue>> Descriptors;
            UeAgentAuthoringState::FStateSnapshot Snapshot;
            TSharedPtr<FJsonObject> Ignored;
            if (!UeAgentDetailsAuthoringService::CaptureStructProperties(FMeshBuildSettings::StaticStruct(),
                &Mesh.GetSourceModel(Lod).BuildSettings, Mesh.GetPathName(), &Mesh, {}, Snapshot, Ignored, OutError)) return false;
            const FString Pointer = TEXT("/build_settings/") + FString::FromInt(Lod);
            if (!UeAgentDetailsAuthoringService::DecodeReadableProperties(Snapshot, Build->AsArray()[Lod], Descriptors, OutError))
            {
                ProfileState::PrefixInputLocation(OutError, Pointer);
                ProfileState::BindInput(OutError, Value);
                return false;
            }
            auto& Plan = Plans.AddDefaulted_GetRef();
            Plan.Settings = Build->AsArray()[Lod]; Plan.Pointer = Pointer;
            if (!PrepareStruct(FMeshBuildSettings::StaticStruct(), &Mesh.GetSourceModel(Lod).BuildSettings,
                &Mesh, Descriptors, Options, Plan.Apply, Failure))
            {
                OutError = Failure.Diagnostic;
                ProfileState::LocateSettingsFailure(OutError, Plan.Settings, Plan.Pointer, Value);
                return false;
            }
        }
    }
    const auto Sockets = Value->AsObject()->TryGetField(TEXT("sockets"));
    if (Sockets)
    {
        TSharedPtr<FJsonValue> Resolved;
        if (!ResolveSockets(Mesh, Sockets, Value, Resolved, OutError)) return false;
        TMap<FName, UStaticMeshSocket*> Existing;
        for (UStaticMeshSocket* Socket : Mesh.Sockets) Existing.Add(Socket->SocketName, Socket);
        // Preserve the order and UObject identity of every surviving native item.
        TSet<FName> DesiredNames;
        for (const auto& Item : Resolved->AsObject()->Values) DesiredNames.Add(FName(*Item.Key));
        for (UStaticMeshSocket* Socket : Mesh.Sockets) if (DesiredNames.Contains(Socket->SocketName)) DesiredSockets.Add(Socket);
        TArray<FString> Names; Resolved->AsObject()->Values.GetKeys(Names); Names.Sort();
        for (const FString& Name : Names)
        {
            const auto Record = Resolved->AsObject()->GetObjectField(Name);
            UStaticMeshSocket* Socket = Existing.FindRef(FName(*Name));
            if (!Socket)
            {
                Socket = NewObject<UStaticMeshSocket>(&Mesh, NAME_None, RF_Transactional);
                Socket->SocketName = FName(*Name);
                NewSockets.Add(Socket); DesiredSockets.Add(Socket);
            }
            if (const auto Settings = Record->TryGetField(TEXT("settings")))
            {
                TArray<TSharedPtr<FJsonValue>> Descriptors;
                UeAgentAuthoringState::FStateSnapshot Snapshot;
                TSharedPtr<FJsonObject> Ignored;
                if (!UeAgentDetailsAuthoringService::CaptureObjectProperties(Socket, {}, Snapshot, Ignored, OutError)) return false;
                const FString Pointer = TEXT("/sockets/")
                    + Name.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")) + TEXT("/settings");
                if (!UeAgentDetailsAuthoringService::DecodeReadableProperties(Snapshot, Settings, Descriptors, OutError))
                {
                    ProfileState::PrefixInputLocation(OutError, Pointer);
                    ProfileState::BindInput(OutError, Value);
                    return false;
                }
                auto& Plan = Plans.AddDefaulted_GetRef();
                Plan.Settings = Settings; Plan.Pointer = Pointer;
                if (!PrepareObject(Socket, Descriptors, Options, Plan.Apply, Failure))
                {
                    OutError = Failure.Diagnostic;
                    ProfileState::LocateSettingsFailure(OutError, Plan.Settings, Plan.Pointer, Value);
                    return false;
                }
            }
        }
    }
    for (auto& Plan : Plans)
    {
        FApplyResult Applied;
        if (!ApplyPrepared(Plan.Apply, Applied))
        {
            OutError = Applied.Diagnostic;
            ProfileState::LocateSettingsFailure(OutError, Plan.Settings, Plan.Pointer, Value);
            return false;
        }
    }
    if (Sockets) { Mesh.Modify(); Mesh.Sockets = MoveTemp(DesiredSockets); SocketsPublished = true; }
    if (Collision) Mesh.GetBodySetup()->InvalidatePhysicsData();
    Mesh.PostEditChange();
    FStaticMeshCompilingManager::Get().FinishCompilation({&Mesh});
    return true;
}
}
