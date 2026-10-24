#include "Domains/Reflection/Authoring/UeAgentPhysicsAssetAuthoringState.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileState.h"

#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialPhysicsNative.h"
#include "Misc/ScopeExit.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"

namespace UeAgentReflectionAuthoring::PhysicsAssetState
{
namespace
{
using namespace UeAgentAuthoringObjectApply;
using namespace UeAgentDetailsAuthoringService;

using FDiagnostic = UE::UAI::Contract::FDiagnostic;
using ProfileState::BindInput;
using ProfileState::MarkNative;
using ProfileState::PrefixInputLocation;
using ProfileState::AddRelatedInput;
using ProfileState::LocateSettingsFailure;

bool Fail(FDiagnostic& Error, FDiagnostic Diagnostic) { Error = MoveTemp(Diagnostic); return false; }

FString Token(const FString& Name)
{
    return Name.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
}

FDiagnostic ValueFailure(const FString& Code, const FString& Reason, const FString& Path,
    const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
{
    auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Path, Expected, Actual,
        TEXT("physics_asset_authoring"), Reason);
    Error.Source.InternalJsonPath = Path;
    return Error;
}


TSharedPtr<FJsonValue> NameValues(const TArray<FName>& Names)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    for (FName Name : Names) Values.Add(MakeShared<FJsonValueString>(Name.ToString()));
    return MakeShared<FJsonValueArray>(Values);
}

FDiagnostic NativeIdentityFailure(const FString& Code, const FString& Reason, UPhysicsAsset& Asset,
    const FString& Path, const TSharedPtr<FJsonValue>& Actual)
{
    auto Error = ValueFailure(Code, Reason, Path, Actual,
        MakeShared<FJsonValueString>(TEXT("a non-null object with a unique non-None FName identity")));
    Error.SourceKind = TEXT("native_result");
    Error.TargetPath = Asset.GetPathName();
    return Error;
}

FCapturePolicy BodyPolicy()
{
    FCapturePolicy Policy;
    // This is a PhAT selection proxy, not the stored per-body profile data.
    Policy.ExcludedProperties = {TEXT("CurrentPhysicalAnimationProfile")};
    return Policy;
}

bool ObjectSettings(UObject& Object, const FCapturePolicy& Policy,
    UeAgentAuthoringState::FStateSnapshot& Snapshot, TSharedPtr<FJsonObject>& Categories, UE::UAI::Contract::FDiagnostic& Error)
{
    return CaptureObjectProperties(&Object, Policy, Snapshot, Categories, Error)
        && ProjectReadableProperties(Snapshot, Categories, Error);
}

bool StructSettings(UScriptStruct* Type, const void* Data, UObject& Owner,
    UeAgentAuthoringState::FStateSnapshot& Snapshot, TSharedPtr<FJsonObject>& Categories, UE::UAI::Contract::FDiagnostic& Error)
{
    return CaptureStructProperties(Type, Data, Owner.GetPathName(), &Owner, {}, Snapshot, Categories, Error)
        && ProjectReadableProperties(Snapshot, Categories, Error);
}

TSharedPtr<FJsonObject> Record(const TSharedPtr<FJsonObject>& Settings)
{
    auto Result = MakeShared<FJsonObject>();
    Result->SetObjectField(TEXT("settings"), Settings);
    return Result;
}

FConstraintInstance DefaultConstraint(const UPhysicsConstraintTemplate& Constraint)
{
    FConstraintInstance Result = Constraint.DefaultInstance;
    // Serialize saves DefaultProfile, not the currently selected PhAT profile.
    Result.ProfileInstance = Constraint.GetConstraintProfilePropertiesOrDefault(NAME_None);
    return Result;
}

bool ReadBodies(UPhysicsAsset& Asset, TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error)
{
    auto Result = MakeShared<FJsonObject>();
    TSet<FName> Names;
    for (int32 Index = 0; Index < Asset.SkeletalBodySetups.Num(); ++Index)
    {
        USkeletalBodySetup* Body = Asset.SkeletalBodySetups[Index];
        if (!Body || Body->BoneName.IsNone() || Names.Contains(Body->BoneName))
            return Fail(Error, NativeIdentityFailure(TEXT("physics_body_identity_invalid"),
                !Body ? TEXT("The native physics body slot is null.")
                    : Body->BoneName.IsNone() ? TEXT("The native physics body has a None bone name.")
                    : TEXT("The native physics body duplicates an earlier body's FName identity."),
                Asset, FString::Printf(TEXT("SkeletalBodySetups[%d].BoneName"), Index),
Body ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Body->BoneName.ToString()))
                    : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>())));
        Names.Add(Body->BoneName);
        UeAgentAuthoringState::FStateSnapshot Snapshot;
        TSharedPtr<FJsonObject> Categories;
        if (!ObjectSettings(*Body, BodyPolicy(), Snapshot, Categories, Error)) return false;
        auto Item = Record(Categories);
        auto Profiles = MakeShared<FJsonObject>();
        for (const auto& Profile : Body->GetPhysicalAnimationProfiles())
        {
            if (!StructSettings(FPhysicalAnimationData::StaticStruct(), &Profile.PhysicalAnimationData,
                *Body, Snapshot, Categories, Error)) return false;
            Profiles->SetObjectField(Profile.ProfileName.ToString(), Record(Categories));
        }
        UeAgentAuthoringEncodedTypes::FScope::RecordNameKeyedObject(Profiles);
        Item->SetObjectField(TEXT("profiles"), Profiles);
        Result->SetObjectField(Body->BoneName.ToString(), Item);
    }
    UeAgentAuthoringEncodedTypes::FScope::RecordNameKeyedObject(Result);
    Value = MakeShared<FJsonValueObject>(Result);
    return true;
}

