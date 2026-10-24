#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UObject/Package.h"

namespace UE::UAI::Composition::LongRunningSession::TestSupport
{
	UEAGENTINTERFACE_API bool SafeExitResourceSetsMatchForTest(
		const TArray<FString>& Requested,
		const TArray<FString>& Dirty);

	UEAGENTINTERFACE_API void DiscardSafeExitConfigResourcesForTest(
		const TArray<FString>& ResourceRefs);
}

namespace
{
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::Tests::PublicCommand;

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

	TSharedRef<FJsonObject> ProcessIdentity(
		const uint32 ProcessId,
		const FString& ExecutablePath)
	{
		const TSharedRef<FJsonObject> Identity = MakeShared<FJsonObject>();
		const FDateTime CapturedAt = FDateTime::UtcNow();
		Identity->SetNumberField(TEXT("process_id"), ProcessId);
		Identity->SetStringField(
			TEXT("start_time_utc"),
			CapturedAt.ToIso8601());
		Identity->SetStringField(
			TEXT("creation_time_filetime_100ns"),
			FString::Printf(TEXT("%lld"), CapturedAt.GetTicks()));
		Identity->SetStringField(TEXT("executable_path"), ExecutablePath);
		return Identity;
	}

	bool WriteJsonObject(
		const FString& Path,
		const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<>> Writer =
			TJsonWriterFactory<>::Create(&Text);
		return FJsonSerializer::Serialize(Object, Writer)
			&& FFileHelper::SaveStringToFile(Text, *Path);
	}

	class FScopedArmedWitnessFixture final
	{
	public:
		~FScopedArmedWitnessFixture()
		{
			if (WitnessHandle.IsValid())
			{
				FPlatformProcess::TerminateProc(WitnessHandle, false);
				FPlatformProcess::CloseProc(WitnessHandle);
			}
			if (!AttemptRoot.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(
					*AttemptRoot,
					false,
					true);
			}
		}

		bool TryArm(
			const FString& LaunchId,
			const FString& ExitAttemptId,
			const FString& Nonce,
			FString& OutError)
		{
			OutError.Reset();
#if PLATFORM_WINDOWS
			const FString WitnessExecutable = FPaths::Combine(
				FPlatformMisc::GetEnvironmentVariable(TEXT("SystemRoot")),
				TEXT("System32/WindowsPowerShell/v1.0/powershell.exe"));
			const FString WitnessArguments =
				TEXT("-NoLogo -NoProfile -NonInteractive -Command "
					"\"Start-Sleep -Seconds 30\"");
#else
			const FString WitnessExecutable = TEXT("/bin/sh");
			const FString WitnessArguments = TEXT("-c \"sleep 30\"");
#endif
			WitnessHandle = FPlatformProcess::CreateProc(
				*WitnessExecutable,
				*WitnessArguments,
				false,
				true,
				true,
				&WitnessProcessId,
				0,
				nullptr,
				nullptr,
				nullptr);
			if (!WitnessHandle.IsValid() || WitnessProcessId == 0)
			{
				OutError = TEXT("safe_exit_test_witness_launch_failed");
				return false;
			}
			for (int32 Attempt = 0;
				Attempt < 20
					&& !FPlatformProcess::IsApplicationRunning(WitnessProcessId);
				++Attempt)
			{
				FPlatformProcess::Sleep(0.01f);
			}
			if (!FPlatformProcess::IsApplicationRunning(WitnessProcessId))
			{
				OutError = TEXT("safe_exit_test_witness_not_running");
				return false;
			}

			AttemptRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("UeAgentInterface/SafeExitCompletion/v1"),
				LaunchId,
				ExitAttemptId));
			IFileManager::Get().DeleteDirectory(*AttemptRoot, false, true);
			if (!IFileManager::Get().MakeDirectory(*AttemptRoot, true))
			{
				OutError = TEXT("safe_exit_test_witness_root_failed");
				return false;
			}

			const TSharedRef<FJsonObject> EditorIdentity = ProcessIdentity(
				FPlatformProcess::GetCurrentProcessId(),
				FPlatformProcess::ExecutablePath());
			const TSharedRef<FJsonObject> WitnessIdentity = ProcessIdentity(
				WitnessProcessId,
				WitnessExecutable);
			const FDateTime Now = FDateTime::UtcNow();
			const TSharedRef<FJsonObject> Armed = MakeShared<FJsonObject>();
			Armed->SetStringField(
				TEXT("schema"),
				TEXT("uai.safe_exit.external_witness_armed.v1"));
			Armed->SetStringField(TEXT("launch_id"), LaunchId);
			Armed->SetStringField(TEXT("exit_attempt_id"), ExitAttemptId);
			Armed->SetStringField(TEXT("nonce"), Nonce);
			Armed->SetObjectField(TEXT("editor_process_identity"), EditorIdentity);
			Armed->SetObjectField(TEXT("witness_process_identity"), WitnessIdentity);
			Armed->SetStringField(TEXT("created_at_utc"), Now.ToIso8601());
			Armed->SetStringField(
				TEXT("expires_at_utc"),
				(Now + FTimespan::FromMinutes(1.0)).ToIso8601());

