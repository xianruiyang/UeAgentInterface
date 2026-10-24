#include "Domains/Reflection/Authoring/UeAgentSoundSubmixAuthoringRelations.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "AudioDeviceManager.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Sound/SoundSubmix.h"
#include "UObject/Package.h"

namespace UeAgentReflectionAuthoring::SoundSubmixRelations
{
namespace
{
bool InputFailure(UE::UAI::Contract::FDiagnostic& Error, const FString& Code, const FString& Reason,
    const FString& Pointer, const TSharedPtr<FJsonValue>& Actual, const FString& Expected,
    const FString& Target = FString())
{
    Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Pointer,
        MakeShared<FJsonValueString>(Expected), Actual, TEXT("relationship_preflight"), Reason);
    Error.SourceKind = TEXT("authored_value");
    Error.Source.InternalJsonPath = Pointer;
    Error.ActualValuePresent = Actual.IsValid();
    Error.ExpectedValuePresent = true;
    Error.TargetPath = Target;
    return false;
}
}

bool Validate(const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError)
{
    if (!Value || Value->Type != EJson::Array)
        return InputFailure(OutError, TEXT("sound_submix_children_array_required"),
            TEXT("The sound submix children field must be an array."), TEXT("/child_submixes"), Value, TEXT("an array"));
    TSet<FString> Seen;
    for (int32 Index = 0; Index < Value->AsArray().Num(); ++Index)
    {
        const auto& Child = Value->AsArray()[Index];
        const FString Pointer = FString::Printf(TEXT("/child_submixes/%d"), Index);
        if (!Child || Child->Type != EJson::String || Child->AsString().IsEmpty())
            return InputFailure(OutError, TEXT("sound_submix_child_reference_required"),
                TEXT("This child must be a non-empty sound submix asset reference."),
                Pointer, Child, TEXT("a non-empty asset reference"));
        if (Seen.Contains(Child->AsString()))
            return InputFailure(OutError, TEXT("sound_submix_child_duplicate"),
                TEXT("This child asset reference already appears earlier in the array."),
                Pointer, Child, TEXT("a unique child asset reference"));
        Seen.Add(Child->AsString());
    }
    return true;
}

TSharedPtr<FJsonValue> Read(const USoundSubmixBase& Asset)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const USoundSubmixBase* Child : Asset.ChildSubmixes) if (Child) Values.Add(MakeShared<FJsonValueString>(Child->GetPathName()));
	return MakeShared<FJsonValueArray>(Values);
}
bool Prepare(USoundSubmixBase& Root, const TSharedPtr<FJsonValue>& Value,
	UeAgentAuthoringAssetRelations::FPlan& OutPlan, UE::UAI::Contract::FDiagnostic& OutError)
{
	if (!Validate(Value, OutError)) { OutError.TargetPath = Root.GetPathName(); return false; }
	TArray<UObject*> Desired;
	TSet<USoundSubmixBase*> Seen;
	for (int32 Index = 0; Index < Value->AsArray().Num(); ++Index)
	{
		const auto& Item = Value->AsArray()[Index];
		const FString Pointer = FString::Printf(TEXT("/child_submixes/%d"), Index);
		// Endpoint-only submixes cannot have a parent. This is the native graph
		// qualification, not a restriction to one concrete submix class.
		auto* Child = LoadObject<USoundSubmixWithParentBase>(nullptr, *Item->AsString());
		if (!Child) return InputFailure(OutError, TEXT("sound_submix_child_not_parentable"),
            TEXT("The referenced child could not be loaded as a parentable USoundSubmixWithParentBase."),
            Pointer, Item, TEXT("a loadable USoundSubmixWithParentBase asset"), Root.GetPathName());
		if (Seen.Contains(Child)) return InputFailure(OutError, TEXT("sound_submix_child_duplicate"),
            FString::Printf(TEXT("This reference resolves to child '%s', which is already present earlier in the array."), *Child->GetPathName()),
            Pointer, Item, TEXT("a reference resolving to a distinct child asset"), Root.GetPathName());
		Seen.Add(Child);
		TArray<USoundSubmixBase*> Pending{Child};
		TSet<USoundSubmixBase*> Visited;
		while (!Pending.IsEmpty())
		{
			auto* Node = Pending.Pop(EAllowShrinking::No);
			if (Node == &Root) return InputFailure(OutError, TEXT("sound_submix_cycle"),
                TEXT("This child already reaches the target sound submix through its descendants; adding it would create a cycle."),
                Pointer, Item, TEXT("a child that does not reach the target sound submix"), Root.GetPathName());
			if (!Node || Visited.Contains(Node)) continue;
			Visited.Add(Node);
			for (USoundSubmixBase* Descendant : Node->ChildSubmixes) Pending.Add(Descendant);
		}
		Desired.Add(Child);
	}
	return OutPlan.Prepare(Root, Desired, GET_MEMBER_NAME_CHECKED(USoundSubmixWithParentBase, ParentSubmix),
		GET_MEMBER_NAME_CHECKED(USoundSubmixBase, ChildSubmixes), [](UObject& Child, UObject* Parent)
		{ CastChecked<USoundSubmixWithParentBase>(&Child)->SetParentSubmix(Cast<USoundSubmixBase>(Parent)); }, OutError);
}
void RefreshDevices(USoundSubmixBase& Asset)
{
	if (auto* Devices = FAudioDeviceManager::Get()) Devices->RegisterSoundSubmix(&Asset);
}
UObject* CreatePreview(USoundSubmixBase& Source, UPackage& Package, UE::UAI::Contract::FDiagnostic& OutError)
{
	OutError.Reset();
	// Normal asset duplication clears parentage and modifies the real parent
	// from PostDuplicate. A prospective state must not execute that mutation.
	UObject* Preview = NewObject<UObject>(&Package, Source.GetClass(), NAME_None, RF_Public | RF_Standalone | RF_Transactional);
	if (!Preview)
	{
		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("sound_submix_preview_create_failed"),
			TEXT("Unreal could not allocate the sound submix preview object."), TEXT("authoring_prepare"));
		OutError.SourceKind = TEXT("native_result");
		OutError.TargetPath = Source.GetPathName();
		OutError.PropertyPath = TEXT("preview_created");
		OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
		OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
		return nullptr;
	}
	UEngine::FCopyPropertiesForUnrelatedObjectsParams Parameters;
	Parameters.bDoDelta = false;
	Parameters.bClearReferences = false;
	UEngine::CopyPropertiesForUnrelatedObjects(&Source, Preview, Parameters);
	return Preview;
}
}
