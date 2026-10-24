#include "Domains/PCG/Authoring/UeAgentPcgDataAssetAuthoring.h"

#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Data/PCGPointData.h"
#include "Data/PCGPointArrayData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "PCGDataAsset.h"
#include "Metadata/PCGMetadata.h"
#include "Misc/ScopeExit.h"
#include "Serialization/ArchiveCrc32.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace UeAgentPcgDataAssetAuthoring
{
namespace
{
using namespace UE::UAI;

Contract::FDiagnostic InputFailure(const TCHAR* Code, const TCHAR* Reason, const FString& Field,
    const TSharedPtr<FJsonValue>& Actual, const FString& Expected, const FJsonObject* Input = nullptr)
{
    auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Field,
        MakeShared<FJsonValueString>(Expected), Actual, TEXT("pcg_data_apply"), Reason);
    Error.SourceKind = TEXT("authored_value");
    Error.Source.InternalJsonPath = Field.IsEmpty() ? TEXT("/") : TEXT("/") + Field;
    if (Input) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Input, Field);
    return Error;
}

void LocateFieldFailure(Contract::FDiagnostic& Error, const FJsonObject& Input, const TCHAR* Field)
{
    UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Error, FString(TEXT("/")) + Field);
    UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Input, Field);
}

void BindTarget(Contract::FDiagnostic& Error, const UPCGDataAsset& Asset, const FString& LogicalTarget)
{
    if (!Error.IsValid()) return;
    const FString NativeTarget = Asset.GetPathName();
    const FString Target = LogicalTarget.IsEmpty() ? NativeTarget : LogicalTarget;
    TFunction<void(Contract::FDiagnostic&)> Bind = [&](Contract::FDiagnostic& Issue)
    {
        if (Issue.TargetPath.IsEmpty()) Issue.TargetPath = Target;
        else if (Issue.TargetPath == NativeTarget || Issue.TargetPath.StartsWith(NativeTarget + TEXT("."))
            || Issue.TargetPath.StartsWith(NativeTarget + TEXT(":")))
        {
            if (Issue.TargetPath != Target)
            {
                if (!Issue.InternalContext.IsEmpty()) Issue.InternalContext += TEXT("; ");
                Issue.InternalContext += Issue.TargetPath;
                Issue.TargetPath = Target;
            }
        }
        for (auto& Child : Issue.Diagnostics) Bind(Child);
        if (Issue.PublicCause)
        {
            auto Cause = MakeShared<Contract::FDiagnostic>(*Issue.PublicCause);
            Bind(*Cause);
            Issue.PublicCause = MoveTemp(Cause);
        }
    };
    Bind(Error);
}

TArray<FPCGPoint> ReadPoints(const UPCGBasePointData& Data)
{
    TArray<FPCGPoint> Points;
    Points.SetNum(Data.GetNumPoints());
    for (int32 Index = 0; Index < Points.Num(); ++Index)
    {
        auto& Point = Points[Index];
        Point.Transform = Data.GetTransform(Index);
        Point.Density = Data.GetDensity(Index);
        Point.BoundsMin = Data.GetBoundsMin(Index);
        Point.BoundsMax = Data.GetBoundsMax(Index);
        Point.Color = Data.GetColor(Index);
        Point.Steepness = Data.GetSteepness(Index);
        Point.Seed = Data.GetSeed(Index);
        Point.MetadataEntry = Data.GetMetadataEntry(Index);
    }
    return Points;
}

template<typename T>
void WriteRange(TPCGValueRange<T> Range, const TArray<FPCGPoint>& Points, T FPCGPoint::* Field)
{
    for (int32 Index = 0; Index < Points.Num(); ++Index) Range[Index] = Points[Index].*Field;
}

