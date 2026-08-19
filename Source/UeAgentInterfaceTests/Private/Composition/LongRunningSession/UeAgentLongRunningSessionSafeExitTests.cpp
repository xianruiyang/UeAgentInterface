#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "GameFramework/WorldSettings.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UObject/Package.h"

namespace
{
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::Tests::PublicCommand;

	TUniquePtr<FScopedPublicCommandComposition> GSafeExitHostComposition;

	TSharedPtr<FJsonObject> ParseObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Result;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(
			Reader,
			Result,
			FJsonSerializer::EFlags::StoreNumbersAsStrings);
		return Result;
	}

	FString JsonText(const TSharedRef<FJsonObject>& Object)
	{
		FString Result;
		const TSharedRef<TJsonWriter<
			TCHAR,
			TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<
				TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
		FJsonSerializer::Serialize(Object, Writer);
		return Result;
	}

	FString OperationErrorCode(const TSharedPtr<FJsonObject>& Root)
	{
		const TSharedPtr<FJsonObject>* Error = nullptr;
		return Root.IsValid()
			&& Root->TryGetObjectField(TEXT("operation_error"), Error)
			&& Error
			&& Error->IsValid()
			? (*Error)->GetStringField(TEXT("code"))
			: FString();
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLongRunningSessionSafeExitTerminalProcessWitnessTest,
	"UeAgentInterface.Composition.LongRunningSession.E3."
		"SafeExitTerminalProcessWitness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentLongRunningSessionSafeExitTerminalProcessWitnessTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::Tests::PublicCommand;

	UWorld* World = GEditor
		? GEditor->GetEditorWorldContext().World()
		: nullptr;
	AWorldSettings* WorldSettings = World ? World->GetWorldSettings() : nullptr;
	if (!TestNotNull(TEXT("editor world settings"), WorldSettings))
	{
		return false;
	}

	TUniquePtr<FScopedPublicCommandComposition> Composition =
		MakeUnique<FScopedPublicCommandComposition>(TEXT("safe-exit-e3"));
	FString StartError;
	if (!TestTrue(
			TEXT("public production composition starts"),
			Composition->TryStart(*WorldSettings, StartError)))
	{
		AddError(StartError);
		return false;
	}

	UPackage* DirtyPackage = CreatePackage(
		TEXT("/Game/__UAI_LongRunningSession/SafeExitDirtyFixture"));
	if (!TestNotNull(TEXT("dirty fixture package"), DirtyPackage))
	{
		return false;
	}
	DirtyPackage->SetDirtyFlag(true);
	if (!TestTrue(TEXT("dirty fixture is dirty"), DirtyPackage->IsDirty()))
	{
		return false;
	}

	TOptional<FUeAgentCommandResult> RejectResult;
	FString InvokeError;
	if (!TestTrue(
			TEXT("dirty reject reaches the public command edge"),
			TryInvokeCommand(
				Composition->GetRouteHost(),
				Composition->GetConfig(),
				TEXT("editor.safe_exit"),
				TEXT("{\"dirty_resolution\":{\"mode\":\"reject\"," 
					"\"save_resources\":[],\"discard_resources\":[]}}"),
				RejectResult,
				InvokeError))
		|| !RejectResult.IsSet())
	{
		DirtyPackage->SetDirtyFlag(false);
		AddError(InvokeError);
		return false;
	}
	const TSharedPtr<FJsonObject> RejectRoot =
		ParseObject(RejectResult->GetPayloadJson());
	if (!TestEqual(
			TEXT("dirty reject fails before an operation is accepted"),
			OperationErrorCode(RejectRoot),
			FString(TEXT("editor_safe_exit_dirty_resources_present"))))
	{
		DirtyPackage->SetDirtyFlag(false);
		return false;
	}

	DirtyPackage->SetDirtyFlag(false);
	TestFalse(TEXT("fixture cleanup establishes the clean success path"),
		DirtyPackage->IsDirty());

	TOptional<FUeAgentCommandResult> StartResult;
	InvokeError.Reset();
	if (!TestTrue(
			TEXT("clean safe exit reaches the public command edge"),
			TryInvokeCommand(
				Composition->GetRouteHost(),
				Composition->GetConfig(),
				TEXT("editor.safe_exit"),
				TEXT("{\"dirty_resolution\":{\"mode\":\"reject\"," 
					"\"save_resources\":[],\"discard_resources\":[]}}"),
				StartResult,
				InvokeError))
		|| !StartResult.IsSet())
	{
		AddError(InvokeError);
		return false;
	}
	const TSharedPtr<FJsonObject> StartRoot =
		ParseObject(StartResult->GetPayloadJson());
	if (!TestTrue(
			TEXT("clean safe exit returns an accepted snapshot"),
			StartRoot.IsValid() && OperationErrorCode(StartRoot).IsEmpty()))
	{
		return false;
	}
	TestEqual(TEXT("accepted phase"),
		StartRoot->GetStringField(TEXT("phase")), FString(TEXT("accepted")));
	TestFalse(TEXT("accepted is nonterminal"),
		StartRoot->GetBoolField(TEXT("terminal")));

	const TSharedPtr<FJsonObject> OperationRef =
		StartRoot->GetObjectField(TEXT("operation_ref"));
	const FString OperationRefJson = JsonText(OperationRef.ToSharedRef());
	const FString ResumeParams = TEXT("{\"operation_ref\":")
		+ OperationRefJson + TEXT(",\"expected_generation\":0}");
	TOptional<FUeAgentCommandResult> ResumeResult;
	InvokeError.Reset();
	if (!TestTrue(
			TEXT("resume schedules the owned worker exit"),
			TryInvokeCommand(
				Composition->GetRouteHost(),
				Composition->GetConfig(),
				TEXT("operation.resume"),
				ResumeParams,
				ResumeResult,
				InvokeError))
		|| !ResumeResult.IsSet())
	{
		AddError(InvokeError);
		return false;
	}
	const TSharedPtr<FJsonObject> ResumeRoot =
		ParseObject(ResumeResult->GetPayloadJson());
	if (!TestTrue(
			TEXT("resume returns a nonterminal snapshot"),
			ResumeRoot.IsValid() && OperationErrorCode(ResumeRoot).IsEmpty()))
	{
		return false;
	}
	TestFalse(TEXT("resume remains nonterminal"),
		ResumeRoot->GetBoolField(TEXT("terminal")));
	TestEqual(
		TEXT("resume reports only the nonterminal running lifecycle"),
		ResumeRoot->GetStringField(TEXT("phase")),
		FString(TEXT("running")));

	// Keep the production owner alive until its operation-scoped ticker fires.
	// The owned UnrealEditor-Cmd exits after this Automation result is emitted;
	// RunSmokeTests.ps1 provides the independent process witness and also proves
	// that a pre-existing main Editor remains alive.
	GSafeExitHostComposition = MoveTemp(Composition);
	return !HasAnyErrors();
}
