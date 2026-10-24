#include "Misc/AutomationTest.h"
#include "MediaPlayer.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
TSharedPtr<FJsonObject> FindMediaPlayerProperty(const TSharedPtr<FJsonObject>& Object, const FString& Name)
{
	if (!Object) return nullptr;
	for (const auto& Field : Object->Values)
	{
		if (Field.Value->Type != EJson::Object) continue;
		const auto Child = Field.Value->AsObject();
		if (Field.Key == Name && Child->HasField(TEXT("value"))) return Child;
		if (auto Found = FindMediaPlayerProperty(Child, Name)) return Found;
	}
	return nullptr;
}

TMap<FString, FString> MediaPlayerNonTargetState(UMediaPlayer& Player)
{
	TMap<FString, FString> State;
	for (TFieldIterator<FProperty> It(Player.GetClass()); It; ++It)
	{
		if (!It->HasAnyPropertyFlags(CPF_Edit) || It->GetName() == TEXT("PlayOnOpen") || It->GetName() == TEXT("Loop")) continue;
		FString Value;
		It->ExportText_InContainer(0, Value, &Player, &Player, &Player, PPF_None);
		State.Add(It->GetName(), Value);
	}
	return State;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208MediaPlayerRootWorkspaceTest,
	"UAI.Root.T208.MediaPlayer.RootWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208MediaPlayerRootWorkspaceTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T208MediaPlayer/MP_") + Token;
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-media-player/native") / Token);
	FString Error;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args)
	{
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
			TEXT("t208-media-player"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	};
	// Native setup is only a fixture. The behavior under test is public workspace editing.
	auto* Player = NewObject<UMediaPlayer>(CreatePackage(*PackageName), *FPackageName::GetLongPackageAssetName(PackageName), RF_Public | RF_Standalone);
	Player->PlayOnOpen = false;
	Player->SetLooping(false);
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
	ON_SCOPE_EXIT
	{
		if (auto* Current = FindObject<UMediaPlayer>(nullptr, *ObjectPath); Current && Current->GetPackage()->IsDirty())
			TestTrue(TEXT("settle only this fixture after assertions"), UPackage::SavePackage(Current->GetPackage(), Current, *Filename, Save));
		if (FPackageName::DoesPackageExist(PackageName))
		{
			auto Delete = MakeShared<FJsonObject>();
			Delete->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(ObjectPath)});
			Delete->SetBoolField(TEXT("recursive"), false);
			for (bool Confirm : {false, true})
			{
				Delete->SetBoolField(TEXT("confirm"), Confirm);
				const auto Result = Submit(TEXT("asset.delete"), Delete);
				if (!Result.IsOk()) AddError(Result.GetErrorJson());
			}
			TestFalse(TEXT("own fixture package removed"), FPackageName::DoesPackageExist(PackageName));
		}
	};
	if (!TestTrue(TEXT("native fixture saved"), UPackage::SavePackage(Player->GetPackage(), Player, *Filename, Save))) return false;
	const auto NonTarget = MediaPlayerNonTargetState(*Player);
	const auto* Loop = FindFProperty<FBoolProperty>(Player->GetClass(), TEXT("Loop"));
	if (!TestNotNull(TEXT("native Loop property"), Loop)) return false;
	auto Export = [&](const FString& Stage)
	{
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath);
		Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Result = Submit(TEXT("content_graph.export_workspace"), Args);
		if (!Result.IsOk()) AddError(Result.GetErrorJson());
		return Result.IsOk();
	};
	auto Edit = [&](const FString& Stage, const FString& Name, const TSharedRef<FJsonValue>& Value)
	{
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false);
		int32 Count = 0;
		for (const auto& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Root;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Root, Error)) { AddError(Error); return false; }
			if (auto Row = FindMediaPlayerProperty(Root, Name))
			{
				Row->SetField(TEXT("value"), Value); ++Count;
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(), Text, Error)
					|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { AddError(Error); return false; }
			}
		}
		return TestEqual(TEXT("one editable property row"), Count, 1);
	};
	auto Import = [&](const FString& Stage)
	{
		auto Args = MakeShared<FJsonObject>(); auto Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage);
		Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
		return Submit(TEXT("content_graph.import_workspace"), Args);
	};
	for (bool Enabled : {true, false})
	{
		const FString Stage = Enabled ? TEXT("enabled") : TEXT("disabled");
		if (!Export(Stage) || !Edit(Stage, TEXT("PlayOnOpen"), MakeShared<FJsonValueBoolean>(Enabled))
			|| !Edit(Stage, TEXT("Loop"), MakeShared<FJsonValueBoolean>(Enabled))) return false;
		bool SawLoopNotification = false;
		const auto Notification = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda(
			[&](UObject* Object, FPropertyChangedEvent& Event)
			{
				if (Object && Object->GetPathName() == ObjectPath && Event.Property
					&& Event.Property->GetFName() == TEXT("Loop"))
					SawLoopNotification |= Loop->GetPropertyValue_InContainer(Object) == Enabled;
			});
		Player = nullptr;
		const auto Result = Import(Stage);
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(Notification);
		if (!TestTrue(TEXT("public MediaPlayer workspace import"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
		TestTrue(TEXT("real target receives Loop property notification for native setter"), SawLoopNotification);
		if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
		Player = LoadObject<UMediaPlayer>(nullptr, *ObjectPath);
		if (!TestTrue(TEXT("settings persist without changing other native fields"), Player && Player->PlayOnOpen == Enabled
			&& Loop->GetPropertyValue_InContainer(Player) == Enabled && !Player->GetPackage()->IsDirty()
			&& MediaPlayerNonTargetState(*Player).OrderIndependentCompareEqual(NonTarget))) return false;
	}
	if (!Export(TEXT("invalid"))) return false;
	TArray<uint8> Before, After;
	if (!FFileHelper::LoadFileToArray(Before, *Filename)) return false;
	if (!Edit(TEXT("invalid"), TEXT("Loop"), MakeShared<FJsonValueString>(TEXT("not-a-boolean")))) return false;
	const auto Rejected = Import(TEXT("invalid"));
	TestFalse(TEXT("invalid boolean rejected before mutation"), Rejected.IsOk());
	TestTrue(TEXT("invalid input preserves memory and original disk"), !Player->PlayOnOpen
		&& !Loop->GetPropertyValue_InContainer(Player) && !Player->GetPackage()->IsDirty()
		&& MediaPlayerNonTargetState(*Player).OrderIndependentCompareEqual(NonTarget)
		&& FFileHelper::LoadFileToArray(After, *Filename) && !Before.IsEmpty() && Before == After);
	TestTrue(TEXT("independent final workspace export"), Export(TEXT("readback")));
	return !HasAnyErrors();
}