void WritePoints(UPCGBasePointData& Data, const TArray<FPCGPoint>& Points)
{
    Data.SetNumPoints(Points.Num());
    WriteRange(Data.GetTransformValueRange(), Points, &FPCGPoint::Transform);
    WriteRange(Data.GetDensityValueRange(), Points, &FPCGPoint::Density);
    WriteRange(Data.GetBoundsMinValueRange(), Points, &FPCGPoint::BoundsMin);
    WriteRange(Data.GetBoundsMaxValueRange(), Points, &FPCGPoint::BoundsMax);
    WriteRange(Data.GetColorValueRange(), Points, &FPCGPoint::Color);
    WriteRange(Data.GetSteepnessValueRange(), Points, &FPCGPoint::Steepness);
    WriteRange(Data.GetSeedValueRange(), Points, &FPCGPoint::Seed);
    WriteRange(Data.GetMetadataEntryValueRange(), Points, &FPCGPoint::MetadataEntry);
    Data.Crc = FPCGCrc();
}

FString DataType(const UPCGData* Data)
{
    if (Cast<UPCGPointArrayData>(Data)) return TEXT("point_array");
    if (Cast<UPCGPointData>(Data)) return TEXT("point");
    return Data ? Data->GetClass()->GetName() : TEXT("null");
}

bool AddBinding(FJsonObject& Row, int32 Index, UE::UAI::Contract::FDiagnostic& Error)
{
    FString Key;
    HjsonWorkspace::FWorkspaceError Failure;
    if (!HjsonWorkspace::TryFormatReadableKeyedItemOrdinal(TEXT("Entry"), Index, Key, Failure))
    { Error = Failure.GetDiagnostic(); return false; }
    Row.SetStringField(TEXT("instance"), Key);
    return true;
}

bool BindRows(const TSharedPtr<FJsonValue>& Desired, int32 Count,
    TArray<HjsonWorkspace::FOrderedStructuredItemBinding>& Bindings, UE::UAI::Contract::FDiagnostic& Error)
{
    auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
    StateSemantic::FUeAgentCanonicalValuePtr Proposed;
    if (!StateSemantic::CanonicalJson::TryFromJsonValue(Desired, Resources, Proposed, Error)) return false;
    HjsonWorkspace::FWorkspaceError Failure;
    if (!HjsonWorkspace::TryResolveOrderedStructuredItems(*Proposed, Count, Resources, Bindings, Failure))
    { Error = Failure.GetDiagnostic(); return false; }
    return true;
}

bool ReadPoint(UPCGDataAsset& Asset, const FPCGPoint& Point, UeAgentAuthoringState::FStateSnapshot& Snapshot,
    TSharedPtr<FJsonObject>& Properties, UE::UAI::Contract::FDiagnostic& Error)
{
    return UeAgentDetailsAuthoringService::CaptureStructProperties(FPCGPoint::StaticStruct(), &Point,
        Asset.GetPathName(), &Asset, {}, Snapshot, Properties, Error)
        && UeAgentDetailsAuthoringService::ProjectReadableProperties(Snapshot, Properties, Error);
}

bool PreparePoints(UPCGDataAsset& Asset, const TArray<FPCGPoint>& Before, const FJsonObject& DataRow,
    TArray<FPCGPoint>& After, UE::UAI::Contract::FDiagnostic& Error)
{
    const auto Desired = DataRow.TryGetField(TEXT("points"));
    TArray<HjsonWorkspace::FOrderedStructuredItemBinding> Bindings;
    if (!BindRows(Desired, Before.Num(), Bindings, Error))
    {
        LocateFieldFailure(Error, DataRow, TEXT("points"));
        return false;
    }
    // Ordered bindings preserve input order; CapturedIndex only selects native data.
    for (int32 InputIndex = 0; InputIndex < Bindings.Num(); ++InputIndex)
    {
        const auto& Binding = Bindings[InputIndex];
        FPCGPoint Point = Binding.CapturedIndex == INDEX_NONE ? FPCGPoint() : Before[Binding.CapturedIndex];
        const auto Row = Desired->AsArray()[InputIndex]->AsObject();
        if (const auto Settings = Row->TryGetField(TEXT("settings")))
        {
            UeAgentAuthoringState::FStateSnapshot Snapshot;
            TSharedPtr<FJsonObject> Properties;
            TArray<TSharedPtr<FJsonValue>> Descriptors;
            if (!ReadPoint(Asset, Point, Snapshot, Properties, Error)) return false;
            if (!UeAgentDetailsAuthoringService::DecodeReadableProperties(Snapshot, Settings, Descriptors, Error))
            {
                LocateFieldFailure(Error, *Row, TEXT("settings"));
                return false;
            }
            const auto LocatePropertyFailure = [&]()
            {
                UeAgentDetailsAuthoringService::LocateReadablePropertyFailure(Error, Settings, TEXT("/settings"));
                UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Row, TEXT("settings"));
            };
            UeAgentAuthoringObjectApply::FOptions Options;
            Options.bCompactValues = true;
            Options.bEmitEditorNotifications = false;
            Options.bMarkPackageDirty = false;
            UeAgentAuthoringObjectApply::FPreparedApply Prepared;
            UeAgentAuthoringObjectApply::FFailure Failure;
            UeAgentAuthoringObjectApply::FApplyResult Applied;
            if (!UeAgentAuthoringObjectApply::PrepareStruct(FPCGPoint::StaticStruct(), &Point, &Asset,
                Descriptors, Options, Prepared, Failure))
            { Error = Failure.Diagnostic; LocatePropertyFailure(); return false; }
            if (!UeAgentAuthoringObjectApply::ApplyPrepared(Prepared, Applied))
            { Error = Applied.Diagnostic; LocatePropertyFailure(); return false; }
        }
        After.Add(MoveTemp(Point));
    }
    return true;
}
}