bool ReadConstraints(UPhysicsAsset& Asset, TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error)
{
    auto Result = MakeShared<FJsonObject>();
    TSet<FName> Names;
    for (int32 Index = 0; Index < Asset.ConstraintSetup.Num(); ++Index)
    {
        UPhysicsConstraintTemplate* Constraint = Asset.ConstraintSetup[Index];
        if (!Constraint || Constraint->DefaultInstance.JointName.IsNone()
            || Names.Contains(Constraint->DefaultInstance.JointName))
            return Fail(Error, NativeIdentityFailure(TEXT("physics_constraint_identity_invalid"),
                !Constraint ? TEXT("The native constraint slot is null.")
                    : Constraint->DefaultInstance.JointName.IsNone() ? TEXT("The native constraint has a None joint name.")
                    : TEXT("The native constraint duplicates an earlier constraint's FName identity."),
                Asset, FString::Printf(TEXT("ConstraintSetup[%d].JointName"), Index),
Constraint ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Constraint->DefaultInstance.JointName.ToString()))
                    : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>())));
        Names.Add(Constraint->DefaultInstance.JointName);
        UeAgentAuthoringState::FStateSnapshot Snapshot;
        TSharedPtr<FJsonObject> Categories;
        const auto Default = DefaultConstraint(*Constraint);
        if (!StructSettings(FConstraintInstance::StaticStruct(), &Default, *Constraint, Snapshot, Categories, Error)) return false;
        auto Item = Record(Categories);
        auto Profiles = MakeShared<FJsonObject>();
        for (const auto& Profile : Constraint->ProfileHandles)
        {
            if (!StructSettings(FConstraintProfileProperties::StaticStruct(), &Profile.ProfileProperties,
                *Constraint, Snapshot, Categories, Error)) return false;
            Profiles->SetObjectField(Profile.ProfileName.ToString(), Record(Categories));
        }
        UeAgentAuthoringEncodedTypes::FScope::RecordNameKeyedObject(Profiles);
        Item->SetObjectField(TEXT("profiles"), Profiles);
        Result->SetObjectField(Constraint->DefaultInstance.JointName.ToString(), Item);
    }
    UeAgentAuthoringEncodedTypes::FScope::RecordNameKeyedObject(Result);
    Value = MakeShared<FJsonValueObject>(Result);
    return true;
}

const UE::UAI::HjsonWorkspace::FKeyedItemProjectionContract& ProfileContract()
{
    static const auto Contract = [] {
        UE::UAI::HjsonWorkspace::FKeyedItemProjectionContract C;
        C.RootField = TEXT("profiles"); C.bIdentityFromMapKey = true;
        C.bMapKeyUsesNameIdentity = true; C.bRequiresCreationCandidate = false;
        C.VisibleFieldPolicy.VisibleTopLevelFields = {TEXT("settings")};
        return C;
    }();
    return Contract;
}

bool Resolve(const TSharedPtr<FJsonValue>& Desired, const TSharedPtr<FJsonValue>& Captured,
    const FString& Name, const TSharedPtr<FJsonValue>& Input, TSharedPtr<FJsonValue>& Resolved, FDiagnostic& Error)
{
    using namespace UE::UAI;
    auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
    StateSemantic::FUeAgentCanonicalValuePtr Before, Proposed, After, NewRecord;
    auto NativeDefaults = MakeShared<FJsonObject>();
    NativeDefaults->SetObjectField(TEXT("profiles"), MakeShared<FJsonObject>());
    if (!StateSemantic::CanonicalJson::TryFromJsonValue(Captured, Resources, Before, Error))
    { MarkNative(Error); return false; }
    if (!StateSemantic::CanonicalJson::TryFromJsonValue(Desired, Resources, Proposed, Error))
    {
        PrefixInputLocation(Error, TEXT("/") + Token(Name));
        BindInput(Error, Input);
        return false;
    }
    if (!StateSemantic::CanonicalJson::TryFromJsonValue(MakeShared<FJsonValueObject>(NativeDefaults), Resources, NewRecord, Error))
    { MarkNative(Error); return false; }
    HjsonWorkspace::FKeyedItemProjectionContract Contract;
    Contract.RootField = Name; Contract.bIdentityFromMapKey = true;
    Contract.bMapKeyUsesNameIdentity = true; Contract.bRequiresCreationCandidate = false;
    Contract.VisibleFieldPolicy.VisibleTopLevelFields = {TEXT("settings"), TEXT("profiles")};
    Contract.CollectionFieldContracts.Add(TEXT("profiles"), &ProfileContract());
    HjsonWorkspace::FWorkspaceError Failure;
    bool Changed = false;
    // New native bodies/constraints start with no named profiles. Supply this
    // existing resolver's record schema; an existing malformed capture still fails.
    if (!HjsonWorkspace::TryResolveKeyedItemDesiredState(*Proposed, *Before, Name, Contract,
        NewRecord.Get(), Resources, After, Changed, Failure))
    {
        Error = Failure.GetDiagnostic();
        PrefixInputLocation(Error, TEXT("/") + HjsonWorkspace::EscapeJsonPointerSegment(Name));
        if (Input->AsObject()->HasField(Name))
            UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Input->AsObject(), Name);
        else MarkNative(Error);
        return false;
    }
    if (!StateSemantic::CanonicalJson::TryToJsonValue(*After, Resolved, Error))
    { MarkNative(Error); return false; }
    return true;
}

