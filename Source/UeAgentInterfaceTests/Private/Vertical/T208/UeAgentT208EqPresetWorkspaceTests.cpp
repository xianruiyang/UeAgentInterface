#include "Misc/AutomationTest.h"
#include "AudioDeviceManager.h"
#include "AudioMixerDevice.h"
#include "AudioMixerSubmix.h"
#include "AudioThread.h"
#include "Engine/Engine.h"
#include "SubmixEffects/AudioMixerSubmixEffectEQ.h"
#include "Sound/SoundSubmix.h"
#include "Dom/JsonObject.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

namespace
{
TSharedPtr<FJsonObject> FindEqRow(const TSharedPtr<FJsonObject>& Object)
{
	for (const auto& Field : Object->Values)
	{
		if (Field.Key == TEXT("Settings") && Field.Value->Type == EJson::Object) return Field.Value->AsObject();
		if (Field.Value->Type == EJson::Object) if (auto Found = FindEqRow(Field.Value->AsObject())) return Found;
	}
	return nullptr;
}
struct FEqDeviceObservation
{
	FEvent* Done = FPlatformProcess::GetSynchEventFromPool(true);
	bool bMatches = false;
	~FEqDeviceObservation() { FPlatformProcess::ReturnSynchEventToPool(Done); }
};
struct FEqChainState { FSoundEffectSubmixPtr Other; };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208EqPresetWorkspaceTest,
	"UAI.Root.T208.AudioEffectPreset.EqWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208EqPresetWorkspaceTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	auto* Device = GEngine ? GEngine->GetMainAudioDeviceRaw() : nullptr;
	if (!TestTrue(TEXT("real mixer device exists"), Device && Device->bAudioMixerModuleLoaded)) return false;
	auto* Mixer = static_cast<Audio::FMixerDevice*>(Device);
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Base = TEXT("/Game/UAI/Transient/T208EqPreset/") + Token;
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-eq-preset/native") / Token);
	const TArray<FString> Names{TEXT("Preset"), TEXT("Submix"), TEXT("OtherPreset")};
	TArray<FString> Paths, Packages, Filenames;
	for (const auto& Name : Names) { Packages.Add(Base / Name); Paths.Add(Packages.Last() + TEXT(".") + Name); Filenames.Add(FPackageName::LongPackageNameToFilename(Packages.Last(), FPackageName::GetAssetPackageExtension())); }
	FString Error;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args) {
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args, TEXT("t208-eq-preset"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")}); };
	auto Load = [&]() { return LoadObject<USubmixEffectSubmixEQPreset>(nullptr, *Paths[0]); };
	auto Submix = [&]() { return LoadObject<USoundSubmix>(nullptr, *Paths[1]); };
	auto Save = [&](int32 Index) { auto* Object = LoadObject<UObject>(nullptr, *Paths[Index]); FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError; return Object && UPackage::SavePackage(Object->GetPackage(), Object, *Filenames[Index], Args); };
	ON_SCOPE_EXIT
	{
		TArray<TSharedPtr<FJsonValue>> Targets;
		for (int32 Index = 0; Index < Paths.Num(); ++Index) {
			if (auto* Object = FindObject<UObject>(nullptr, *Paths[Index]); Object && Object->GetPackage()->IsDirty()) TestTrue(TEXT("save own disposable fixture"), Save(Index));
			if (FPackageName::DoesPackageExist(Packages[Index])) Targets.Add(MakeShared<FJsonValueString>(Paths[Index]));
		}
		auto Args = MakeShared<FJsonObject>(); Args->SetArrayField(TEXT("targets"), Targets); Args->SetBoolField(TEXT("recursive"), false);
		for (bool Confirm : {false, true}) { Args->SetBoolField(TEXT("confirm"), Confirm); const auto Deleted = Submit(TEXT("asset.delete"), Args); if (!Deleted.IsOk()) AddError(Deleted.GetErrorJson()); }
	};
	for (int32 Index = 0; Index < Paths.Num(); ++Index) NewObject<UObject>(CreatePackage(*Packages[Index]), Index == 1 ? USoundSubmix::StaticClass() : USubmixEffectSubmixEQPreset::StaticClass(), *Names[Index], RF_Public | RF_Standalone | RF_Transactional);
	Load()->Init();
	auto* Other = LoadObject<USubmixEffectSubmixEQPreset>(nullptr, *Paths[2]); Other->Init();
	Submix()->SubmixEffectChain = {Load(), Other};
	for (int32 Index = 0; Index < Paths.Num(); ++Index) if (!Save(Index)) return false;
	FAudioDeviceManager::Get()->RegisterSoundSubmix(Submix());
	TWeakObjectPtr<USoundSubmix> OriginalSubmix(Submix());
	TWeakObjectPtr<USubmixEffectSubmixEQPreset> OriginalOther(Other);
	TArray<uint8> SubmixDisk, OtherDisk; FFileHelper::LoadFileToArray(SubmixDisk, *Filenames[1]); FFileHelper::LoadFileToArray(OtherDisk, *Filenames[2]);
	auto PreservedEffects = MakeShared<FEqChainState, ESPMode::ThreadSafe>();
	auto NativeMatches = [&](int32 Count, float Frequency) {
		const auto Copy = Load()->GetSettings();
		return Load()->Settings.EQBands.Num() == Count && Copy.EQBands.Num() == Count
			&& (!Count || (Load()->Settings.EQBands[0].Frequency == Frequency && Copy.EQBands[0].Frequency == Frequency)); };
	auto DeviceMatches = [&]() {
		FAudioCommandFence Fence; Fence.BeginFence(); Fence.Wait(); Mixer->FlushAudioRenderingCommands(); Mixer->FlushAudioRenderingCommands();
		auto Observation = MakeShared<FEqDeviceObservation, ESPMode::ThreadSafe>(); auto* Asset = Load(); auto* SubmixAsset = Submix();
		FAudioThread::RunCommandOnAudioThread([Mixer, Asset, SubmixAsset, Other, Observation, PreservedEffects]() {
			const auto Instance = Mixer->GetSubmixInstance(SubmixAsset).Pin();
			Mixer->AudioRenderThreadCommand([Instance, Asset, Other, Observation, PreservedEffects]() {
				bool Good = Instance.IsValid() && Instance->GetNumChainEffects() == 2;
				if (Good) { const auto Effect = Instance->GetSubmixEffect(0), OtherEffect = Instance->GetSubmixEffect(1);
					if (!PreservedEffects->Other.IsValid()) { PreservedEffects->Other = OtherEffect; if (Effect.IsValid()) Effect->SetEnabled(false); }
					Good = Effect.IsValid() && Effect->GetPreset() == Asset && Effect->GetParentPresetId() == Asset->GetUniqueID()
						&& !Effect->IsActive() && OtherEffect == PreservedEffects->Other
						&& OtherEffect.IsValid() && OtherEffect->GetPreset() == Other && OtherEffect->GetParentPresetId() == Other->GetUniqueID(); }
				Observation->bMatches = Good; Observation->Done->Trigger();
			});
		});
		FAudioCommandFence DispatchFence; DispatchFence.BeginFence(); DispatchFence.Wait();
		return Observation->Done->Wait(5000) && Observation->bMatches;
	};
	auto NonTargetPreserved = [&]() { TArray<uint8> A, B; return OriginalSubmix.Get() == Submix() && OriginalOther.Get() == Other
		&& !Submix()->GetPackage()->IsDirty() && !Other->GetPackage()->IsDirty() && FFileHelper::LoadFileToArray(A, *Filenames[1]) && FFileHelper::LoadFileToArray(B, *Filenames[2]) && A == SubmixDisk && B == OtherDisk; };
	if (!TestTrue(TEXT("baseline empty preset native copy and actual registered effect"), NativeMatches(0, 0) && DeviceMatches())) return false;
	auto Export = [&](const FString& Stage) { auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), Paths[0]); Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring")); const auto Result = Submit(TEXT("content_graph.export_workspace"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk(); };
	auto Import = [&](const FString& Stage) { auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), Paths[0]); auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage); Args->SetObjectField(TEXT("source"), Source); return Submit(TEXT("content_graph.import_workspace"), Args); };
	auto Edit = [&](const FString& Stage, int32 Count, bool bWrongType) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false);
		TSharedPtr<FJsonObject> Document, Row; FString PropertyFile; TSharedPtr<FJsonObject> Template;
		for (const auto& File : Files) {
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Parsed; if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Parsed, Error)) return false;
			if (auto Found = FindEqRow(Parsed)) { Row = Found; Document = Parsed; PropertyFile = File; }
			const TSharedPtr<FJsonObject>* Definitions = nullptr;
			if (Parsed->TryGetObjectField(TEXT("definitions"), Definitions)) for (const auto& Definition : (*Definitions)->Values) {
				const TSharedPtr<FJsonObject>* Schema = nullptr; if (Definition.Value->AsObject()->TryGetObjectField(TEXT("value_definition"), Schema)) {
					const TSharedPtr<FJsonObject>* Value = nullptr; if ((*Schema)->TryGetObjectField(TEXT("element_template"), Value)) Template = *Value;
				}
			}
		}
		if (!TestTrue(TEXT("Settings and copyable band template are ordinary visible content"), Row && Template)) return false;
		auto Fields = Row->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"));
		TArray<TSharedPtr<FJsonValue>> Bands;
		for (int32 Index = 0; Index < Count; ++Index) {
			auto Band = MakeShared<FJsonObject>(); Band->Values = Template->Values;
			auto BandFields = MakeShared<FJsonObject>(); BandFields->Values = Template->GetObjectField(TEXT("fields"))->Values; Band->SetObjectField(TEXT("fields"), BandFields);
			if (!TestTrue(TEXT("band editable fields come from template"), BandFields->HasField(TEXT("Frequency")) && BandFields->HasField(TEXT("GainDb")) && BandFields->HasField(TEXT("bEnabled")))) return false;
			if (bWrongType) BandFields->SetStringField(TEXT("Frequency"), TEXT("invalid-number")); else BandFields->SetNumberField(TEXT("Frequency"), Count == 1 ? 1000 : 2000);
			BandFields->SetNumberField(TEXT("GainDb"), 3); BandFields->SetBoolField(TEXT("bEnabled"), true); Bands.Add(MakeShared<FJsonValueObject>(Band));
		}
		Fields->SetArrayField(TEXT("EQBands"), Bands); FString Text; return FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error) && FFileHelper::SaveStringToFile(Text, *PropertyFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	};
	if (!Export(TEXT("first")) || !Edit(TEXT("first"), 1, false)) return false;
	const TWeakObjectPtr<USubmixEffectSubmixEQPreset> Before(Load());
	auto Result = Import(TEXT("first"));
	if (!TestTrue(TEXT("first band imports through ordinary workspace"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	const bool bReplaced = TestFalse(TEXT("persisted preset is independently reloaded"), Before.IsValid());
	const bool bNative = TestTrue(TEXT("persisted settings and native settings copy agree"), NativeMatches(1, 1000));
	const bool bDevice = TestTrue(TEXT("registered native chain refers to reloaded preset"), DeviceMatches());
	AddInfo(FString::Printf(TEXT("EQ readback: old_valid=%d property_bands=%d copy_bands=%d device=%d"), Before.IsValid(), Load()->Settings.EQBands.Num(), Load()->GetSettings().EQBands.Num(), bDevice));
	if (!bReplaced || !bNative || !bDevice || !TestTrue(TEXT("non-target assets and packages preserved"), NonTargetPreserved())) return false;
	if (!Export(TEXT("bad")) || !Edit(TEXT("bad"), 1, true)) return false;
	Result = Import(TEXT("bad")); TestTrue(TEXT("wrong input rejected before copy/device mutation"), !Result.IsOk() && NativeMatches(1, 1000) && DeviceMatches() && NonTargetPreserved());
	if (!Export(TEXT("override")) || !Edit(TEXT("override"), 2, false)) return false;
	auto SetOverride = [&](bool bOverride) {
		auto Observation = MakeShared<FEqDeviceObservation, ESPMode::ThreadSafe>(); auto* SubmixAsset = Submix();
		FAudioThread::RunCommandOnAudioThread([Mixer, SubmixAsset, bOverride, Observation, PreservedEffects]() {
			const auto Instance = Mixer->GetSubmixInstance(SubmixAsset).Pin();
			Mixer->AudioRenderThreadCommand([Instance, bOverride, Observation, PreservedEffects]() {
				if (Instance.IsValid()) {
					if (bOverride) Instance->SetSubmixEffectChainOverride({PreservedEffects->Other}, 0.0f);
					else Instance->ClearSubmixEffectChainOverride(0.0f);
					Observation->bMatches = Instance->GetNumChainEffects() == (bOverride ? 1 : 2);
				}
				Observation->Done->Trigger();
			});
		});
		FAudioCommandFence Fence; Fence.BeginFence(); Fence.Wait();
		return Observation->Done->Wait(5000) && Observation->bMatches;
	};
	if (!TestTrue(TEXT("native incompatible override established"), SetOverride(true))) return false;
	Result = Import(TEXT("override"));
	const bool bOverrideRefused = !Result.IsOk() && Result.GetErrorJson().Contains(TEXT("eq_preset_incompatible_current_chain"));
	const bool bOverrideCleared = SetOverride(false);
	if (!TestTrue(TEXT("incompatible current chain refuses before changing preset or non-target slot"), bOverrideRefused && bOverrideCleared
		&& NativeMatches(1, 1000) && DeviceMatches() && NonTargetPreserved())) return false;
	if (!Export(TEXT("changed")) || !Edit(TEXT("changed"), 2, false)) return false;
	Result = Import(TEXT("changed")); if (!TestTrue(TEXT("band struct and array edits persist"), Result.IsOk() && NativeMatches(2, 2000) && DeviceMatches())) { if (!Result.IsOk()) AddError(Result.GetErrorJson()); return false; }
	TArray<uint8> PublishedDisk; FFileHelper::LoadFileToArray(PublishedDisk, *Filenames[0]);
	UeAgentReflectionAuthoring::FAssetApplyRequest Late; Late.TypeId = TEXT("audio_effect_preset"); Late.AssetPath = Packages[0]; Late.bSave = true; Late.bCompactValues = true; Late.bEmitEditorNotifications = true;
	auto Patch = MakeShared<FJsonObject>(); Patch->SetStringField(TEXT("property_path"), TEXT("Settings.EQBands[0].Frequency")); Patch->SetNumberField(TEXT("value"), 3000); Late.Properties.Add(MakeShared<FJsonValueObject>(Patch));
	bool SawPublished = false;
	Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		SawPublished = NativeMatches(2, 3000) && DeviceMatches(); Failure.Diagnostic.Code = TEXT("t208_eq_late_rejected"); return false; };
	UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
	TestFalse(TEXT("late rejection fails"), UeAgentReflectionAuthoring::ApplyAsset(Late, LateResult));
	TArray<uint8> RestoredDisk; FFileHelper::LoadFileToArray(RestoredDisk, *Filenames[0]);
	if (!TestTrue(TEXT("late hook observes independently rebound native instance"), SawPublished)
		|| !TestTrue(TEXT("checkpoint rollback restores settings copy native identity and non-target instance"), LateResult.bRollbackSucceeded
			&& LateResult.Diagnostic.Code == TEXT("t208_eq_late_rejected") && NativeMatches(2, 2000) && DeviceMatches()
			&& NonTargetPreserved() && RestoredDisk == PublishedDisk)) return false;
	if (!Export(TEXT("clear")) || !Edit(TEXT("clear"), 0, false)) return false;
	Result = Import(TEXT("clear")); TestTrue(TEXT("clear propagates native settings and device state"), Result.IsOk() && NativeMatches(0, 0) && DeviceMatches() && NonTargetPreserved());
	return !HasAnyErrors();
}