bool Read(UPCGDataAsset& Asset, TSharedPtr<FJsonValue>& Value,
    UE::UAI::Contract::FDiagnostic& Error, const FString& LogicalTarget)
{
    ON_SCOPE_EXIT { BindTarget(Error, Asset, LogicalTarget); };
    TArray<TSharedPtr<FJsonValue>> Sets;
    for (int32 Index = 0; Index < Asset.Data.TaggedData.Num(); ++Index)
    {
        const auto* Data = Asset.Data.TaggedData[Index].Data.Get();
        const auto* Points = Cast<UPCGBasePointData>(Data);
        auto Row = MakeShared<FJsonObject>();
        if (!AddBinding(*Row, Index, Error)) return false;
        Row->SetStringField(TEXT("data_type"), DataType(Data));
        if (Points)
        {
            TArray<TSharedPtr<FJsonValue>> Values;
            const auto NativePoints = ReadPoints(*Points);
            for (int32 PointIndex = 0; PointIndex < NativePoints.Num(); ++PointIndex)
            {
                auto PointRow = MakeShared<FJsonObject>();
                UeAgentAuthoringState::FStateSnapshot Snapshot;
                TSharedPtr<FJsonObject> Properties;
                if (!AddBinding(*PointRow, PointIndex, Error)
                    || !ReadPoint(Asset, NativePoints[PointIndex], Snapshot, Properties, Error)) return false;
                PointRow->SetObjectField(TEXT("settings"), Properties);
                Values.Add(MakeShared<FJsonValueObject>(PointRow));
            }
            Row->SetArrayField(TEXT("points"), Values);
        }
        Sets.Add(MakeShared<FJsonValueObject>(Row));
    }
    auto Result = MakeShared<FJsonObject>();
    auto* SourceProperty = FindFProperty<FProperty>(UPCGDataAsset::StaticClass(), TEXT("ObjectPath"));
    Result->SetField(TEXT("source"), UeAgentPropertyValueCodec::Encode(SourceProperty, &Asset.ObjectPath, &Asset, {}));
    Result->SetArrayField(TEXT("sets"), Sets);
    Value = MakeShared<FJsonValueObject>(Result);
    return true;
}