FOptions ChildOptions()
{
    FOptions Options;
    Options.bCompactValues = true; Options.bEmitEditorNotifications = false;
    Options.bMarkPackageDirty = false; Options.bAllowDomainAuthorizedOwners = true;
    Options.AuthorizeArrayResize = UE::UAI::DomainAdapters::RigSimulationSpatial::AuthorizesSimpleCollisionShapeResize;
    return Options;
}

struct FSettingsPlan
{
    FPreparedApply Apply;
    TSharedPtr<FJsonValue> Settings;
    FString InputPointer;
};


bool PrepareSettings(UObject& Owner, UScriptStruct* Type, void* Data,
    const TSharedPtr<FJsonValue>& Settings, const FString& Pointer, const TSharedPtr<FJsonValue>& Input,
    TArray<FSettingsPlan>& Plans, FDiagnostic& Error)
{
    if (!Settings) return true;
    UeAgentAuthoringState::FStateSnapshot Snapshot;
    TSharedPtr<FJsonObject> Categories;
    if (Type ? !StructSettings(Type, Data, Owner, Snapshot, Categories, Error)
        : !ObjectSettings(Owner, BodyPolicy(), Snapshot, Categories, Error)) return false;
    TArray<TSharedPtr<FJsonValue>> Descriptors;
    if (!DecodeReadableProperties(Snapshot, Settings, Descriptors, Error))
    {
        PrefixInputLocation(Error, Pointer);
        BindInput(Error, Input);
        return false;
    }
    auto& Plan = Plans.AddDefaulted_GetRef();
    Plan.Settings = Settings;
    Plan.InputPointer = Pointer;
    FFailure Failure;
    const bool Prepared = Type
        ? PrepareStruct(Type, Data, &Owner, Descriptors, ChildOptions(), Plan.Apply, Failure)
        : PrepareObject(&Owner, Descriptors, ChildOptions(), Plan.Apply, Failure);
    if (!Prepared)
    {
        Error = Failure.Diagnostic;
        LocateSettingsFailure(Error, Settings, Pointer, Input);
        return false;
    }
    return true;
}

bool Names(const TSharedPtr<FJsonValue>& Value, const FString& Field, const TSharedPtr<FJsonValue>& Input,
    TArray<FName>& Result, FDiagnostic& Error)
{
    const FString Path = TEXT("/") + Token(Field);
    auto Reject = [&](const FString& Code, const FString& Reason, const FString& At,
        const TSharedPtr<FJsonValue>& Actual, const FString& Expected)
    {
        Error = ValueFailure(Code, Reason, At, Actual, MakeShared<FJsonValueString>(Expected));
        BindInput(Error, Input);
        return false;
    };
    if (!Value || Value->Type != EJson::Array)
        return Reject(TEXT("physics_profile_names_array_required"), TEXT("The profile-name list must be an array."),
            Path, Value, TEXT("an array of unique, non-None names"));
    for (int32 Index = 0; Index < Value->AsArray().Num(); ++Index)
    {
        const auto& Item = Value->AsArray()[Index];
        const FString ItemPath = Path + TEXT("/") + LexToString(Index);
        if (!Item || Item->Type != EJson::String)
            return Reject(TEXT("physics_profile_name_required"), TEXT("Each profile name must be a string."),
                ItemPath, Item, TEXT("a string"));
        const FName Name(*Item->AsString());
        if (Name.IsNone() || Result.Contains(Name))
            return Reject(TEXT("physics_profile_name_invalid"),
                Name.IsNone() ? TEXT("None is not a valid physics profile name.")
                    : TEXT("This profile name duplicates an earlier entry under FName comparison."),
                ItemPath, Item, TEXT("a unique, non-None FName"));
        Result.Add(Name);
    }
    return true;
}

struct FBodyPlan
{
    USkeletalBodySetup* Body = nullptr;
    TArray<FPhysicalAnimationProfile> Profiles;
};
struct FConstraintPlan
{
    UPhysicsConstraintTemplate* Constraint = nullptr;
    FName SourceName;
    FString InputKey;
    TSharedPtr<FJsonValue> Settings;
    FConstraintInstance Default;
    TOptional<FConstraintInstance> Before, Native;
    TArray<FPhysicsConstraintProfileHandle> Profiles;
};
}