			const TSharedRef<FJsonObject> Heartbeat = MakeShared<FJsonObject>();
			Heartbeat->SetStringField(
				TEXT("schema"),
				TEXT("uai.safe_exit.witness_heartbeat.v1"));
			Heartbeat->SetStringField(TEXT("launch_id"), LaunchId);
			Heartbeat->SetStringField(TEXT("exit_attempt_id"), ExitAttemptId);
			Heartbeat->SetStringField(TEXT("nonce"), Nonce);
			Heartbeat->SetObjectField(
				TEXT("witness_process_identity"),
				WitnessIdentity);
			Heartbeat->SetNumberField(
				TEXT("observed_at_unix_ms"),
				static_cast<double>(Now.ToUnixTimestamp()) * 1000.0);

			if (!WriteJsonObject(
					FPaths::Combine(AttemptRoot, TEXT("armed.json")),
					Armed)
				|| !WriteJsonObject(
					FPaths::Combine(AttemptRoot, TEXT("heartbeat.json")),
					Heartbeat))
			{
				OutError = TEXT("safe_exit_test_witness_write_failed");
				return false;
			}
			return true;
		}

		const FString& GetAttemptRoot() const
		{
			return AttemptRoot;
		}

	private:
		FProcHandle WitnessHandle;
		uint32 WitnessProcessId = 0;
		FString AttemptRoot;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLongRunningSessionSafeExitConfigResourceContractTest,
	"UeAgentInterface.Composition.LongRunningSession.E3."
		"SafeExitConfigResourceContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentLongRunningSessionSafeExitConfigResourceContractTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Composition::LongRunningSession::TestSupport;

	TestTrue(
		TEXT("package object refs and config refs match the public dirty set"),
		SafeExitResourceSetsMatchForTest(
			{
				TEXT("/Game/UAI/SafeExit/BP_Fixture.BP_Fixture"),
				TEXT("config:engine"),
			},
			{
				TEXT("/Game/UAI/SafeExit/BP_Fixture"),
				TEXT("config:engine"),
			}));
	TestFalse(
		TEXT("a missing dirty config prevents exact-set resolution"),
		SafeExitResourceSetsMatchForTest(
			{TEXT("/Game/UAI/SafeExit/BP_Fixture")},
			{
				TEXT("/Game/UAI/SafeExit/BP_Fixture"),
				TEXT("config:engine"),
			}));

	if (!TestNotNull(TEXT("config cache"), GConfig))
	{
		return false;
	}
	FConfigFile* EngineConfig = GConfig->FindConfigFile(GEngineIni);
	FConfigFile* GameConfig = GConfig->FindConfigFile(GGameIni);
	if (!TestNotNull(TEXT("engine config"), EngineConfig)
		|| !TestNotNull(TEXT("game config"), GameConfig))
	{
		return false;
	}
	const bool bEngineWasDirty = EngineConfig->Dirty;
	const bool bGameWasDirty = GameConfig->Dirty;

	EngineConfig->Dirty = true;
	GameConfig->Dirty = true;
	DiscardSafeExitConfigResourcesForTest(
		{TEXT("config:engine")});
	TestFalse(TEXT("discard clears the selected config"), EngineConfig->Dirty);
	TestTrue(TEXT("discard leaves an unselected config dirty"), GameConfig->Dirty);

	EngineConfig->Dirty = bEngineWasDirty;
	GameConfig->Dirty = bGameWasDirty;
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLongRunningSessionSafeExitRequiresArmedWitnessTest,
	"UeAgentInterface.Composition.LongRunningSession.E3."
		"SafeExitRequiresArmedWitness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentLongRunningSessionSafeExitRequiresArmedWitnessTest::RunTest(
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

	TSharedPtr<FJsonObject> RejectResult;
	FString InvokeError;
	if (!TestTrue(
			TEXT("dirty reject reaches the public command edge"),
			TryInvokeCommand(
				Composition->GetRouteHost(),
				Composition->GetConfig(),
				TEXT("editor.safe_exit"),
				TEXT("{\"dirty_resolution\":{\"mode\":\"reject\"," 
					"\"save_resources\":[],\"discard_resources\":[]},"
					"\"external_witness\":{\"schema\":"
					"\"uai.safe_exit.external_witness_ref.v1\","
					"\"launch_id\":\"11111111111111111111111111111111\","
					"\"exit_attempt_id\":\"22222222222222222222222222222222\","
					"\"nonce\":\"3333333333333333333333333333333333333333333333333333333333333333\"}}"),
				RejectResult,
				InvokeError))
		|| !RejectResult.IsValid())
	{
		DirtyPackage->SetDirtyFlag(false);
		AddError(InvokeError);
		return false;
	}
	const TSharedPtr<FJsonObject> RejectRoot =
		RejectResult->GetObjectField(TEXT("payload"));
	if (!TestEqual(
			TEXT("missing witness fails before dirty resolution"),
			OperationErrorCode(RejectRoot),
			FString(TEXT("editor_safe_exit_external_witness_missing"))))
	{
		DirtyPackage->SetDirtyFlag(false);
		return false;
	}
	TestTrue(
		TEXT("witness rejection leaves the dirty package untouched"),
		DirtyPackage->IsDirty());

	DirtyPackage->SetDirtyFlag(false);
	TestFalse(TEXT("fixture cleanup clears the dirty package"),
		DirtyPackage->IsDirty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLongRunningSessionSafeExitRejectsApplicableDirtyTest,
	"UeAgentInterface.Composition.LongRunningSession.E3."
		"SafeExitRejectsApplicableDirtyWithArmedWitness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentLongRunningSessionSafeExitRejectsApplicableDirtyTest::RunTest(
	const FString& Parameters)
{
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
		MakeUnique<FScopedPublicCommandComposition>(TEXT("safe-exit-e3-dirty"));
	FString StartError;
	if (!TestTrue(
			TEXT("public production composition starts"),
			Composition->TryStart(*WorldSettings, StartError)))
	{
		AddError(StartError);
		return false;
	}

	const FString LaunchId = TEXT("44444444444444444444444444444444");
	const FString ExitAttemptId = TEXT("55555555555555555555555555555555");
	const FString Nonce =
		TEXT("6666666666666666666666666666666666666666666666666666666666666666");
	FScopedArmedWitnessFixture Witness;
	FString WitnessError;
	if (!TestTrue(
			TEXT("external witness fixture is armed"),
			Witness.TryArm(LaunchId, ExitAttemptId, Nonce, WitnessError)))
	{
		AddError(WitnessError);
		return false;
	}

	UPackage* DirtyPackage = CreatePackage(
		TEXT("/Game/__UAI_LongRunningSession/SafeExitArmedDirtyFixture"));
	if (!TestNotNull(TEXT("dirty fixture package"), DirtyPackage))
	{
		return false;
	}
	DirtyPackage->SetDirtyFlag(true);
	if (!TestTrue(TEXT("dirty fixture is dirty"), DirtyPackage->IsDirty()))
	{
		return false;
	}

	TSharedPtr<FJsonObject> RejectResult;
	FString InvokeError;
	const FString Params = FString::Printf(
		TEXT("{\"dirty_resolution\":{\"mode\":\"reject\","
			"\"save_resources\":[],\"discard_resources\":[]},"
			"\"external_witness\":{\"schema\":"
			"\"uai.safe_exit.external_witness_ref.v1\","
			"\"launch_id\":\"%s\",\"exit_attempt_id\":\"%s\","
			"\"nonce\":\"%s\"}}"),
		*LaunchId,
		*ExitAttemptId,
		*Nonce);
	if (!TestTrue(
			TEXT("armed dirty reject reaches the public command edge"),
			TryInvokeCommand(
				Composition->GetRouteHost(),
				Composition->GetConfig(),
				TEXT("editor.safe_exit"),
				Params,
				RejectResult,
				InvokeError))
		|| !RejectResult.IsValid())
	{
		DirtyPackage->SetDirtyFlag(false);
		AddError(InvokeError);
		return false;
	}
	const TSharedPtr<FJsonObject> RejectRoot =
		RejectResult->GetObjectField(TEXT("payload"));
	TestEqual(
		TEXT("valid witness rejects an applicable dirty resource"),
		OperationErrorCode(RejectRoot),
		FString(TEXT("editor_safe_exit_dirty_resources_present")));
	TestTrue(
		TEXT("dirty reject leaves the package untouched"),
		DirtyPackage->IsDirty());
	TestFalse(
		TEXT("dirty reject does not publish a pre-exit receipt"),
		FPaths::FileExists(FPaths::Combine(
			Witness.GetAttemptRoot(),
			TEXT("pre_exit_committed.json"))));

	DirtyPackage->SetDirtyFlag(false);
	TestFalse(
		TEXT("fixture cleanup clears the dirty package"),
		DirtyPackage->IsDirty());
	return !HasAnyErrors();
}