bool Apply(UPCGDataAsset& Asset, const TSharedPtr<FJsonValue>& Value,
    UE::UAI::Contract::FDiagnostic& Error, const FString& LogicalTarget)
{
    ON_SCOPE_EXIT { BindTarget(Error, Asset, LogicalTarget); };
    if (!Value || Value->Type != EJson::Object)
    {
        Error = InputFailure(TEXT("pcg_data_object_required"), TEXT("PCG data asset state must be an object."),
            FString(), Value, TEXT("a PCG data asset state object"));
        return false;
    }
    FSoftObjectPath Source = Asset.ObjectPath;
    if (const auto DesiredSource = Value->AsObject()->TryGetField(TEXT("source")))
    {
        auto* SourceProperty = FindFProperty<FProperty>(UPCGDataAsset::StaticClass(), TEXT("ObjectPath"));
        if (!UeAgentPropertyValueCodec::Decode(SourceProperty, DesiredSource, &Source, &Asset, {}, Error))
        {
            LocateFieldFailure(Error, *Value->AsObject(), TEXT("source"));
            return false;
        }
    }
    const auto Desired = Value->AsObject()->TryGetField(TEXT("sets"));
    if (!Desired)
    {
        Asset.Modify(); Asset.ObjectPath = Source; Asset.PostEditChange(); Asset.MarkPackageDirty();
        return true;
    }
    TArray<UE::UAI::HjsonWorkspace::FOrderedStructuredItemBinding> Bindings;
    if (!BindRows(Desired, Asset.Data.TaggedData.Num(), Bindings, Error))
    {
        LocateFieldFailure(Error, *Value->AsObject(), TEXT("sets"));
        return false;
    }
    TArray<FPCGTaggedData> After;
    // Use the authored row itself so nested values retain their parser source identity.
    for (int32 InputIndex = 0; InputIndex < Bindings.Num(); ++InputIndex)
    {
        const auto& Binding = Bindings[InputIndex];
        const auto Row = Desired->AsArray()[InputIndex]->AsObject();
        if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Row,
            {{TEXT("data_type"), EJson::String}}, Error)) return false;
        FPCGTaggedData Tagged = Binding.CapturedIndex == INDEX_NONE ? FPCGTaggedData() : Asset.Data.TaggedData[Binding.CapturedIndex];
        const auto* Before = Cast<UPCGBasePointData>(Tagged.Data.Get());
        const FString Type = DataType(Tagged.Data.Get());
        FString RequestedType = Binding.CapturedIndex == INDEX_NONE ? TEXT("point") : Type;
        Row->TryGetStringField(TEXT("data_type"), RequestedType);
        if ((Binding.CapturedIndex != INDEX_NONE && RequestedType != Type)
            || (Binding.CapturedIndex == INDEX_NONE && RequestedType != TEXT("point") && RequestedType != TEXT("point_array")))
        {
            Error = InputFailure(TEXT("pcg_data_type_change_unsupported"),
                Binding.CapturedIndex == INDEX_NONE ? TEXT("A new PCG data set must use point or point_array data.")
                    : TEXT("The data type of an existing PCG data set cannot be changed."),
                TEXT("data_type"), MakeShared<FJsonValueString>(RequestedType),
                Binding.CapturedIndex == INDEX_NONE ? FString(TEXT("point or point_array")) : Type, Row.Get());
            return false;
        }
        const auto CreatePoints = [&]() -> UPCGBasePointData* {
            return RequestedType == TEXT("point_array")
                ? static_cast<UPCGBasePointData*>(NewObject<UPCGPointArrayData>(&Asset, NAME_None, RF_Transactional))
                : static_cast<UPCGBasePointData*>(NewObject<UPCGPointData>(&Asset, NAME_None, RF_Transactional));
        };
        if (const auto Points = Row->TryGetField(TEXT("points")))
        {
            if (!Before && Binding.CapturedIndex != INDEX_NONE)
            {
                Error = InputFailure(TEXT("pcg_data_points_require_point_data"),
                    TEXT("Points cannot be edited on an existing PCG data set that is not point data."),
                    TEXT("points"), MakeShared<FJsonValueString>(Type),
                    TEXT("existing point or point_array data"), Row.Get());
                Error.PropertyPath = TEXT("data_type");
                return false;
            }
            const auto BeforePoints = Before ? ReadPoints(*Before) : TArray<FPCGPoint>();
            TArray<FPCGPoint> PreparedPoints;
            if (!PreparePoints(Asset, BeforePoints, *Row, PreparedPoints, Error)) return false;
            bool Changed = !Before || BeforePoints.Num() != PreparedPoints.Num();
            for (int32 Index = 0; !Changed && Index < PreparedPoints.Num(); ++Index)
                Changed = !FPCGPoint::StaticStruct()->CompareScriptStruct(&BeforePoints[Index], &PreparedPoints[Index], 0);
            // Copy the complete native data (including metadata) into this owner.
            // Preview and actual apply must never mutate data shared with another asset.
            if (Changed)
            {
                auto* Edited = Before ? DuplicateObject<UPCGBasePointData>(Before, &Asset) : CreatePoints();
                WritePoints(*Edited, PreparedPoints);
                Tagged.Data = Edited;
            }
        }
        else if (Binding.CapturedIndex == INDEX_NONE)
            Tagged.Data = CreatePoints();
        After.Add(MoveTemp(Tagged));
    }
    Asset.Modify();
    Asset.ObjectPath = Source;
    Asset.Data.TaggedData = MoveTemp(After);
    Asset.Data.ComputeCrcs(true);
    Asset.PostEditChange();
    Asset.MarkPackageDirty();
    return true;
}

