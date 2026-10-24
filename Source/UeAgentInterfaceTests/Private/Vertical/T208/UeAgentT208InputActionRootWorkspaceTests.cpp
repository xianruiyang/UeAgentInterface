#include "Misc/AutomationTest.h"
#include "InputAction.h"
#include "InputTriggers.h"
#include "InputModifiers.h"
#include "Misc/DataValidation.h"
#include "PlayerMappableKeySettings.h"
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
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
TSharedPtr<FJsonObject> FindInputRow(const TSharedPtr<FJsonObject>& Object, const FString& Name)
{
	if (!Object) return nullptr;
	for (const auto& Field : Object->Values)
	{
		if (Field.Value->Type == EJson::Object)
		{
			const auto Child = Field.Value->AsObject();
			if (Field.Key == Name && Child->HasField(TEXT("value"))) return Child;
			if (auto Row = FindInputRow(Child, Name)) return Row;
		}
		if (Field.Value->Type == EJson::Array)
			for (const auto& Item : Field.Value->AsArray())
				if (Item->Type == EJson::Object) if (auto Row = FindInputRow(Item->AsObject(), Name)) return Row;
	}
	return nullptr;
}
TMap<FString, FString> OtherInputState(UInputAction& Action, const FString& Excluded = TEXT("ValueType"))
{
	TMap<FString, FString> Result;
	for (TFieldIterator<FProperty> It(Action.GetClass()); It; ++It)
	{
		if (!It->HasAnyPropertyFlags(CPF_Edit) || It->GetName() == Excluded) continue;
		FString Value;
		It->ExportText_InContainer(0, Value, &Action, &Action, &Action, PPF_None);
		Result.Add(It->GetName(), Value);
	}
	return Result;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208InputActionRootWorkspaceTest,
	"UAI.Root.T208.InputAction.RootWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
static bool RunInputActionWorkspace(FAutomationTestBase& Test, bool bOwned, bool bArrays = false)
{
	auto TestTrue = [&](const TCHAR* What, bool Value) { return Test.TestTrue(What, Value); };
	auto TestFalse = [&](const TCHAR* What, bool Value) { return Test.TestFalse(What, Value); };
	auto TestNotNull = [&](const TCHAR* What, const void* Value) { return Test.TestNotNull(What, Value); };
	auto AddError = [&](const FString& Message) { Test.AddError(Message); };
	using namespace UE::UAI;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T208InputActionRoot/IA_") + Token;
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString Directory = FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T208/InputAction") / Token;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args)
	{
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
			TEXT("t208-input-action"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	};
	ON_SCOPE_EXIT
	{
		if (FPackageName::DoesPackageExist(PackageName) || FindObject<UInputAction>(nullptr, *ObjectPath))
		{
			// Recovery assertions have finished. Save only this disposable fixture
			// so ordinary destructive preflight can delete it without bypasses.
			if (auto* Owned = FindObject<UInputAction>(nullptr, *ObjectPath); Owned && Owned->GetPackage()->IsDirty())
			{
				FSavePackageArgs SaveArgs; SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
				SaveArgs.SaveFlags = SAVE_NoError;
				TestTrue(TEXT("self fixture settles for public cleanup"), UPackage::SavePackage(Owned->GetPackage(), Owned, *Filename, SaveArgs));
			}
			auto Delete = MakeShared<FJsonObject>();
			Delete->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(ObjectPath)});
			Delete->SetBoolField(TEXT("recursive"), false);
			for (bool Confirm : {false, true})
			{
				Delete->SetBoolField(TEXT("confirm"), Confirm);
				auto Result = Submit(TEXT("asset.delete"), Delete);
				if (!Result.IsOk()) AddError(Result.GetErrorJson());
			}
		}
		TestFalse(TEXT("self fixture removed"), FPackageName::DoesPackageExist(PackageName));
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	auto Create = MakeShared<FJsonObject>();
	auto Factory = MakeShared<FJsonObject>();
	auto Select = MakeShared<FJsonObject>();
	Select->SetStringField(TEXT("creation"), TEXT("constructor"));
	Factory->SetStringField(TEXT("name"), TEXT("Input Action")); Factory->SetObjectField(TEXT("select"), Select);
	Create->SetObjectField(TEXT("factory"), Factory); Create->SetStringField(TEXT("target_ref"), ObjectPath);
	auto Result = Submit(TEXT("asset.object.create"), Create);
	if (!TestTrue(TEXT("public factory creates InputAction"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	UInputAction* Action = LoadObject<UInputAction>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("real native InputAction exists"), Action)) return false;
	FString PropertyName = bArrays ? TEXT("Triggers") : bOwned ? TEXT("PlayerMappableKeySettings") : TEXT("ValueType");
	const auto NonTarget = OtherInputState(*Action, PropertyName);
	auto Export = [&](const FString& Stage)
	{
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath);
		Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		auto Exported = Submit(TEXT("content_graph.export_workspace"), Args);
		if (!Exported.IsOk()) AddError(Exported.GetErrorJson());
		return Exported.IsOk();
	};
	auto MutateRow = [&](const FString& Stage, TFunctionRef<void(const TSharedPtr<FJsonObject>&)> Mutate)
	{
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false);
		int32 Changed = 0;
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Root;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Root, Error)) return false;
			if (auto Row = FindInputRow(Root, PropertyName))
			{
				Mutate(Row); ++Changed;
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(), Text, Error)
					|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
			}
		}
		if (Changed != 1) AddError(FString::Printf(TEXT("ordinary ValueType row count=%d in %s"), Changed, *Stage));
		return Changed == 1;
	};
	auto EditValue = [&](const FString& Stage, const TSharedRef<FJsonValue>& Value)
	{ return MutateRow(Stage, [&](const TSharedPtr<FJsonObject>& Row) { Row->SetField(TEXT("value"), Value); }); };
	auto Edit = [&](const FString& Stage, const FString& Value) { return EditValue(Stage, MakeShared<FJsonValueString>(Value)); };
	auto Import = [&](const FString& Stage)
	{
		auto Args = MakeShared<FJsonObject>(); auto Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage);
		Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
		return Submit(TEXT("content_graph.import_workspace"), Args);
	};
	if (bArrays)
	{
		// Native seed establishes non-target instances; their initialization is not
		// evidence of public candidate creation.
		auto* Settings = NewObject<UPlayerMappableKeySettings>(Action, TEXT("ExistingSettings"));
		Settings->Name = FName(*Token); Settings->DisplayName = FText::FromString(TEXT("Existing mapping"));
		FindFProperty<FObjectPropertyBase>(Action->GetClass(), TEXT("PlayerMappableKeySettings"))
			->SetObjectPropertyValue_InContainer(Action, Settings);
		auto* Down = NewObject<UInputTriggerDown>(Action, TEXT("ExistingDown")); Down->ActuationThreshold = 0.4f;
		Action->Triggers.Add(Down);
		Action->Modifiers.Add(NewObject<UInputModifierNegate>(Action, TEXT("ExistingNegate")));
		FSavePackageArgs SaveArgs; SaveArgs.TopLevelFlags = RF_Public | RF_Standalone; SaveArgs.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Action->GetPackage(), Action, *Filename, SaveArgs)) return false;
		auto QueryName = [&](UClass* Class)
		{
			auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath);
			auto Input = MakeShared<FJsonObject>(); Input->SetStringField(TEXT("property_path"), PropertyName);
			Input->SetStringField(TEXT("query"), Class->GetDisplayNameText().ToString()); Args->SetObjectField(TEXT("input"), Input);
			auto Found = Submit(TEXT("asset.owned_candidates"), Args);
			TSharedPtr<FJsonObject> Payload;
			if (!Found.IsOk() || !CommandContract::Json::TryParseObject(Found.GetPayloadJson(), Payload, Error))
			{ AddError(Found.GetErrorJson()); return FString(); }
			for (const auto& Name : Payload->GetArrayField(TEXT("names")))
				if (Name->AsString() == Class->GetDisplayNameText().ToString()) return Name->AsString();
			AddError(TEXT("native class readable name absent from candidate response")); return FString();
		};
		auto Append = [&](const FString& Stage, const FString& Name, const FString& Category, const FString& Field, double Number)
		{
			return MutateRow(Stage, [&](const TSharedPtr<FJsonObject>& Row)
			{
				auto Items = Row->GetArrayField(TEXT("value"));
				auto Value = MakeShared<FJsonObject>(); Value->SetStringField(TEXT("type"), TEXT("float")); Value->SetNumberField(TEXT("value"), Number);
				auto Fields = MakeShared<FJsonObject>(); Fields->SetObjectField(Field, Value);
				auto Properties = MakeShared<FJsonObject>(); Properties->SetObjectField(Category, Fields);
				auto Owned = MakeShared<FJsonObject>(); Owned->SetStringField(TEXT("name"), Name); Owned->SetObjectField(TEXT("properties"), Properties);
				auto Item = MakeShared<FJsonObject>(); Item->SetObjectField(TEXT("owned_object"), Owned);
				Items.Add(MakeShared<FJsonValueObject>(Item)); Row->SetArrayField(TEXT("value"), Items);
			});
		};
		const FString HoldName = QueryName(UInputTriggerHold::StaticClass()); if (HoldName.IsEmpty()) return false;
		TArray<uint8> OriginalDisk, CurrentDisk; FFileHelper::LoadFileToArray(OriginalDisk, *Filename);
		const auto SeedOther = OtherInputState(*Action, PropertyName);
		if (!Export(TEXT("badtrigger")) || !Append(TEXT("badtrigger"), TEXT("MissingTriggerClass"), TEXT("Trigger Settings"), TEXT("HoldTimeThreshold"), 0.75)) return false;
		Result = Import(TEXT("badtrigger")); TestFalse(TEXT("invalid trigger candidate rejects"), Result.IsOk());
		FFileHelper::LoadFileToArray(CurrentDisk, *Filename);
		TestTrue(TEXT("invalid trigger does not write"), OriginalDisk == CurrentDisk && Action->Triggers.Num() == 1
			&& !Action->GetPackage()->IsDirty() && OtherInputState(*Action, PropertyName).OrderIndependentCompareEqual(SeedOther));
		Action->Modify(); Action->ValueType = EInputActionValueType::Axis2D; Action->MarkPackageDirty();
		const auto DirtyOther = OtherInputState(*Action, PropertyName);
		if (!Export(TEXT("triggerlate")) || !Append(TEXT("triggerlate"), HoldName, TEXT("Trigger Settings"), TEXT("HoldTimeThreshold"), 0.75)) return false;
		bool SawPublished = false, SawRealTriggerNotification = false;
		TWeakObjectPtr<UInputAction> NotificationTarget = Action;
		Action->OnTriggersChanged.AddLambda([&]() { auto* Real = NotificationTarget.Get();
			SawRealTriggerNotification |= Real && Real->GetPathName() == ObjectPath && Real->Triggers.Num() == 2; });
		const auto Loaded = FCoreUObjectDelegates::OnAssetLoaded.AddLambda([&](UObject* Object)
		{
			auto* Published = Cast<UInputAction>(Object);
			if (SawPublished || !Published || Published->GetPathName() != ObjectPath || Published->Triggers.Num() != 2) return;
			auto* Hold = Cast<UInputTriggerHold>(Published->Triggers[1]); TArray<uint8> Bytes;
			SawPublished = Hold && Hold->GetOuter() == Published && Hold->HoldTimeThreshold == 0.75f
				&& !Published->GetPackage()->IsDirty() && OtherInputState(*Published, PropertyName).OrderIndependentCompareEqual(DirtyOther)
				&& FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes != OriginalDisk;
			if (SawPublished) Hold->HoldTimeThreshold = 0.91f;
		});
		Action = nullptr; Result = Import(TEXT("triggerlate")); FCoreUObjectDelegates::OnAssetLoaded.Remove(Loaded);
		TestFalse(TEXT("published trigger mismatch rejects late"), Result.IsOk());
		TestTrue(TEXT("real target PostEdit and persistent new array observed before rejection"), SawPublished && SawRealTriggerNotification);
		Action = LoadObject<UInputAction>(nullptr, *ObjectPath); CurrentDisk.Reset(); FFileHelper::LoadFileToArray(CurrentDisk, *Filename);
		TestTrue(TEXT("array late recovery restores dirty memory and independent original disk"), Action && Action->GetPackage()->IsDirty()
			&& Action->Triggers.Num() == 1 && Action->Triggers[0]->GetFName() == TEXT("ExistingDown")
			&& OtherInputState(*Action, PropertyName).OrderIndependentCompareEqual(DirtyOther) && OriginalDisk == CurrentDisk);
		if (!Export(TEXT("trigger")) || !Append(TEXT("trigger"), HoldName, TEXT("Trigger Settings"), TEXT("HoldTimeThreshold"), 0.75)) return false;
		Action = nullptr; Result = Import(TEXT("trigger"));
		if (!TestTrue(TEXT("ordinary Trigger creation succeeds"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
		if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
		Action = LoadObject<UInputAction>(nullptr, *ObjectPath);
		if (!TestTrue(TEXT("Trigger and retained native identity persist"), Action && Action->Triggers.Num() == 2
			&& Action->Triggers[0]->GetFName() == TEXT("ExistingDown") && Cast<UInputTriggerDown>(Action->Triggers[0]) && Cast<UInputTriggerDown>(Action->Triggers[0])->ActuationThreshold == 0.4f
			&& Cast<UInputTriggerHold>(Action->Triggers[1]) && Cast<UInputTriggerHold>(Action->Triggers[1])->HoldTimeThreshold == 0.75f
			&& OtherInputState(*Action, PropertyName).OrderIndependentCompareEqual(DirtyOther))) return false;
		PropertyName = TEXT("Modifiers");
		const auto BeforeModifiers = OtherInputState(*Action, PropertyName);
		const FString DeadZoneName = QueryName(UInputModifierDeadZone::StaticClass()); if (DeadZoneName.IsEmpty()) return false;
		if (!Export(TEXT("modifier")) || !Append(TEXT("modifier"), DeadZoneName, TEXT("Settings"), TEXT("LowerThreshold"), 0.25)) return false;
		bool SawRealModifierNotification = false; NotificationTarget = Action;
		Action->OnModifiersChanged.AddLambda([&]() { auto* Real = NotificationTarget.Get();
			SawRealModifierNotification |= Real && Real->GetPathName() == ObjectPath && Real->Modifiers.Num() == 2; });
		Action = nullptr; Result = Import(TEXT("modifier"));
		if (!TestTrue(TEXT("ordinary Modifier creation and real target notification"), Result.IsOk() && SawRealModifierNotification)) { AddError(Result.GetErrorJson()); return false; }
		if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
		Action = LoadObject<UInputAction>(nullptr, *ObjectPath);
		if (!TestTrue(TEXT("new modifier native array is available"), Action && Action->Modifiers.Num() == 2 && Action->Modifiers[1])) return false;
		const FName AddedIdentity = Action->Modifiers[1]->GetFName();
		if (!Export(TEXT("modifieredit")) || !MutateRow(TEXT("modifieredit"), [&](const TSharedPtr<FJsonObject>& Row) {
			auto Lower = FindInputRow(Row, TEXT("LowerThreshold")); if (Lower) Lower->SetNumberField(TEXT("value"), 0.35); else AddError(TEXT("ordinary modifier field missing"));
		})) return false;
		Action = nullptr; Result = Import(TEXT("modifieredit"));
		if (!TestTrue(TEXT("ordinary existing Modifier field edit succeeds"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
		if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
		Action = LoadObject<UInputAction>(nullptr, *ObjectPath);
		TestTrue(TEXT("modifier order identity edited value and non-target persist"), Action && Action->Modifiers.Num() == 2
			&& Action->Modifiers[0]->GetFName() == TEXT("ExistingNegate") && Action->Modifiers[1]->GetFName() == AddedIdentity
			&& Cast<UInputModifierDeadZone>(Action->Modifiers[1]) && Cast<UInputModifierDeadZone>(Action->Modifiers[1])->LowerThreshold == 0.35f
			&& OtherInputState(*Action, PropertyName).OrderIndependentCompareEqual(BeforeModifiers));
		FDataValidationContext Validation(false, EDataValidationUsecase::Manual, {});
		TestTrue(TEXT("native validation is explicitly observed for this non-combo fixture"), Action->IsDataValid(Validation) == EDataValidationResult::Valid);
		TestTrue(TEXT("independent final array workspace export"), Export(TEXT("arrayreadback")));
		return !Test.HasAnyErrors();
	}
	if (bOwned)
	{
		auto OwnedValue = [](const FString& Name, const FString& Display, bool bInvalidSelect = false)
		{
			auto Owned = MakeShared<FJsonObject>(); Owned->SetStringField(TEXT("name"), Name);
			if (bInvalidSelect) { auto Select = MakeShared<FJsonObject>(); Select->SetStringField(TEXT("source"), TEXT("MissingSource")); Owned->SetObjectField(TEXT("select"), Select); }
			auto Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("type"), TEXT("text")); Row->SetStringField(TEXT("value"), Display);
			auto Settings = MakeShared<FJsonObject>(); Settings->SetObjectField(TEXT("DisplayName"), Row);
			auto Properties = MakeShared<FJsonObject>(); Properties->SetObjectField(TEXT("Settings"), Settings);
			Owned->SetObjectField(TEXT("properties"), Properties);
			auto Value = MakeShared<FJsonObject>(); Value->SetObjectField(TEXT("owned_object"), Owned);
			return MakeShared<FJsonValueObject>(Value);
		};
		auto Query = MakeShared<FJsonObject>(); Query->SetStringField(TEXT("target"), ObjectPath);
		auto QueryInput = MakeShared<FJsonObject>(); QueryInput->SetStringField(TEXT("property_path"), PropertyName);
		QueryInput->SetStringField(TEXT("query"), TEXT("Player Mappable Key Settings")); Query->SetObjectField(TEXT("input"), QueryInput);
		Result = Submit(TEXT("asset.owned_candidates"), Query);
		if (!TestTrue(TEXT("direct owned candidates use public query"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
		TSharedPtr<FJsonObject> CandidatePayload;
		if (!CommandContract::Json::TryParseObject(Result.GetPayloadJson(), CandidatePayload, Error)) { AddError(Error); return false; }
		const auto& CandidateNames = CandidatePayload->GetArrayField(TEXT("names"));
		if (!TestTrue(TEXT("public query returns the exact native class name"), CandidateNames.Num() == 1
			&& CandidateNames[0]->AsString() == UPlayerMappableKeySettings::StaticClass()->GetDisplayNameText().ToString())) return false;
		const FString CandidateName = CandidateNames[0]->AsString();
		TArray<uint8> DiskBefore, DiskAfter;
		FFileHelper::LoadFileToArray(DiskBefore, *Filename);
		for (bool BadSelect : {false, true})
		{
			const FString Stage = BadSelect ? TEXT("invalidselect") : TEXT("invalidname");
			if (!Export(Stage) || !EditValue(Stage, OwnedValue(BadSelect ? CandidateName : TEXT("MissingOwnedClass"), TEXT("Invalid"), BadSelect))) return false;
			Result = Import(Stage); TestFalse(TEXT("invalid owned selector rejects before write"), Result.IsOk());
			DiskAfter.Reset(); FFileHelper::LoadFileToArray(DiskAfter, *Filename);
			TestTrue(TEXT("invalid selector preserves null memory and disk"), !Action->GetPlayerMappableKeySettings()
				&& !Action->GetPackage()->IsDirty() && DiskBefore == DiskAfter && OtherInputState(*Action, PropertyName).OrderIndependentCompareEqual(NonTarget));
		}
		// Dirty memory differs from the saved asset before null-to-owned creation.
		Action->Modify(); Action->ValueType = EInputActionValueType::Axis2D; Action->MarkPackageDirty();
		const auto DirtyOther = OtherInputState(*Action, PropertyName);
		if (!Export(TEXT("ownedlate")) || !EditValue(TEXT("ownedlate"), OwnedValue(CandidateName, TEXT("Published Mapping")))) return false;
		bool SawPersisted = false;
		const auto Handle = FCoreUObjectDelegates::OnAssetLoaded.AddLambda([&](UObject* Object)
		{
			auto* Published = Cast<UInputAction>(Object);
			if (SawPersisted || !Published || Published->GetPathName() != ObjectPath) return;
			auto* Settings = Published->GetPlayerMappableKeySettings().Get();
			TArray<uint8> Bytes;
			SawPersisted = Settings && Settings->GetClass() == UPlayerMappableKeySettings::StaticClass()
				&& Settings->GetOuter() == Published && Settings->DisplayName.ToString() == TEXT("Published Mapping")
				&& !Published->GetPackage()->IsDirty() && FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes != DiskBefore
				&& OtherInputState(*Published, PropertyName).OrderIndependentCompareEqual(DirtyOther);
			if (SawPersisted) Settings->DisplayName = FText::FromString(TEXT("Injected Mismatch"));
		});
		Action = nullptr; Result = Import(TEXT("ownedlate"));
		FCoreUObjectDelegates::OnAssetLoaded.Remove(Handle);
		TestFalse(TEXT("late owned readback mismatch rejects"), Result.IsOk());
		TestTrue(TEXT("late observed native class outer value after saved reload"), SawPersisted);
		Action = LoadObject<UInputAction>(nullptr, *ObjectPath);
		TestTrue(TEXT("late restores dirty null-owned baseline"), Action && !Action->GetPlayerMappableKeySettings()
			&& Action->GetPackage()->IsDirty() && OtherInputState(*Action, PropertyName).OrderIndependentCompareEqual(DirtyOther));
		DiskAfter.Reset(); FFileHelper::LoadFileToArray(DiskAfter, *Filename);
		TestTrue(TEXT("late restores original disk separately"), !DiskBefore.IsEmpty() && DiskAfter == DiskBefore);
		if (!Export(TEXT("ownedcreate")) || !EditValue(TEXT("ownedcreate"), OwnedValue(CandidateName, TEXT("Player Mapping")))) return false;
		Action = nullptr; Result = Import(TEXT("ownedcreate"));
		if (!TestTrue(TEXT("ordinary direct owned creation succeeds"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
		if (!TestTrue(TEXT("independent owned unload"), T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error))) { AddError(Error); return false; }
		Action = LoadObject<UInputAction>(nullptr, *ObjectPath);
		auto* Settings = Action ? Action->GetPlayerMappableKeySettings().Get() : nullptr;
		TestTrue(TEXT("owned class outer value persist and non-target remains"), Settings && Settings->GetClass() == UPlayerMappableKeySettings::StaticClass()
			&& Settings->GetOuter() == Action && Settings->DisplayName.ToString() == TEXT("Player Mapping") && !Action->GetPackage()->IsDirty()
			&& OtherInputState(*Action, PropertyName).OrderIndependentCompareEqual(DirtyOther));
		TestTrue(TEXT("independent owned workspace export"), Export(TEXT("ownedreadback")));
		return !Test.HasAnyErrors();
	}
	if (!TestTrue(TEXT("ordinary export and root edit"), Export(TEXT("edit")) && Edit(TEXT("edit"), TEXT("Axis1D")))) return false;
	Action = nullptr; Result = Import(TEXT("edit"));
	if (!TestTrue(TEXT("ordinary root imports"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	if (!TestTrue(TEXT("independent native unload"), T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error))) { AddError(Error); return false; }
	Action = LoadObject<UInputAction>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("root value persists with non-target native state"), Action && Action->ValueType == EInputActionValueType::Axis1D
		&& !Action->GetPackage()->IsDirty() && OtherInputState(*Action).OrderIndependentCompareEqual(NonTarget))) return false;
	if (!TestTrue(TEXT("independent workspace export"), Export(TEXT("readback")))) return false;
	TArray<uint8> DiskBefore; FFileHelper::LoadFileToArray(DiskBefore, *Filename);
	if (!TestTrue(TEXT("illegal enum edits exported root"), Edit(TEXT("readback"), TEXT("NotAnInputValueType")))) return false;
	Result = Import(TEXT("readback")); TestFalse(TEXT("illegal enum rejects"), Result.IsOk());
	TArray<uint8> DiskAfter; FFileHelper::LoadFileToArray(DiskAfter, *Filename);
	TestTrue(TEXT("invalid input writes neither memory nor disk"), Action->ValueType == EInputActionValueType::Axis1D
		&& !Action->GetPackage()->IsDirty() && DiskBefore == DiskAfter);
	Action->Modify(); Action->ValueType = EInputActionValueType::Axis2D; Action->MarkPackageDirty();
	if (!TestTrue(TEXT("distinct dirty baseline and late edit"), Export(TEXT("late")) && Edit(TEXT("late"), TEXT("Axis3D")))) return false;
	bool SawPersisted = false;
	const auto Handle = FCoreUObjectDelegates::OnAssetLoaded.AddLambda([&](UObject* Object)
	{
		auto* Published = Cast<UInputAction>(Object);
		if (SawPersisted || !Published || Published->GetPathName() != ObjectPath || Published->ValueType != EInputActionValueType::Axis3D) return;
		TArray<uint8> Bytes;
		SawPersisted = !Published->GetPackage()->IsDirty() && FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes != DiskBefore
			&& OtherInputState(*Published).OrderIndependentCompareEqual(NonTarget);
		if (SawPersisted) Published->ValueType = EInputActionValueType::Boolean;
	});
	ON_SCOPE_EXIT { FCoreUObjectDelegates::OnAssetLoaded.Remove(Handle); };
	Action = nullptr; Result = Import(TEXT("late"));
	TestFalse(TEXT("late persisted mismatch rejects"), Result.IsOk());
	TestTrue(TEXT("late observer saw saved reloaded candidate before rejection"), SawPersisted);
	Action = LoadObject<UInputAction>(nullptr, *ObjectPath);
	TestTrue(TEXT("dirty memory restored independently"), Action && Action->ValueType == EInputActionValueType::Axis2D
		&& Action->GetPackage()->IsDirty() && OtherInputState(*Action).OrderIndependentCompareEqual(NonTarget));
	DiskAfter.Reset(); FFileHelper::LoadFileToArray(DiskAfter, *Filename);
	TestTrue(TEXT("original disk bytes separately restored"), DiskBefore.Num() > 0 && DiskBefore == DiskAfter);
	return !Test.HasAnyErrors();
}

bool FUeAgentT208InputActionRootWorkspaceTest::RunTest(const FString&)
{ return RunInputActionWorkspace(*this, false); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208InputActionOwnedWorkspaceTest,
	"UAI.Root.T208.InputAction.OwnedWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208InputActionOwnedWorkspaceTest::RunTest(const FString&)
{ return RunInputActionWorkspace(*this, true); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208InputActionOwnedArraysTest,
	"UAI.Root.T208.InputAction.OwnedArrays", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208InputActionOwnedArraysTest::RunTest(const FString&)
{ return RunInputActionWorkspace(*this, false, true); }
