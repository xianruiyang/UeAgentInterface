#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "InputMappingContext.h"
#include "InputTriggers.h"
#include "Dom/JsonObject.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTargetLocator.h"
#include "Utilities/AuthoringObjectApply/UeAgentOwnedObjectCreation.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208NestedOwnedCandidateTest,
	"UAI.Root.T208.InputMappingContext.NestedOwnedCandidate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208NestedOwnedCandidateTest::RunTest(const FString&)
{
	auto* Context = NewObject<UInputMappingContext>(GetTransientPackage());
	UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Mappings;
	UeAgentAuthoringTargetLocator::FResolveResult Resolve;
	if (!TestTrue(TEXT("root mappings resolve"),
		UeAgentAuthoringTargetLocator::ResolvePropertyPath(Context, TEXT("Mappings"), Mappings, Resolve))) return false;
	auto* MappingsProperty = CastFieldChecked<FArrayProperty>(Mappings.Property);
	FScriptArrayHelper MappingArray(MappingsProperty, Mappings.ValuePtr);
	UeAgentPropertyValueCodec::FOptions CodecOptions;
	CodecOptions.bCaptureOwnedObjectDefinitions = true;
	CodecOptions.bCaptureStructFieldDescriptors = true;
	int32 CapturedObjectCalls = 0;
	CodecOptions.ObjectReferenceEncoder = [&](const FObjectPropertyBase*, const void*, UObject*, int32,
		TSharedPtr<FJsonValue>&, TSharedPtr<FJsonObject>*) { ++CapturedObjectCalls; return false; };
	TSharedPtr<FJsonObject> MappingDefinition;
	UeAgentPropertyValueCodec::Encode(Mappings.Property, Mappings.ValuePtr, Context,
		CodecOptions, 0, &MappingDefinition);
	TestEqual(TEXT("empty owned-descendant struct arrays retain their type contract"),
		MappingDefinition->GetStringField(TEXT("element_kind")), FString(TEXT("structured_owned_item")));
	TestTrue(TEXT("empty structured-owned array has separate element metadata"), MappingDefinition->HasField(TEXT("element_definition")) && MappingDefinition->HasField(TEXT("element_template")));
	TestEqual(TEXT("template is not a captured existing item"), MappingDefinition->GetArrayField(TEXT("items")).Num(), 0);
	TestEqual(TEXT("template does not invoke real owned capture callbacks"), CapturedObjectCalls, 0);
	const auto EmptyTemplate = MappingDefinition->GetObjectField(TEXT("element_template"));
	TestFalse(TEXT("template has no captured instance identity"), EmptyTemplate->HasField(TEXT("instance")));
	MappingArray.AddValue();
	UeAgentPropertyValueCodec::Encode(Mappings.Property, Mappings.ValuePtr, Context,
		CodecOptions, 0, &MappingDefinition);
	TestEqual(TEXT("owned-descendant struct arrays carry the common structured item contract"),
		MappingDefinition->GetStringField(TEXT("element_kind")), FString(TEXT("structured_owned_item")));
	TestEqual(TEXT("template does not add to captured item inventory"), MappingDefinition->GetArrayField(TEXT("items")).Num(), 1);
	TestTrue(TEXT("nonempty structured-owned array retains insertion template"), MappingDefinition->HasField(TEXT("element_template")));
	UeAgentAuthoringTargetLocator::FResolvedPropertyTarget PlainArray;
	if (!TestTrue(TEXT("ordinary value array resolves"), UeAgentAuthoringTargetLocator::ResolvePropertyPath(
		Context, TEXT("InputModeQueryOverride.TagDictionary"), PlainArray, Resolve))) return false;
	FScriptArrayHelper PlainItems(CastFieldChecked<FArrayProperty>(PlainArray.Property), PlainArray.ValuePtr);
	PlainItems.AddValue();
	TSharedPtr<FJsonObject> PlainDefinition;
	UeAgentPropertyValueCodec::Encode(PlainArray.Property, PlainArray.ValuePtr, Context,
		CodecOptions, 0, &PlainDefinition);
	TestTrue(TEXT("arrays without owned descendants retain the original definition contract"),
		!PlainDefinition.IsValid() || !PlainDefinition->HasField(TEXT("element_kind")));

	const bool bDirtyBefore = Context->GetPackage()->IsDirty();
	FString Error;
	UeAgentAuthoringObjectApply::FNativeOwnedCreationContext Prospective;
	if (!TestTrue(TEXT("nested trigger context is eligible"),
		Prospective.Initialize(Context, TEXT("Mappings[0].Triggers"), true, Error, 0)))
	{
		AddError(Error);
		return false;
	}
	TArray<UeAgentAuthoringObjectApply::FOwnedClassCandidate> Candidates;
	if (!TestTrue(TEXT("nested trigger candidates collect"), Prospective.Collect(Candidates, Error)))
	{
		AddError(Error);
		return false;
	}
	const FString HoldName = UInputTriggerHold::StaticClass()->GetDisplayNameText().ToString();
	TestTrue(TEXT("native candidate name is returned unchanged"), Candidates.ContainsByPredicate(
		[&](const auto& Candidate) { return Candidate.Selection.Name == HoldName && Candidate.Class == UInputTriggerHold::StaticClass(); }));

	UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Triggers;
	TestTrue(TEXT("nested property path resolves"),
		UeAgentAuthoringTargetLocator::ResolvePropertyPath(Context, TEXT("Mappings[0].Triggers"), Triggers, Resolve));
	auto* TriggersProperty = CastFieldChecked<FArrayProperty>(Triggers.Property);
	FScriptArrayHelper TriggerArray(TriggersProperty, Triggers.ValuePtr);
	TestEqual(TEXT("prospective query leaves real trigger array unchanged"), TriggerArray.Num(), 0);
	TestEqual(TEXT("prospective query preserves package dirty state"), Context->GetPackage()->IsDirty(), bDirtyBefore);

	TriggerArray.AddValue();
	UeAgentAuthoringObjectApply::FNativeOwnedCreationContext Actual;
	if (!TestTrue(TEXT("real nested trigger slot is eligible"),
		Actual.Initialize(Context, TEXT("Mappings[0].Triggers"), false, Error, 0)))
	{
		AddError(Error);
		return false;
	}
	auto* Created = Actual.Create(UInputTriggerHold::StaticClass(), Error);
	TestTrue(TEXT("native picker creates selected trigger in parent context"),
		Created && Created->GetClass() == UInputTriggerHold::StaticClass() && Created->GetOuter() == Context);

	UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Invalid;
	TestFalse(TEXT("out of range mapping rejects"),
		UeAgentAuthoringTargetLocator::ResolvePropertyPath(Context, TEXT("Mappings[1].Triggers"), Invalid, Resolve));
	TestFalse(TEXT("malformed mapping index rejects"),
		UeAgentAuthoringTargetLocator::ResolvePropertyPath(Context, TEXT("Mappings[x].Triggers"), Invalid, Resolve));
	TestFalse(TEXT("indexed non-array rejects"),
		UeAgentAuthoringTargetLocator::ResolvePropertyPath(Context, TEXT("Mappings[0].Triggers[0].ActuationThreshold[0]"), Invalid, Resolve));
	return !HasAnyErrors();
}

#endif