void AppendPreservationSnapshot(UPCGDataAsset& Asset, const TSharedPtr<FJsonValue>& Value)
{
    // Operation-only state: never expose metadata entry keys or checksums in
    // the editable workspace. Native metadata CRC resolves values per point.
    TArray<TSharedPtr<FJsonValue>> Records;
    for (const auto& Tagged : Asset.Data.TaggedData)
    {
        auto Record = MakeShared<FJsonObject>();
        TArray<FString> Tags = Tagged.Tags.Array();
        Tags.Sort();
        TArray<TSharedPtr<FJsonValue>> TagValues;
        for (const auto& Tag : Tags) TagValues.Add(MakeShared<FJsonValueString>(Tag));
        Record->SetArrayField(TEXT("tags"), TagValues);
        Record->SetStringField(TEXT("pin"), Tagged.Pin.ToString());
        Record->SetBoolField(TEXT("pinless"), Tagged.bPinlessData);
        const auto* Data = Tagged.Data.Get();
        Record->SetBoolField(TEXT("has_metadata"), Data && Data->ConstMetadata());
        if (Data && Data->ConstMetadata())
        {
            FArchiveCrc32 Archive;
            Data->ConstMetadata()->AddToCrc(Archive, true);
            Record->SetStringField(TEXT("metadata_crc"), LexToString(Archive.GetCrc()));
        }
        if (const auto* Points = Cast<UPCGBasePointData>(Data))
        {
            TArray<TSharedPtr<FJsonValue>> Entries;
            for (int32 Index = 0; Index < Points->GetNumPoints(); ++Index)
                Entries.Add(MakeShared<FJsonValueString>(LexToString(Points->GetMetadataEntry(Index))));
            Record->SetArrayField(TEXT("metadata_entries"), Entries);
        }
        Records.Add(MakeShared<FJsonValueObject>(Record));
    }
    Value->AsObject()->SetArrayField(TEXT("preservation"), Records);
}

TSharedPtr<FJsonObject> BuildValueDefinition()
{
    auto Contract = MakeShared<FJsonObject>();
    Contract->SetStringField(TEXT("collection_semantics"), TEXT("keyed_desired_state"));
    auto Sets = MakeShared<FJsonObject>();
    Sets->SetStringField(TEXT("type"), TEXT("object[]"));
    Sets->SetObjectField(TEXT("contract"), Contract);
    auto Fields = MakeShared<FJsonObject>();
    Fields->SetObjectField(TEXT("sets"), Sets);
    auto Source = MakeShared<FJsonObject>();
    Source->SetObjectField(TEXT("type"), UeAgentAuthoringTypeDescriptors::Describe(
        FindFProperty<FProperty>(UPCGDataAsset::StaticClass(), TEXT("ObjectPath"))).ToJsonObject());
    Fields->SetObjectField(TEXT("source"), Source);
    // Preservation is internal verification data, not an editable field definition.
    auto Definition = MakeShared<FJsonObject>();
    Definition->SetObjectField(TEXT("fields"), Fields);
    return Definition;
}
}
