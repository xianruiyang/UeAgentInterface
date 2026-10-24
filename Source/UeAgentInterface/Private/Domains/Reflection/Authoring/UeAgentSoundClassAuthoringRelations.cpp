#include "Domains/Reflection/Authoring/UeAgentSoundClassAuthoringRelations.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Sound/SoundClass.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace UeAgentReflectionAuthoring::SoundClassRelations
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
        return InputFailure(OutError, TEXT("sound_class_children_array_required"),
            TEXT("The sound class children field must be an array."), TEXT("/child_classes"), Value, TEXT("an array"));
    TSet<FString> Seen;
    for (int32 Index = 0; Index < Value->AsArray().Num(); ++Index)
    {
        const auto& Child = Value->AsArray()[Index];
        if (Child && Child->Type == EJson::Null) continue;
        const FString Pointer = FString::Printf(TEXT("/child_classes/%d"), Index);
        if (!Child || Child->Type != EJson::String || Child->AsString().IsEmpty())
            return InputFailure(OutError, TEXT("sound_class_child_reference_required"),
                TEXT("This child must be a non-empty sound class asset reference or null."),
                Pointer, Child, TEXT("a non-empty asset reference or null"));
        if (Seen.Contains(Child->AsString()))
            return InputFailure(OutError, TEXT("sound_class_child_duplicate"),
                TEXT("This child asset reference already appears earlier in the array."),
                Pointer, Child, TEXT("a unique child asset reference"));
        Seen.Add(Child->AsString());
    }
    return true;
}

TSharedPtr<FJsonValue> Read(const USoundClass& Asset)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const USoundClass* Child : Asset.ChildClasses)
	{
		if (Child) Values.Add(MakeShared<FJsonValueString>(Child->GetPathName()));
		else Values.Add(MakeShared<FJsonValueNull>());
	}
	return MakeShared<FJsonValueArray>(Values);
}

void RefreshDerivedGraphs(USoundClass& Asset)
{
	// These graphs are non-persistent derived views. Preserve the flags at this
	// instant, after business mutation, not the transaction's original flags.
	TMap<UPackage*, bool> DirtyBeforeRefresh;
	for (TObjectIterator<USoundClass> It; It; ++It)
		if (It->SoundClassGraph) DirtyBeforeRefresh.FindOrAdd(
			It->SoundClassGraph->GetPackage(), It->SoundClassGraph->GetPackage()->IsDirty());
	Asset.RefreshAllGraphs(false);
	for (const auto& Entry : DirtyBeforeRefresh) Entry.Key->SetDirtyFlag(Entry.Value);
}

bool Prepare(USoundClass& Root, const TSharedPtr<FJsonValue>& Value,
    UeAgentAuthoringAssetRelations::FPlan& OutPlan, UE::UAI::Contract::FDiagnostic& OutError)
{
    if (!Validate(Value, OutError)) { OutError.TargetPath = Root.GetPathName(); return false; }
    TArray<UObject*> Desired;
    TSet<USoundClass*> Seen;
    for (int32 Index = 0; Index < Value->AsArray().Num(); ++Index)
    {
        const auto& Item = Value->AsArray()[Index];
        const FString Pointer = FString::Printf(TEXT("/child_classes/%d"), Index);
        if (Item->Type == EJson::Null) { Desired.Add(nullptr); continue; }
        USoundClass* Child = LoadObject<USoundClass>(nullptr, *Item->AsString());
        if (!Child) return InputFailure(OutError, TEXT("sound_class_child_not_found"),
            TEXT("The referenced child could not be loaded as a USoundClass."),
            Pointer, Item, TEXT("a loadable USoundClass asset"), Root.GetPathName());
        if (Seen.Contains(Child)) return InputFailure(OutError, TEXT("sound_class_child_duplicate"),
            FString::Printf(TEXT("This reference resolves to child '%s', which is already present earlier in the array."), *Child->GetPathName()),
            Pointer, Item, TEXT("a reference resolving to a distinct child asset"), Root.GetPathName());
        Seen.Add(Child);
        TArray<USoundClass*> Pending{Child};
        TSet<USoundClass*> Visited;
        while (!Pending.IsEmpty())
        {
            USoundClass* Node = Pending.Pop(EAllowShrinking::No);
            if (Node == &Root) return InputFailure(OutError, TEXT("sound_class_cycle"),
                TEXT("This child already reaches the target sound class through its descendants; adding it would create a cycle."),
                Pointer, Item, TEXT("a child that does not reach the target sound class"), Root.GetPathName());
            if (!Node || Visited.Contains(Node)) continue;
            Visited.Add(Node);
            for (USoundClass* Descendant : Node->ChildClasses) Pending.Add(Descendant);
        }
        Desired.Add(Child);
    }
    return OutPlan.Prepare(Root, Desired, GET_MEMBER_NAME_CHECKED(USoundClass, ParentClass),
        GET_MEMBER_NAME_CHECKED(USoundClass, ChildClasses), [](UObject& Child, UObject* Parent)
        { CastChecked<USoundClass>(&Child)->SetParentClass(Cast<USoundClass>(Parent)); }, OutError);
}
}
