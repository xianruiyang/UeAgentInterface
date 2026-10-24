#include "Misc/AutomationTest.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputTriggers.h"
#include "InputModifiers.h"
#include "Core/Contract/UeAgentCommandJson.h"
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
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
TSharedPtr<FJsonObject> FindMappingProperty(const TSharedPtr<FJsonObject>& Object, const FString& Name)
{
	if (!Object) return nullptr;
	for (const auto& Field : Object->Values)
	{
		if (Field.Value->Type != EJson::Object) continue;
		const auto Child = Field.Value->AsObject();
		if (Field.Key == Name && Child->HasField(TEXT("value"))) return Child;
		if (auto Found = FindMappingProperty(Child, Name)) return Found;
	}
	return nullptr;
}

// Edit a named business field in the exported shape, without decoding hidden
// identities or manufacturing canonical wrappers for the test.
int32 SetMappingField(const TSharedPtr<FJsonValue>& Value, const FString& Name, const TSharedRef<FJsonValue>& Replacement)
{
	if (!Value) return 0;
	int32 Count = 0;
	if (Value->Type == EJson::Array)
		for (const auto& Item : Value->AsArray()) Count += SetMappingField(Item, Name, Replacement);
	else if (Value->Type == EJson::Object)
	{
		auto Object = Value->AsObject();
		for (auto& Field : Object->Values)
		{
			if (Field.Key == Name)
			{
				if (Field.Value->Type == EJson::Object && Field.Value->AsObject()->HasField(TEXT("value")))
					Field.Value->AsObject()->SetField(TEXT("value"), Replacement);
				else Field.Value = Replacement;
				++Count;
			}
			else Count += SetMappingField(Field.Value, Name, Replacement);
		}
	}
	return Count;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208InputMappingContextWorkspaceTest,
	"UAI.Root.T208.InputMappingContext.Workspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208InputMappingContextWorkspaceTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Base = TEXT("/Game/UAI/Transient/T208InputMapping/") + Token;
	const FString PackageName = Base + TEXT("/IMC_Test");
	const FString ObjectPath = PackageName + TEXT(".IMC_Test");
	const FString ActionPackage = Base + TEXT("/IA_Test");
	const FString ActionPath = ActionPackage + TEXT(".IA_Test");
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-input-mapping/native") / Token);
	FString Error;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args)
	{
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
			TEXT("t208-input-mapping"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	};
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
	auto SaveFixture = [&](UObject* Object)
	{
		return UPackage::SavePackage(Object->GetPackage(), Object,
			*FPackageName::LongPackageNameToFilename(Object->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension()), Save);
	};
	ON_SCOPE_EXIT
	{
		for (const auto& Path : {ObjectPath, ActionPath})
		{
			if (auto* Object = FindObject<UObject>(nullptr, *Path); Object && Object->GetPackage()->IsDirty())
				TestTrue(TEXT("settle disposable fixture after assertions"), SaveFixture(Object));
			if (!FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path))) continue;
			auto Delete = MakeShared<FJsonObject>(); Delete->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(Path)});
			Delete->SetBoolField(TEXT("recursive"), false);
			for (bool Confirm : {false, true})
			{
				Delete->SetBoolField(TEXT("confirm"), Confirm);
				auto Result = Submit(TEXT("asset.delete"), Delete);
				if (!Result.IsOk()) AddError(Result.GetErrorJson());
			}
			TestFalse(TEXT("own package removed"), FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path)));
		}
	};
	// Native baseline is fixture setup, not public authoring evidence.
	auto* Action = NewObject<UInputAction>(CreatePackage(*ActionPackage), TEXT("IA_Test"), RF_Public | RF_Standalone);
	auto* Context = NewObject<UInputMappingContext>(CreatePackage(*PackageName), TEXT("IMC_Test"), RF_Public | RF_Standalone);
	Context->ContextDescription = FText::FromString(TEXT("Retained context description"));
	auto& First = Context->MapKey(Action, EKeys::A);
	auto* Hold = NewObject<UInputTriggerHold>(Context, TEXT("RetainedHold")); Hold->HoldTimeThreshold = 0.4f;
	First.Triggers.Add(Hold);
	First.Modifiers.Add(NewObject<UInputModifierNegate>(Context, TEXT("RetainedNegate")));
	Context->MapKey(Action, EKeys::B);
	if (!SaveFixture(Action) || !SaveFixture(Context)) { AddError(TEXT("fixture save failed")); return false; }
	auto Export = [&](const FString& Stage)
	{
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath);
		Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		auto Result = Submit(TEXT("content_graph.export_workspace"), Args);
		if (!Result.IsOk()) AddError(Result.GetErrorJson());
		return Result.IsOk();
	};
	auto Edit = [&](const FString& Stage, TFunctionRef<bool(TArray<TSharedPtr<FJsonValue>>&)> Change)
	{
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false);
		int32 Count = 0;
		for (const auto& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Root;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Root, Error)) { AddError(Error); return false; }
			if (auto Row = FindMappingProperty(Root, TEXT("Mappings")))
			{
				const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
				if (!Row->TryGetArrayField(TEXT("value"), Values)) { AddError(TEXT("Mappings must export as an ordered value array")); return false; }
				auto Items = *Values;
				if (!Change(Items)) return false;
				Row->SetArrayField(TEXT("value"), Items); ++Count;
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(), Text, Error)
					|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { AddError(Error); return false; }
			}
		}
		return TestEqual(TEXT("one ordinary Mappings field"), Count, 1);
	};
	auto Import = [&](const FString& Stage)
	{
		auto Args = MakeShared<FJsonObject>(); auto Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage);
		Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
		return Submit(TEXT("content_graph.import_workspace"), Args);
	};
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	TArray<uint8> BeforeInvalid, AfterInvalid;
	if (!FFileHelper::LoadFileToArray(BeforeInvalid, *Filename) || !Export(TEXT("invalid"))
		|| !Edit(TEXT("invalid"), [&](TArray<TSharedPtr<FJsonValue>>& Items)
		{
			return Items.Num() == 2 && SetMappingField(Items[0], TEXT("Action"),
				MakeShared<FJsonValueString>(Base + TEXT("/Missing.Missing"))) == 1;
		})) return false;
	const auto Rejected = Import(TEXT("invalid"));
	TestFalse(TEXT("missing Action reference rejects before mutation"), Rejected.IsOk());
	if (!TestTrue(TEXT("invalid reference preserves native order owned state and disk"), Context->GetMappings().Num() == 2
		&& Context->GetMapping(0).Key == EKeys::A && Context->GetMapping(0).Triggers[0] == Hold
		&& Hold->HoldTimeThreshold == 0.4f && !Context->GetPackage()->IsDirty()
		&& FFileHelper::LoadFileToArray(AfterInvalid, *Filename) && BeforeInvalid == AfterInvalid)) return false;
	for (const FString& Stage : {FString(TEXT("unknown-key")), FString(TEXT("duplicate-key"))})
	{
		if (!Export(Stage) || !Edit(Stage, [&](TArray<TSharedPtr<FJsonValue>>& Items)
		{
			if (!TestTrue(TEXT("ordinary mappings carry captured structured keys"), Items.Num() == 2
				&& Items[0]->AsObject()->GetStringField(TEXT("instance")) == TEXT("Entry1")
				&& Items[1]->AsObject()->GetStringField(TEXT("instance")) == TEXT("Entry2"))) return false;
			Items[1]->AsObject()->SetStringField(TEXT("instance"), Stage == TEXT("unknown-key") ? TEXT("Entry99") : TEXT("Entry1"));
			return true;
		})) return false;
		const auto InvalidKey = Import(Stage);
		if (!TestTrue(TEXT("structured identity rejects before native mutation"), !InvalidKey.IsOk()
			&& InvalidKey.GetErrorJson().Contains(Stage == TEXT("unknown-key") ? TEXT("structured_item_key_unknown") : TEXT("structured_item_key_duplicate")))
			|| !TestTrue(TEXT("invalid structured identity preserves live and saved state"), Context->GetMappings().Num() == 2
				&& Context->GetMapping(0).Key == EKeys::A && Context->GetMapping(0).Triggers[0] == Hold
				&& Hold->HoldTimeThreshold == 0.4f && !Context->GetPackage()->IsDirty()
				&& FFileHelper::LoadFileToArray(AfterInvalid, *Filename) && BeforeInvalid == AfterInvalid)) return false;
	}
	if (!Export(TEXT("edit")) || !Edit(TEXT("edit"), [&](TArray<TSharedPtr<FJsonValue>>& Items)
	{
		if (Items.Num() != 2) return false;
		Items.Swap(0, 1);
		return TestEqual(TEXT("editable nested trigger field"), SetMappingField(Items[1], TEXT("HoldTimeThreshold"), MakeShared<FJsonValueNumber>(0.75)), 1)
			&& TestEqual(TEXT("editable nested key field"), SetMappingField(Items[0], TEXT("KeyName"), MakeShared<FJsonValueString>(TEXT("Gamepad_LeftX"))), 1);
	})) return false;
	Context = nullptr;
	auto Result = Import(TEXT("edit"));
	if (!TestTrue(TEXT("public ordered mappings edit"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
	Context = LoadObject<UInputMappingContext>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("real mappings persist"), Context && Context->GetMappings().Num() == 2)) return false;
	const auto& Mappings = Context->GetMappings();
	TestTrue(TEXT("mapping order reference key and native key details"), Mappings[0].Key == EKeys::Gamepad_LeftX && Mappings[0].Key.IsGamepadKey()
		&& Mappings[1].Key == EKeys::A && Mappings[0].Action->GetPathName() == ActionPath && Mappings[1].Action->GetPathName() == ActionPath);
	TestTrue(TEXT("owned trigger modifier identity and non-target root preserved"), Mappings[1].Triggers.Num() == 1
		&& Mappings[1].Triggers[0]->GetFName() == TEXT("RetainedHold") && Mappings[1].Triggers[0]->GetOuter() == Context
		&& Cast<UInputTriggerHold>(Mappings[1].Triggers[0]) && Cast<UInputTriggerHold>(Mappings[1].Triggers[0])->HoldTimeThreshold == 0.75f
		&& Mappings[1].Modifiers.Num() == 1 && Mappings[1].Modifiers[0]->GetFName() == TEXT("RetainedNegate")
		&& Mappings[1].Modifiers[0]->GetOuter() == Context && Cast<UInputModifierNegate>(Mappings[1].Modifiers[0])
		&& Context->ContextDescription.ToString() == TEXT("Retained context description"));
	auto CandidateArgs = MakeShared<FJsonObject>(); CandidateArgs->SetStringField(TEXT("target"), ObjectPath);
	auto CandidateInput = MakeShared<FJsonObject>(); CandidateInput->SetStringField(TEXT("property_path"), TEXT("Mappings[0].Triggers"));
	CandidateInput->SetStringField(TEXT("query"), UInputTriggerHold::StaticClass()->GetDisplayNameText().ToString());
	CandidateArgs->SetObjectField(TEXT("input"), CandidateInput);
	Result = Submit(TEXT("asset.owned_candidates"), CandidateArgs);
	TSharedPtr<FJsonObject> CandidatePayload;
	if (!TestTrue(TEXT("nested native owned candidate query"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	if (!CommandContract::Json::TryParseObject(Result.GetPayloadJson(), CandidatePayload, Error)) { AddError(Error); return false; }
	const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
	if (!TestTrue(TEXT("returned trigger candidates"), CandidatePayload->TryGetArrayField(TEXT("names"), Names))) return false;
	FString CandidateName;
	for (const auto& Name : *Names)
		if (Name->AsString() == UInputTriggerHold::StaticClass()->GetDisplayNameText().ToString()) CandidateName = Name->AsString();
	if (!TestFalse(TEXT("exact native candidate present"), CandidateName.IsEmpty())) return false;
	if (!TestTrue(TEXT("candidate query preserves real context"), Context->GetMappings().Num() == 2 && Context->GetMapping(0).Triggers.IsEmpty()
		&& !Context->GetPackage()->IsDirty())) return false;
	if (!Export(TEXT("append")) || !Edit(TEXT("append"), [&](TArray<TSharedPtr<FJsonValue>>& Items)
	{
		// This shape is taken from the ordinary exported mapping, not a native ID.
		auto KeyFields = MakeShared<FJsonObject>(); KeyFields->SetStringField(TEXT("KeyName"), TEXT("C"));
		auto Key = MakeShared<FJsonObject>(); Key->SetObjectField(TEXT("fields"), KeyFields);
		auto Fields = MakeShared<FJsonObject>(); Fields->SetStringField(TEXT("Action"), ActionPath);
		Fields->SetObjectField(TEXT("Key"), Key); Fields->SetArrayField(TEXT("Modifiers"), {});
		auto Threshold = MakeShared<FJsonObject>(); Threshold->SetStringField(TEXT("type"), TEXT("float")); Threshold->SetNumberField(TEXT("value"), 0.6);
		auto TriggerFields = MakeShared<FJsonObject>(); TriggerFields->SetObjectField(TEXT("HoldTimeThreshold"), Threshold);
		auto TriggerProperties = MakeShared<FJsonObject>(); TriggerProperties->SetObjectField(TEXT("Trigger Settings"), TriggerFields);
		auto Owned = MakeShared<FJsonObject>(); Owned->SetStringField(TEXT("name"), CandidateName); Owned->SetObjectField(TEXT("properties"), TriggerProperties);
		auto Trigger = MakeShared<FJsonObject>(); Trigger->SetObjectField(TEXT("owned_object"), Owned);
		Fields->SetArrayField(TEXT("Triggers"), {MakeShared<FJsonValueObject>(Trigger)}); Fields->SetField(TEXT("PlayerMappableKeySettings"), MakeShared<FJsonValueNull>());
		Fields->SetStringField(TEXT("SettingBehavior"), TEXT("InheritSettingsFromAction"));
		auto Item = MakeShared<FJsonObject>(); Item->SetObjectField(TEXT("fields"), Fields);
		Items.Add(MakeShared<FJsonValueObject>(Item)); return true;
	})) return false;
	Context = nullptr; Result = Import(TEXT("append"));
	if (!TestTrue(TEXT("public mapping creation from ordinary fields"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
	Context = LoadObject<UInputMappingContext>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("new mapping preserves preceding entries"), Context && Context->GetMappings().Num() == 3
		&& Context->GetMapping(2).Key == EKeys::C && Context->GetMapping(2).Action->GetPathName() == ActionPath
		&& Context->GetMapping(1).Triggers[0]->GetFName() == TEXT("RetainedHold"))) return false;
	const auto& NewMapping = Context->GetMapping(2);
	if (!TestTrue(TEXT("new mapping creates native owned trigger from returned candidate"), NewMapping.Triggers.Num() == 1
		&& NewMapping.Triggers[0]->GetOuter() == Context && Cast<UInputTriggerHold>(NewMapping.Triggers[0])
		&& FMath::IsNearlyEqual(Cast<UInputTriggerHold>(NewMapping.Triggers[0])->HoldTimeThreshold, 0.6f))) return false;
	if (!Export(TEXT("remove")) || !Edit(TEXT("remove"), [](TArray<TSharedPtr<FJsonValue>>& Items)
	{
		Items.RemoveAt(2); Items.RemoveAt(0);
		// Sparse edits preserve captured descendants even when their parent moves.
		Items[0]->AsObject()->GetObjectField(TEXT("fields"))->RemoveField(TEXT("Triggers"));
		Items[0]->AsObject()->GetObjectField(TEXT("fields"))->RemoveField(TEXT("Modifiers"));
		return true;
	})) return false;
	Context = nullptr; Result = Import(TEXT("remove"));
	if (!TestTrue(TEXT("public mapping removal"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
	Context = LoadObject<UInputMappingContext>(nullptr, *ObjectPath);
	TestTrue(TEXT("surviving mapping retains owned content"), Context && Context->GetMappings().Num() == 1
		&& Context->GetMapping(0).Key == EKeys::A && Context->GetMapping(0).Triggers[0]->GetFName() == TEXT("RetainedHold"));
	if (!Export(TEXT("clear")) || !Edit(TEXT("clear"), [](TArray<TSharedPtr<FJsonValue>>& Items) { Items.Reset(); return true; })) return false;
	Context = nullptr; Result = Import(TEXT("clear"));
	if (!TestTrue(TEXT("public explicit empty mappings"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
	Context = LoadObject<UInputMappingContext>(nullptr, *ObjectPath);
	TestTrue(TEXT("empty mappings persist"), Context && Context->GetMappings().IsEmpty());
	TestTrue(TEXT("independent final workspace export"), Export(TEXT("readback")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208InputMappingKeyFinalizationTest,
	"UAI.Root.T208.InputMappingContext.NativeKeyFinalization", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208InputMappingKeyFinalizationTest::RunTest(const FString&)
{
	const auto* KeyProperty = FindFProperty<FStructProperty>(FEnhancedActionKeyMapping::StaticStruct(), TEXT("Key"));
	if (!TestNotNull(TEXT("native key type"), KeyProperty)) return false;
	FKey Key = EKeys::A;
	TestFalse(TEXT("baseline keyboard details cached"), Key.IsGamepadKey());
	const FKey Desired = EKeys::Gamepad_LeftX;
	const auto Value = UeAgentPropertyValueCodec::EncodeStructValue(KeyProperty->Struct, &Desired, nullptr, {});
	FString Error;
	if (!TestTrue(TEXT("shared struct codec decodes key"), UeAgentPropertyValueCodec::DecodeStructValue(KeyProperty->Struct, Value, &Key, Error)))
	{ AddError(Error); return false; }
	TestTrue(TEXT("key name and rebuilt native details agree before reload"), Key == Desired && Key.IsGamepadKey() && Key.IsAxis1D());
	return !HasAnyErrors();
}