TSharedPtr<FJsonObject> BuildValueDefinition()
{
    auto Fields = MakeShared<FJsonObject>();
    for (const TCHAR* Name : {TEXT("bodies"), TEXT("constraints")})
    {
        auto Item = MakeShared<FJsonObject>(), Contract = MakeShared<FJsonObject>();
        Contract->SetStringField(TEXT("collection_semantics"), TEXT("keyed_desired_state"));
        Item->SetStringField(TEXT("type"), TEXT("object")); Item->SetObjectField(TEXT("contract"), Contract);
        Fields->SetObjectField(Name, Item);
    }
    auto Name = MakeShared<FJsonObject>(); Name->SetStringField(TEXT("type"), TEXT("name"));
    Name->SetObjectField(TEXT("contract"), MakeShared<FJsonObject>());
    auto Names = MakeShared<FJsonObject>();
    Names->SetArrayField(TEXT("items"), TArray<TSharedPtr<FJsonValue>>{});
    Names->SetObjectField(TEXT("element_definition"), Name);
    Fields->SetObjectField(TEXT("physical_animation_profiles"), Names);
    Fields->SetObjectField(TEXT("constraint_profiles"), Names);
    auto Definition = MakeShared<FJsonObject>(); Definition->SetObjectField(TEXT("fields"), Fields);
    return Definition;
}

bool Validate(const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error)
{
    if (!Value || Value->Type != EJson::Object)
    {
        Error = ValueFailure(TEXT("physics_structure_object_required"), TEXT("Physics asset structure must be an object."),
            TEXT("/"), Value, MakeShared<FJsonValueString>(TEXT("an object")));
        Error.SourceKind = TEXT("authored_value");
        return false;
    }
    for (const auto& Field : Value->AsObject()->Values)
        if (Field.Key != TEXT("bodies") && Field.Key != TEXT("constraints")
            && Field.Key != TEXT("physical_animation_profiles") && Field.Key != TEXT("constraint_profiles"))
        {
            Error = ValueFailure(TEXT("physics_structure_unknown_field"),
                TEXT("This field is not supported in physics asset structure."), TEXT("/") + Token(Field.Key),
                Field.Value, MakeShared<FJsonValueString>(TEXT("bodies, constraints, physical_animation_profiles, or constraint_profiles")));
            BindInput(Error, Value);
            return false;
        }
    return true;
}

bool Read(UPhysicsAsset& Asset, TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error)
{
    auto Result = MakeShared<FJsonObject>();
    TSharedPtr<FJsonValue> Bodies, Constraints;
    if (!ReadBodies(Asset, Bodies, Error) || !ReadConstraints(Asset, Constraints, Error)) return false;
    Result->SetField(TEXT("bodies"), Bodies); Result->SetField(TEXT("constraints"), Constraints);
    auto EncodeNames = [](const TArray<FName>& Native) {
        TArray<TSharedPtr<FJsonValue>> Items;
        for (FName Name : Native) Items.Add(MakeShared<FJsonValueString>(Name.ToString()));
        return Items;
    };
    Result->SetArrayField(TEXT("physical_animation_profiles"), EncodeNames(Asset.PhysicalAnimationProfiles));
    Result->SetArrayField(TEXT("constraint_profiles"), EncodeNames(Asset.ConstraintProfiles));
    Value = MakeShared<FJsonValueObject>(Result); return true;
}

