#include "Misc/AutomationTest.h"

#if __has_include("DomainAdapters/Sequence/UeAgentSequenceHjsonContract.h")
#define UAI_SEQUENCE_HJSON_CONTRACT_AVAILABLE 1
#include "DomainAdapters/Sequence/UeAgentSequenceHjsonContract.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#else
#define UAI_SEQUENCE_HJSON_CONTRACT_AVAILABLE 0
#endif

#if UAI_SEQUENCE_HJSON_CONTRACT_AVAILABLE
namespace
{
	using namespace UE::UAI::DomainAdapters::Sequence;
	using namespace UE::UAI::HjsonWorkspace;

	TSharedRef<FJsonObject> Marker(const FString& Value)
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("marker"), Value);
		return Result;
	}

	TSharedRef<FJsonObject> BaseStructure()
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetObjectField(TEXT("display_rate"), Marker(TEXT("display-old")));
		Result->SetObjectField(TEXT("tick_resolution"), Marker(TEXT("tick-old")));
		Result->SetObjectField(TEXT("playback_range"), Marker(TEXT("range-old")));
		Result->SetArrayField(TEXT("bindings"), {});
		Result->SetArrayField(TEXT("root_tracks"), {});
		return Result;
	}

	TSharedRef<FJsonObject> Field(const FString& Intent)
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("intent"), Intent);
		return Result;
	}

	TSharedRef<FJsonObject> RootWithFields(
		const TSharedRef<FJsonObject>& Fields)
	{
		const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetObjectField(TEXT("structure"), BaseStructure());
		Result->SetObjectField(TEXT("fields"), Fields);
		return Result;
	}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSequenceHjsonExportDocumentTest,
	"UAI.DomainAdapters.Sequence.Hjson.ExportDocumentUsesActiveFieldContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSequenceHjsonExportDocumentTest::RunTest(
	const FString& Parameters)
{
#if !UAI_SEQUENCE_HJSON_CONTRACT_AVAILABLE
	TestTrue(TEXT("Sequence HJSON contract is available"), false);
	return false;
#else
	using namespace UE::UAI::DomainAdapters::Sequence;
	using namespace UE::UAI::HjsonWorkspace;

	ULevelSequence* Sequence = NewObject<ULevelSequence>(
		GetTransientPackage(),
		TEXT("UAI_SequenceHjsonExportDocument"));
	TestNotNull(TEXT("create transient Level Sequence"), Sequence);
	if (!Sequence)
	{
		return false;
	}
	Sequence->Initialize();
	TestNotNull(TEXT("the transient Sequence has a MovieScene"),
		Sequence->GetMovieScene());

	TSharedPtr<FJsonObject> Document;
	FWorkspaceError Error;
	if (!TestTrue(
		TEXT("the active Sequence owner captures one editable document"),
		TryBuildSequenceHjsonExportDocument(
			Sequence->GetPathName(), Document, Error))
		|| !Document.IsValid())
	{
		AddError(Error.GetCode() + TEXT("|") + Error.GetJsonPointer());
		return false;
	}
	TestEqual(TEXT("export uses the active schema identity"),
		Document->GetStringField(TEXT("schema")),
		GetSequenceHjsonSchemaId());

	const TSharedPtr<FJsonObject>* Root = nullptr;
	const TSharedPtr<FJsonObject>* CurrentValues = nullptr;
	const TSharedPtr<FJsonObject>* Fields = nullptr;
	TestTrue(
		TEXT("export remains a settings field-set projection"),
		Document->TryGetObjectField(TEXT("root"), Root)
			&& Root && Root->IsValid()
			&& (*Root)->GetStringField(TEXT("type"))
				== GetSequenceHjsonRootType()
			&& (*Root)->GetStringField(TEXT("field_set"))
				== GetSequenceHjsonFieldSetId()
			&& (*Root)->TryGetObjectField(
				TEXT("current_values"), CurrentValues)
			&& CurrentValues && CurrentValues->IsValid()
			&& (*Root)->TryGetObjectField(TEXT("fields"), Fields)
			&& Fields && Fields->IsValid()
			&& !(*Root)->HasField(TEXT("structure")));
	if (Fields && Fields->IsValid())
	{
		TestEqual(TEXT("the field set exports exactly three owned fields"),
			(*Fields)->Values.Num(), 3);
		for (const FString& FieldId : TArray<FString>{
			TEXT("display_rate"),
			TEXT("tick_resolution"),
			TEXT("playback_range")})
		{
			const TSharedPtr<FJsonObject>* Field = nullptr;
			TestTrue(
				TEXT("export defaults the field to preserve: ") + FieldId,
				(*Fields)->TryGetObjectField(FieldId, Field)
					&& Field && Field->IsValid()
					&& (*Field)->Values.Num() == 1
					&& (*Field)->GetStringField(TEXT("intent"))
						== TEXT("preserve"));
		}
	}
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSequenceHjsonExistingEditModeConflictTest,
	"UAI.DomainAdapters.Sequence.Hjson.ExistingEditModeConflictPrewriteFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSequenceHjsonExistingEditModeConflictTest::RunTest(
	const FString& Parameters)
{
#if !UAI_SEQUENCE_HJSON_CONTRACT_AVAILABLE
	TestTrue(TEXT("Sequence HJSON contract is available"), false);
	return false;
#else
	using namespace UE::UAI::DomainAdapters::Sequence;
	using namespace UE::UAI::HjsonWorkspace;

	const TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
	Fields->SetObjectField(
		TEXT("playback_range"), Field(TEXT("preserve")));
	FSequenceHjsonPatch Patch;
	FWorkspaceError Error;
	TestFalse(
		TEXT("an existing edit cannot mix full replacement and sparse fields"),
		TryResolveSequenceHjsonPatch(
			TEXT("/Game/UAI/Tests/Sequence.Sequence"),
			true,
			RootWithFields(Fields),
			Patch,
			Error));
	TestEqual(TEXT("the conflict is typed"), Error.GetCode(),
		FString(TEXT("sequence_hjson_edit_modes_conflict")));
	TestEqual(TEXT("the conflict identifies the mode envelope"),
		Error.GetJsonPointer(), FString(TEXT("/root")));
	TestEqual(TEXT("the conflict identifies the field owner"),
		Error.GetSourceUnit(), GetSequenceHjsonFieldOwner());
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSequenceHjsonSparseFieldContractTest,
	"UAI.DomainAdapters.Sequence.Hjson.SparseFieldContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSequenceHjsonSparseFieldContractTest::RunTest(
	const FString& Parameters)
{
#if !UAI_SEQUENCE_HJSON_CONTRACT_AVAILABLE
	TestTrue(TEXT("Sequence HJSON contract is available"), false);
	return false;
#else
	using namespace UE::UAI::DomainAdapters::Sequence;
	using namespace UE::UAI::HjsonWorkspace;

	const TSharedRef<FJsonObject> SetPlayback = Field(TEXT("set"));
	SetPlayback->SetObjectField(TEXT("value"), Marker(TEXT("range-new")));
	const TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
	Fields->SetObjectField(TEXT("display_rate"), Field(TEXT("preserve")));
	Fields->SetObjectField(TEXT("playback_range"), SetPlayback);

	FSequenceHjsonPatch Patch;
	FWorkspaceError Error;
	if (!TestTrue(
		TEXT("a sparse set with omitted and explicit preserve fields resolves"),
		TryResolveSequenceHjsonPatch(
			TEXT("/Game/UAI/Tests/Sequence.Sequence"),
			false,
			RootWithFields(Fields),
			Patch,
			Error)))
	{
		AddError(Error.GetCode() + TEXT("|") + Error.GetJsonPointer());
		return false;
	}
	TestEqual(
		TEXT("only the explicitly set field is reported changed"),
		Patch.ChangedFields,
		TArray<FString>{TEXT("playback_range")});
	TestTrue(
		TEXT("the fields envelope selects the non-destructive settings path"),
		Patch.bSparseSettings);

	const TSharedPtr<FJsonObject>* DisplayRate = nullptr;
	const TSharedPtr<FJsonObject>* TickResolution = nullptr;
	const TSharedPtr<FJsonObject>* PlaybackRange = nullptr;
	TestTrue(
		TEXT("the omitted display and tick fields remain in the complete native structure"),
		Patch.Structure.IsValid()
			&& Patch.Structure->TryGetObjectField(TEXT("display_rate"), DisplayRate)
			&& DisplayRate && (*DisplayRate)->GetStringField(TEXT("marker"))
				== TEXT("display-old")
			&& Patch.Structure->TryGetObjectField(
				TEXT("tick_resolution"), TickResolution)
			&& TickResolution && (*TickResolution)->GetStringField(TEXT("marker"))
				== TEXT("tick-old"));
	TestTrue(
		TEXT("the explicit set field replaces its prior value"),
		Patch.Structure->TryGetObjectField(TEXT("playback_range"), PlaybackRange)
			&& PlaybackRange && (*PlaybackRange)->GetStringField(TEXT("marker"))
				== TEXT("range-new"));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSequenceHjsonUnsupportedIntentTest,
	"UAI.DomainAdapters.Sequence.Hjson.UnsupportedIntentPrewriteFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSequenceHjsonUnsupportedIntentTest::RunTest(
	const FString& Parameters)
{
#if !UAI_SEQUENCE_HJSON_CONTRACT_AVAILABLE
	TestTrue(TEXT("Sequence HJSON contract is available"), false);
	return false;
#else
	using namespace UE::UAI::DomainAdapters::Sequence;
	using namespace UE::UAI::HjsonWorkspace;

	for (const FString& Intent
		: TArray<FString>{TEXT("remove"), TEXT("null"), TEXT("default")})
	{
		const TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
		Fields->SetObjectField(TEXT("playback_range"), Field(Intent));
		FSequenceHjsonPatch Patch;
		FWorkspaceError Error;
		TestFalse(
			TEXT("an unsupported intent fails before native mutation: ") + Intent,
			TryResolveSequenceHjsonPatch(
				TEXT("/Game/UAI/Tests/Sequence.Sequence"),
				false,
				RootWithFields(Fields),
				Patch,
				Error));
		TestEqual(
			TEXT("the unsupported intent has one stable error code"),
			Error.GetCode(),
			FString(TEXT("sequence_hjson_field_intent_unsupported")));
		TestEqual(
			TEXT("the failure identifies the exact field pointer"),
			Error.GetJsonPointer(),
			FString(TEXT("/root/fields/playback_range")));
		TestEqual(
			TEXT("the failure identifies the field owner"),
			Error.GetSourceUnit(),
			GetSequenceHjsonFieldOwner());
	}

	const TSharedRef<FJsonObject> TickFields = MakeShared<FJsonObject>();
	const TSharedRef<FJsonObject> SetTick = Field(TEXT("set"));
	SetTick->SetObjectField(TEXT("value"), Marker(TEXT("tick-new")));
	TickFields->SetObjectField(TEXT("tick_resolution"), SetTick);
	FSequenceHjsonPatch TickPatch;
	FWorkspaceError TickError;
	TestFalse(
		TEXT("tick-resolution mutation fails until a non-interactive retime contract exists"),
		TryResolveSequenceHjsonPatch(
			TEXT("/Game/UAI/Tests/Sequence.Sequence"),
			false,
			RootWithFields(TickFields),
			TickPatch,
			TickError));
	TestEqual(
		TEXT("tick-resolution write rejection is typed"),
		TickError.GetCode(),
		FString(TEXT("sequence_hjson_field_intent_unsupported")));
	TestEqual(
		TEXT("tick-resolution rejection identifies the exact field"),
		TickError.GetJsonPointer(),
		FString(TEXT("/root/fields/tick_resolution")));

	const TSharedRef<FJsonObject> UnknownFields = MakeShared<FJsonObject>();
	UnknownFields->SetObjectField(TEXT("unknown"), Field(TEXT("set")));
	FSequenceHjsonPatch UnknownPatch;
	FWorkspaceError UnknownError;
	TestFalse(
		TEXT("an unknown field fails before native mutation"),
		TryResolveSequenceHjsonPatch(
			TEXT("/Game/UAI/Tests/Sequence.Sequence"),
			false,
			RootWithFields(UnknownFields),
			UnknownPatch,
			UnknownError));
	TestEqual(
		TEXT("the unknown field failure is typed"),
		UnknownError.GetCode(),
		FString(TEXT("sequence_hjson_field_unknown")));
	TestEqual(
		TEXT("the unknown field failure is precisely located"),
		UnknownError.GetJsonPointer(),
		FString(TEXT("/root/fields/unknown")));
	return true;
#endif
}