bool Apply(UPhysicsAsset& Asset, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error)
{
    if (!Validate(Value, Error)) return false;
    TSharedPtr<FJsonValue> Before;
    if (!Read(Asset, Before, Error)) return false;
    auto Reject = [&](const FString& Code, const FString& Reason, const FString& Path,
        const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
    {
        Error = ValueFailure(Code, Reason, Path, Actual, Expected);
        Error.TargetPath = Asset.GetPathName();
        BindInput(Error, Value);
        return false;
    };
    auto Get = [&](const TCHAR* Name) {
        const auto Proposed = Value->AsObject()->TryGetField(Name);
        return Proposed ? Proposed : Before->AsObject()->TryGetField(Name);
    };
    TArray<FName> PhysicalNames, ConstraintNames;
    if (!Names(Get(TEXT("physical_animation_profiles")), TEXT("physical_animation_profiles"), Value, PhysicalNames, Error)
        || !Names(Get(TEXT("constraint_profiles")), TEXT("constraint_profiles"), Value, ConstraintNames, Error)) return false;
    const auto IsUnchangedProfile = [&](const TCHAR* Collection, const FString& ItemName,
        const FString& ProfileName, const TSharedPtr<FJsonValue>& Desired) {
        const auto Item = Before->AsObject()->GetObjectField(Collection)->TryGetField(ItemName);
        if (!Item || Item->Type != EJson::Object) return false;
        const auto Profiles = Item->AsObject()->TryGetField(TEXT("profiles"));
        const auto Old = Profiles && Profiles->Type == EJson::Object
            ? Profiles->AsObject()->TryGetField(ProfileName) : nullptr;
        return Old && UeAgentAuthoringState::MakeStableCanonicalValueKey(Old)
            == UeAgentAuthoringState::MakeStableCanonicalValueKey(Desired);
    };
    TSharedPtr<FJsonValue> Bodies, Constraints;
    if (!Resolve(Get(TEXT("bodies")), Before->AsObject()->TryGetField(TEXT("bodies")), TEXT("bodies"), Value, Bodies, Error)
        || !Resolve(Get(TEXT("constraints")), Before->AsObject()->TryGetField(TEXT("constraints")), TEXT("constraints"), Value, Constraints, Error)) return false;

    TMap<USkeletalBodySetup*, int32> BeforeEditorFlags;
    TMap<FName, FConstraintInstance> BeforeConstraints;
    TMap<FName, TArray<FPhysicsConstraintProfileHandle>> BeforeProfiles;
    for (const UPhysicsConstraintTemplate* Constraint : Asset.ConstraintSetup)
    {
        BeforeConstraints.Add(Constraint->DefaultInstance.JointName, DefaultConstraint(*Constraint));
        BeforeProfiles.Add(Constraint->DefaultInstance.JointName, Constraint->ProfileHandles);
    }
    for (int32 Index = 0; Index < Asset.SkeletalBodySetups.Num(); ++Index)
        BeforeEditorFlags.Add(Asset.SkeletalBodySetups[Index], Asset.EditorBodyFlags.IsValidIndex(Index) ? Asset.EditorBodyFlags[Index] : 0);
    bool RemovedBody = false;
    TMap<FName, FName> ReconnectedNames;
    for (int32 Index = Asset.SkeletalBodySetups.Num() - 1; Index >= 0; --Index)
        if (!Bodies->AsObject()->HasField(Asset.SkeletalBodySetups[Index]->BoneName.ToString()))
        {
            if (!UE::UAI::DomainAdapters::RigSimulationSpatial::DeletePhysicsAssetBody(Asset, Index, Error, &ReconnectedNames)) return false;
            RemovedBody = true;
        }
    if (RemovedBody)
    {
        TSharedPtr<FJsonValue> NativeConstraints;
        if (!ReadConstraints(Asset, NativeConstraints, Error)) return false;
        const auto OldConstraints = Before->AsObject()->GetObjectField(TEXT("constraints"));
        const auto Native = NativeConstraints->AsObject();
        TSet<FString> Represented, ExplicitlyRemoved;
        for (const auto& Pair : ReconnectedNames)
            if (!Constraints->AsObject()->HasField(Pair.Key.ToString())) ExplicitlyRemoved.Add(Pair.Value.ToString());
        // An untouched relation follows PhAT. An explicit new relation remains
        // authored intent and is validated against the surviving bodies below.
        for (auto It = Constraints->AsObject()->Values.CreateIterator(); It; ++It)
        {
            const FName* Reconnected = ReconnectedNames.Find(FName(*It.Key()));
            const FString NativeName = Reconnected ? Reconnected->ToString() : It.Key();
            if (Native->HasField(NativeName)) Represented.Add(NativeName);
            else if (const auto Old = OldConstraints->TryGetField(It.Key()); Old.IsValid()
                && UeAgentAuthoringState::MakeStableCanonicalValueKey(Old) == UeAgentAuthoringState::MakeStableCanonicalValueKey(It.Value()))
            {
                It.RemoveCurrent();
            }
        }
        for (const auto& Row : Native->Values)
            if (!OldConstraints->HasField(Row.Key) && !Represented.Contains(Row.Key) && !ExplicitlyRemoved.Contains(Row.Key))
                Constraints->AsObject()->SetField(Row.Key, Row.Value);
    }

    TArray<FSettingsPlan> Plans;
    TArray<TUniquePtr<FBodyPlan>> BodyPlans;
    TArray<TUniquePtr<FConstraintPlan>> ConstraintPlans;
    TArray<UObject*> NewObjects;
    bool Published = false;
    ON_SCOPE_EXIT { if (!Published) for (auto* Object : NewObjects) Object->MarkAsGarbage(); };
    TMap<FName, USkeletalBodySetup*> ExistingBodies;
    TMap<FName, UPhysicsConstraintTemplate*> ExistingConstraints;
    for (USkeletalBodySetup* Body : Asset.SkeletalBodySetups) ExistingBodies.Add(Body->BoneName, Body);
    for (UPhysicsConstraintTemplate* Constraint : Asset.ConstraintSetup) ExistingConstraints.Add(Constraint->DefaultInstance.JointName, Constraint);
    for (const auto& Item : Bodies->AsObject()->Values)
    {
        auto Plan = MakeUnique<FBodyPlan>(); const FName Name(*Item.Key);
        Plan->Body = ExistingBodies.FindRef(Name);
        if (!Plan->Body) { Plan->Body = NewObject<USkeletalBodySetup>(&Asset, NAME_None, RF_Transactional); Plan->Body->BoneName = Name; NewObjects.Add(Plan->Body); }
        const auto Data = Item.Value->AsObject();
        if (!PrepareSettings(*Plan->Body, nullptr, nullptr, Data->TryGetField(TEXT("settings")),
            TEXT("/bodies/") + Token(Item.Key) + TEXT("/settings"), Value, Plans, Error)) return false;
        const auto Profiles = Data->TryGetField(TEXT("profiles"));
        if (Profiles)
        {
            if (Profiles->Type != EJson::Object)
                return Reject(TEXT("physics_profiles_object_required"), TEXT("Body profiles must be an object keyed by profile name."),
                    TEXT("/bodies/") + Token(Item.Key) + TEXT("/profiles"), Profiles, MakeShared<FJsonValueString>(TEXT("an object")));
            Plan->Profiles.Reserve(Profiles->AsObject()->Values.Num());
            for (const auto& Profile : Profiles->AsObject()->Values)
            {
                const FName ProfileName(*Profile.Key);
                if (!PhysicalNames.Contains(ProfileName))
                {
                    if (Asset.PhysicalAnimationProfiles.Contains(ProfileName)
                        && IsUnchangedProfile(TEXT("bodies"), Item.Key, Profile.Key, Profile.Value)) continue;
                    return Reject(TEXT("physics_body_profile_undeclared"),
                        TEXT("The body profile name is not declared in physical_animation_profiles."),
                        TEXT("/bodies/") + Token(Item.Key) + TEXT("/profiles/") + Token(Profile.Key),
                        MakeShared<FJsonValueString>(Profile.Key), NameValues(PhysicalNames));
                }
                auto& Target = Plan->Profiles.AddDefaulted_GetRef();
                if (const auto* Existing = Plan->Body->FindPhysicalAnimationProfile(ProfileName)) Target = *Existing;
                Target.ProfileName = ProfileName;
                if (!PrepareSettings(*Plan->Body, FPhysicalAnimationData::StaticStruct(), &Target.PhysicalAnimationData,
                    Profile.Value->AsObject()->TryGetField(TEXT("settings")),
                    TEXT("/bodies/") + Token(Item.Key) + TEXT("/profiles/") + Token(Profile.Key) + TEXT("/settings"),
                    Value, Plans, Error)) return false;
            }
        }
        BodyPlans.Add(MoveTemp(Plan));
    }
    for (const auto& Item : Constraints->AsObject()->Values)
    {
        auto Plan = MakeUnique<FConstraintPlan>(); const FName Name(*Item.Key);
        Plan->SourceName = Name;
        Plan->InputKey = Item.Key;
        const FName* Reconnected = ReconnectedNames.Find(Name);
        Plan->Constraint = ExistingConstraints.FindRef(Reconnected ? *Reconnected : Name);
        if (!Plan->Constraint) { Plan->Constraint = NewObject<UPhysicsConstraintTemplate>(&Asset, NAME_None, RF_Transactional); Plan->Constraint->DefaultInstance.JointName = Name; NewObjects.Add(Plan->Constraint); }
        Plan->Default = DefaultConstraint(*Plan->Constraint);
        Plan->Before = Plan->Default; Plan->Native = Plan->Default;
        if (RemovedBody && ExistingConstraints.Contains(Plan->Constraint->DefaultInstance.JointName))
            if (const auto* Old = BeforeConstraints.Find(Name))
            {
                // Decode against the original intent baseline, then overlay only
                // explicit reflected changes onto PhAT's reconnected constraint.
                Plan->Before = *Old; Plan->Native = Plan->Default; Plan->Default = *Old;
            }
        const auto Data = Item.Value->AsObject();
        Plan->Settings = Data->TryGetField(TEXT("settings"));
        if (!PrepareSettings(*Plan->Constraint, FConstraintInstance::StaticStruct(), &Plan->Default,
            Plan->Settings, TEXT("/constraints/") + Token(Item.Key) + TEXT("/settings"), Value, Plans, Error)) return false;
        const auto Profiles = Data->TryGetField(TEXT("profiles"));
        if (Profiles)
        {
            if (Profiles->Type != EJson::Object)
                return Reject(TEXT("physics_profiles_object_required"), TEXT("Constraint profiles must be an object keyed by profile name."),
                    TEXT("/constraints/") + Token(Item.Key) + TEXT("/profiles"), Profiles, MakeShared<FJsonValueString>(TEXT("an object")));
            Plan->Profiles.Reserve(Profiles->AsObject()->Values.Num());
            for (const auto& Profile : Profiles->AsObject()->Values)
            {
                const FName ProfileName(*Profile.Key);
                if (!ConstraintNames.Contains(ProfileName))
                {
                    if (Asset.ConstraintProfiles.Contains(ProfileName)
                        && IsUnchangedProfile(TEXT("constraints"), Item.Key, Profile.Key, Profile.Value)) continue;
                    return Reject(TEXT("physics_constraint_profile_undeclared"),
                        TEXT("The constraint profile name is not declared in constraint_profiles."),
                        TEXT("/constraints/") + Token(Item.Key) + TEXT("/profiles/") + Token(Profile.Key),
                        MakeShared<FJsonValueString>(Profile.Key), NameValues(ConstraintNames));
                }
                auto& Target = Plan->Profiles.AddDefaulted_GetRef(); Target.ProfileName = ProfileName;
                Target.ProfileProperties = Plan->Constraint->GetConstraintProfilePropertiesOrDefault(ProfileName);
                if (const auto* ProfilesBefore = BeforeProfiles.Find(Name))
                    if (const auto* Old = ProfilesBefore->FindByPredicate([&](const auto& P) { return P.ProfileName == ProfileName; }))
                        Target.ProfileProperties = Old->ProfileProperties;
                if (!PrepareSettings(*Plan->Constraint, FConstraintProfileProperties::StaticStruct(), &Target.ProfileProperties,
                    Profile.Value->AsObject()->TryGetField(TEXT("settings")),
                    TEXT("/constraints/") + Token(Item.Key) + TEXT("/profiles/") + Token(Profile.Key) + TEXT("/settings"),
                    Value, Plans, Error)) return false;
            }
        }
        ConstraintPlans.Add(MoveTemp(Plan));
    }
    for (auto& Plan : Plans)
    {
        FApplyResult Result;
        if (!ApplyPrepared(Plan.Apply, Result))
        {
            Error = Result.Diagnostic;
            LocateSettingsFailure(Error, Plan.Settings, Plan.InputPointer, Value);
            return false;
        }
    }
    TMap<UPhysicsConstraintTemplate*, TArray<FConstraintPlan*>> ConstraintGroups;
    for (auto& Plan : ConstraintPlans) ConstraintGroups.FindOrAdd(Plan->Constraint).Add(Plan.Get());
    for (const auto& Group : ConstraintGroups)
    {
        FConstraintInstance Merged = Group.Value[0]->Native.GetValue();
        TArray<FStructChange> Edits;
        for (const auto* Plan : Group.Value) Edits.Add({&Plan->Before.GetValue(), &Plan->Default});
        if (!TryApplyStructChanges(FConstraintInstance::StaticStruct(), Edits, &Merged, Group.Key, Error))
        {
            MarkNative(Error);
            for (const auto* Plan : Group.Value)
                AddRelatedInput(Error, Value, TEXT("/constraints/") + Token(Plan->InputKey) + TEXT("/settings"));
            return false;
        }

        TSet<FName> ProfileNames;
        for (const auto& P : Group.Key->ProfileHandles) ProfileNames.Add(P.ProfileName);
        for (const auto* Plan : Group.Value) {
            for (const auto& P : Plan->Profiles) ProfileNames.Add(P.ProfileName);
            if (const auto* Old = BeforeProfiles.Find(Plan->SourceName))
                for (const auto& P : *Old) ProfileNames.Add(P.ProfileName);
        }
        TArray<FPhysicsConstraintProfileHandle> MergedProfiles;
        for (FName ProfileName : ProfileNames) {
            if (!ConstraintNames.Contains(ProfileName)) continue;
            const auto FindProfile = [ProfileName](const TArray<FPhysicsConstraintProfileHandle>& Items) {
                return Items.FindByPredicate([&](const auto& P) { return P.ProfileName == ProfileName; });
            };
            const auto* NativeProfile = FindProfile(Group.Key->ProfileHandles);
            FConstraintProfileProperties Empty;
            FConstraintProfileProperties Current = NativeProfile ? NativeProfile->ProfileProperties : Empty;
            TArray<FStructChange> ProfileEdits;
            TArray<TSharedPtr<FJsonValue>> RemovedFrom, EditedBy;
            bool bRemoved = false, bChanged = false;
            for (const auto* Plan : Group.Value) {
                const auto* OldProfiles = BeforeProfiles.Find(Plan->SourceName);
                const auto* Old = OldProfiles ? FindProfile(*OldProfiles) : nullptr;
                const auto* Desired = FindProfile(Plan->Profiles);
                bRemoved |= Old && !Desired;
                if (Old && !Desired) RemovedFrom.Add(MakeShared<FJsonValueString>(Plan->InputKey));
                if (!Desired) continue;
                const bool bEdited = !Old || !FConstraintProfileProperties::StaticStruct()->CompareScriptStruct(
                    &Old->ProfileProperties, &Desired->ProfileProperties, 0);
                bChanged |= bEdited;
                if (bEdited) {
                    EditedBy.Add(MakeShared<FJsonValueString>(Plan->InputKey));
                    ProfileEdits.Add({Old ? &Old->ProfileProperties : &Empty, &Desired->ProfileProperties});
                }
            }
            if (bRemoved && bChanged)
            {
                auto Actual = MakeShared<FJsonObject>();
                Actual->SetStringField(TEXT("profile"), ProfileName.ToString());
                Actual->SetArrayField(TEXT("removed_from"), RemovedFrom);
                Actual->SetArrayField(TEXT("edited_by"), EditedBy);
                Error = ValueFailure(TEXT("physics_constraint_profile_edit_remove_conflict"),
                    TEXT("Constraints reconnected to the same native owner both remove and edit this profile."),
                    TEXT("profiles.") + ProfileName.ToString(), MakeShared<FJsonValueObject>(Actual),
                    MakeShared<FJsonValueString>(TEXT("consistent removal or editing of the shared profile")));
                Error.TargetPath = Group.Key->GetPathName();
                MarkNative(Error);
                for (const auto* Plan : Group.Value)
                    AddRelatedInput(Error, Value, TEXT("/constraints/") + Token(Plan->InputKey) + TEXT("/profiles"));
                return false;
            }
            if (bRemoved || (!NativeProfile && !bChanged)) continue;
            if (!TryApplyStructChanges(FConstraintProfileProperties::StaticStruct(), ProfileEdits, &Current, Group.Key, Error))
            {
                MarkNative(Error);
                for (const auto* Plan : Group.Value)
                    AddRelatedInput(Error, Value, TEXT("/constraints/") + Token(Plan->InputKey)
                        + TEXT("/profiles/") + Token(ProfileName.ToString()) + TEXT("/settings"));
                return false;
            }
            auto& Profile = MergedProfiles.AddDefaulted_GetRef(); Profile.ProfileName = ProfileName; Profile.ProfileProperties = Current;
        }
        for (auto* Plan : Group.Value) { Plan->Default = Merged; Plan->Profiles = MergedProfiles; }
    }
    TSet<FName> DesiredBodyNames;
    for (const auto& Plan : BodyPlans) DesiredBodyNames.Add(Plan->Body->BoneName);
    TArray<FName> SortedBodies = DesiredBodyNames.Array();
    SortedBodies.Sort(FNameLexicalLess());
    TArray<TSharedPtr<FJsonValue>> BodyChoices;
    for (const FName Name : SortedBodies) BodyChoices.Add(MakeShared<FJsonValueString>(Name.ToString()));
    UeAgentAuthoringDiagnostics::FDiagnostic MissingBodies;
    for (const auto& Plan : ConstraintPlans)
    {
        const auto CheckBody = [&](FName Body, const TCHAR* Field) {
            if (DesiredBodyNames.Contains(Body)) return;
            auto Missing = ValueFailure(TEXT("physics_constraint_body_missing"),
                TEXT("The constraint refers to a body that is absent from the desired physics asset."),
                Field, MakeShared<FJsonValueString>(Body.ToString()), MakeShared<FJsonValueArray>(BodyChoices));
            Missing.TargetPath = Plan->Constraint->GetPathName();
            LocateSettingsFailure(Missing, Plan->Settings,
                TEXT("/constraints/") + Token(Plan->InputKey) + TEXT("/settings"), Value);
            MissingBodies.Append(MoveTemp(Missing));
        };
        CheckBody(Plan->Default.ConstraintBone1, TEXT("ConstraintBone1"));
        CheckBody(Plan->Default.ConstraintBone2, TEXT("ConstraintBone2"));
    }
    if (!MissingBodies.Code.IsEmpty()) return Fail(Error, MissingBodies);

    // Preserve native order and identity for survivors, then append new members.
    TArray<TObjectPtr<USkeletalBodySetup>> DesiredBodies;
    TArray<TObjectPtr<UPhysicsConstraintTemplate>> DesiredConstraints;
    for (USkeletalBodySetup* Body : Asset.SkeletalBodySetups) if (DesiredBodyNames.Contains(Body->BoneName)) DesiredBodies.Add(Body);
    for (const auto& Plan : BodyPlans) DesiredBodies.AddUnique(Plan->Body);
    for (UPhysicsConstraintTemplate* Constraint : Asset.ConstraintSetup)
        if (ConstraintPlans.ContainsByPredicate([&](const auto& P) { return P->Constraint == Constraint; })) DesiredConstraints.Add(Constraint);
    for (const auto& Plan : ConstraintPlans) DesiredConstraints.AddUnique(Plan->Constraint);
    TMap<FRigidBodyIndexPair, bool> Disabled;
    for (const auto& Pair : Asset.CollisionDisableTable)
    {
        if (!Asset.SkeletalBodySetups.IsValidIndex(Pair.Key.Indices[0]) || !Asset.SkeletalBodySetups.IsValidIndex(Pair.Key.Indices[1])) continue;
        const int32 A = DesiredBodies.IndexOfByKey(Asset.SkeletalBodySetups[Pair.Key.Indices[0]]);
        const int32 B = DesiredBodies.IndexOfByKey(Asset.SkeletalBodySetups[Pair.Key.Indices[1]]);
        if (A != INDEX_NONE && B != INDEX_NONE) Disabled.Add(FRigidBodyIndexPair(A, B), Pair.Value);
    }
    TArray<int32> EditorFlags;
    for (const auto& Body : DesiredBodies)
    {
        EditorFlags.Add(BeforeEditorFlags.FindRef(Body));
    }
    Asset.Modify();
    Asset.SkeletalBodySetups = MoveTemp(DesiredBodies); Asset.ConstraintSetup = MoveTemp(DesiredConstraints);
    Asset.CollisionDisableTable = MoveTemp(Disabled); Asset.EditorBodyFlags = MoveTemp(EditorFlags);
    Asset.PhysicalAnimationProfiles = PhysicalNames; Asset.ConstraintProfiles = ConstraintNames;
    if (!PhysicalNames.Contains(Asset.CurrentPhysicalAnimationProfileName)) Asset.CurrentPhysicalAnimationProfileName = NAME_None;
    if (!ConstraintNames.Contains(Asset.CurrentConstraintProfileName)) Asset.CurrentConstraintProfileName = NAME_None;
    for (auto& Plan : BodyPlans)
    {
        TArray<FName> Included;
        for (const auto& Profile : Plan->Profiles) Included.Add(Profile.ProfileName);
        Plan->Body->UpdatePhysicalAnimationProfiles(Included);
        for (const auto& Profile : Plan->Profiles)
        {
            if (!Plan->Body->FindPhysicalAnimationProfile(Profile.ProfileName)) Plan->Body->AddPhysicalAnimationProfile(Profile.ProfileName);
            *Plan->Body->FindPhysicalAnimationProfile(Profile.ProfileName) = Profile;
        }
        const auto* Selected = Plan->Body->FindPhysicalAnimationProfile(Asset.CurrentPhysicalAnimationProfileName);
        Plan->Body->CurrentPhysicalAnimationProfile = Selected ? *Selected : FPhysicalAnimationProfile();
    }
    for (auto& Plan : ConstraintPlans)
    {
        Plan->Constraint->DefaultInstance = Plan->Default;
        Plan->Constraint->SetDefaultProfile(Plan->Default);
        Plan->Constraint->ProfileHandles = MoveTemp(Plan->Profiles);
        Plan->Constraint->ApplyConstraintProfile(Asset.CurrentConstraintProfileName, Plan->Constraint->DefaultInstance, true);
    }
    Published = true;
    Refresh(Asset);
    return true;
}

void Refresh(UPhysicsAsset& Asset)
{
    UE::UAI::DomainAdapters::RigSimulationSpatial::RefreshPhysicsAsset(Asset);
}
}
