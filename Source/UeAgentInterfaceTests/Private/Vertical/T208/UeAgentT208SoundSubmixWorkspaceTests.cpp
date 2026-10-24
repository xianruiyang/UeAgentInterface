#include "Misc/AutomationTest.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "AudioDeviceManager.h"
#include "AudioMixerDevice.h"
#include "AudioMixerSubmix.h"
#include "AudioThread.h"
#include "Engine/Engine.h"
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Sound/SoundSubmix.h"
#include "Sound/SoundEffectSubmix.h"
#include "SubmixEffects/AudioMixerSubmixEffectEQ.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetDeleteOwner.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentSoundSubmixDeleteLifecycle.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

namespace
{
int32 EditSubmixField(const TSharedPtr<FJsonObject>& Object, const FString& Name, const TSharedPtr<FJsonValue>& Value)
{
	int32 Count = 0;
	for (auto& Field : Object->Values)
	{
		if (Field.Key == Name)
		{
			if (Field.Value->Type == EJson::Object && Field.Value->AsObject()->HasField(TEXT("value"))) Field.Value->AsObject()->SetField(TEXT("value"), Value);
			else Field.Value = Value;
			++Count;
		}
		else if (Field.Value->Type == EJson::Object) Count += EditSubmixField(Field.Value->AsObject(), Name, Value);
	}
	return Count;
}
struct FSubmixDeviceObservation
{
	FEvent* Done = FPlatformProcess::GetSynchEventFromPool(true);
	bool bMatches = false;
	~FSubmixDeviceObservation() { FPlatformProcess::ReturnSynchEventToPool(Done); }
};
class FSubmixDeleteCancellation final : public UE::UAI::Contract::IUeAgentCancellationSignal
{
public:
	TFunction<bool()> AfterRelease;
	virtual bool IsCancelled() const override { return AfterRelease(); }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208SoundSubmixWorkspaceTest,
	"UAI.Root.T208.SoundSubmix.Workspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208SoundSubmixWorkspaceTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	auto* Device = GEngine ? GEngine->GetMainAudioDeviceRaw() : nullptr;
	if (!TestTrue(TEXT("real audio mixer is required"), Device && Device->bAudioMixerModuleLoaded)) return false;
	auto* Mixer = static_cast<Audio::FMixerDevice*>(Device);
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Base = TEXT("/Game/UAI/Transient/T208SoundSubmix/") + Token;
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-sound-submix/native") / Token);
	const TArray<FString> Names{TEXT("Root"), TEXT("OldParent"), TEXT("Moved"), TEXT("Removed"), TEXT("EffectA"), TEXT("EffectB"), TEXT("Unrelated")};
	TArray<FString> Paths, Packages, Filenames;
	for (const auto& Name : Names) { Packages.Add(Base / Name); Paths.Add(Packages.Last() + TEXT(".") + Name); Filenames.Add(FPackageName::LongPackageNameToFilename(Packages.Last(), FPackageName::GetAssetPackageExtension())); }
	UE::UAI::Contract::FDiagnostic Error;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error.ToPublicDetail()); return false; }
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args) {
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
			TEXT("t208-sound-submix"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")}); };
	auto Load = [&](int32 Index) { return LoadObject<USoundSubmix>(nullptr, *Paths[Index]); };
	auto Effect = [&](int32 Index) { return LoadObject<USoundEffectSubmixPreset>(nullptr, *Paths[Index]); };
	auto Save = [&](int32 Index) {
		auto* Object = LoadObject<UObject>(nullptr, *Paths[Index]); FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
		return Object && UPackage::SavePackage(Object->GetPackage(), Object, *Filenames[Index], Args); };
	ON_SCOPE_EXIT
	{
		TArray<TSharedPtr<FJsonValue>> Targets;
		for (int32 Index = 0; Index < Paths.Num(); ++Index)
		{
			if (auto* Object = FindObject<UObject>(nullptr, *Paths[Index]); Object && Object->GetPackage()->IsDirty()) TestTrue(TEXT("settle own disposable fixture"), Save(Index));
			if (FPackageName::DoesPackageExist(Packages[Index])) Targets.Add(MakeShared<FJsonValueString>(Paths[Index]));
		}
		if (!Targets.IsEmpty())
		{
			auto Delete = MakeShared<FJsonObject>(); Delete->SetArrayField(TEXT("targets"), Targets); Delete->SetBoolField(TEXT("recursive"), false);
			for (bool Confirm : {false, true})
			{
				Delete->SetBoolField(TEXT("confirm"), Confirm); const auto Result = Submit(TEXT("asset.delete"), Delete);
				if (!Result.IsOk())
				{
					AddError(Result.GetErrorJson());
					for (int32 Index = 0; Index < Paths.Num(); ++Index)
					{
						const UObject* Remaining = FindObject<UObject>(nullptr, *Paths[Index]);
						AddInfo(FString::Printf(TEXT("delete readback %s: file=%d package=%d object=%d rooted=%d"), *Names[Index],
							IFileManager::Get().FileExists(*Filenames[Index]), FPackageName::DoesPackageExist(Packages[Index]), Remaining != nullptr, Remaining && Remaining->IsRooted()));
					}
				}
			}
		}
	};
	for (int32 Index = 0; Index < Paths.Num(); ++Index)
		NewObject<UObject>(CreatePackage(*Packages[Index]), Index == 4 || Index == 5 ? USubmixEffectSubmixEQPreset::StaticClass() : USoundSubmix::StaticClass(), *Names[Index], RF_Public | RF_Standalone | RF_Transactional);
	Load(3)->SetParentSubmix(Load(0)); Load(2)->SetParentSubmix(Load(1));
	Load(0)->SubmixEffectChain = {Effect(4)};
	for (int32 Index = 0; Index < Paths.Num(); ++Index) if (!Save(Index)) return false;
	for (int32 Index : {0, 1, 2, 3, 6}) FAudioDeviceManager::Get()->RegisterSoundSubmix(Load(Index));
	Load(6)->AutoDisableTime = 7.0f; Load(6)->MarkPackageDirty();
	TArray<TArray<uint8>> InitialDisk; InitialDisk.SetNum(Paths.Num());
	for (int32 Index = 0; Index < Paths.Num(); ++Index) if (!FFileHelper::LoadFileToArray(InitialDisk[Index], *Filenames[Index])) return false;
	TWeakObjectPtr<UObject> EffectA(Effect(4)), EffectB(Effect(5)), Unrelated(Load(6));
	auto UnrelatedPreserved = [&]() {
		bool Good = EffectA.Get() == Effect(4) && EffectB.Get() == Effect(5) && Unrelated.Get() == Load(6)
			&& Load(6)->AutoDisableTime == 7.0f && Load(6)->GetPackage()->IsDirty();
		for (int32 Index : {4, 5, 6}) { TArray<uint8> Bytes; Good &= FFileHelper::LoadFileToArray(Bytes, *Filenames[Index]) && Bytes == InitialDisk[Index]; }
		return TestTrue(TEXT("external presets and unrelated dirty asset are preserved"), Good);
	};
	auto DeviceMatches = [&](bool bMovedUnderRoot, const TArray<int32>& Effects) {
		FAudioCommandFence Fence; Fence.BeginFence(); Fence.Wait();
		Mixer->FlushAudioRenderingCommands(); Mixer->FlushAudioRenderingCommands();
		auto Observation = MakeShared<FSubmixDeviceObservation, ESPMode::ThreadSafe>();
		auto* Root = Load(0); auto* Moved = Load(2);
		TArray<uint32> PresetIds; for (int32 Index : Effects) PresetIds.Add(Effect(Index)->GetUniqueID());
		FAudioThread::RunCommandOnAudioThread([Mixer, Root, Moved, bMovedUnderRoot, PresetIds, Observation]() {
			auto RootInstance = Mixer->GetSubmixInstance(Root).Pin(); auto MovedInstance = Mixer->GetSubmixInstance(Moved).Pin();
			Mixer->AudioRenderThreadCommand([RootInstance, MovedInstance, bMovedUnderRoot, PresetIds, Observation]() {
				bool Good = RootInstance.IsValid() && MovedInstance.IsValid();
				if (Good) {
					Good = (MovedInstance->GetParentSubmix().Pin() == RootInstance) == bMovedUnderRoot;
					Good &= RootInstance->GetNumChainEffects() == PresetIds.Num();
					for (int32 Index = 0; Good && Index < PresetIds.Num(); ++Index) {
						const auto Instance = RootInstance->GetSubmixEffect(Index);
						Good &= Instance.IsValid() && Instance->GetParentPresetId() == PresetIds[Index];
					}
				}
				Observation->bMatches = Good; Observation->Done->Trigger();
			});
		});
		// Game-thread audio commands are batched until a fence or frame end.
		FAudioCommandFence DispatchFence; DispatchFence.BeginFence(); DispatchFence.Wait();
		return Observation->Done->Wait(5000) && Observation->bMatches;
	};
	auto NoPreviewRegistration = [&]() {
		TArray<UObject*> Objects; TArray<uint32> ScratchIds;
		GetObjectsOfClass(USoundSubmixBase::StaticClass(), Objects);
		for (UObject* Object : Objects)
			if (Object->GetPathName().StartsWith(TEXT("/Temp/UAI/Reflection/Proposed_"))) ScratchIds.Add(Object->GetUniqueID());
		bool bClean = true;
		FAudioThread::RunCommandOnAudioThread([Mixer, ScratchIds, &bClean]() {
			for (uint32 Id : ScratchIds) bClean &= !Mixer->FindSubmixInstanceByObjectId(Id).IsValid();
		});
		FAudioCommandFence Fence; Fence.BeginFence(); Fence.Wait();
		return TestTrue(TEXT("proposed roots never register with the real mixer"), bClean);
	};
	if (!TestTrue(TEXT("native mixer fixture is registered"), DeviceMatches(false, {4}))) return false;
	auto Export = [&](int32 Index, const FString& Stage) {
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), Paths[Index]); Args->SetStringField(TEXT("workspace_path"), Directory / Stage);
		Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring")); const auto Result = Submit(TEXT("content_graph.export_workspace"), Args);
		if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk(); };
	auto Edit = [&](const FString& Stage, const TArray<int32>& Children, TOptional<TArray<int32>> Effects, TOptional<float> Time) {
		TMap<FString, TSharedPtr<FJsonValue>> Changes;
		TArray<TSharedPtr<FJsonValue>> ChildRefs; for (int32 Index : Children) ChildRefs.Add(MakeShared<FJsonValueString>(Paths[Index]));
		Changes.Add(TEXT("child_submixes"), MakeShared<FJsonValueArray>(ChildRefs));
		if (Effects.IsSet()) { TArray<TSharedPtr<FJsonValue>> Refs; for (int32 Index : Effects.GetValue()) Refs.Add(MakeShared<FJsonValueString>(Paths[Index])); Changes.Add(TEXT("SubmixEffectChain"), MakeShared<FJsonValueArray>(Refs)); }
		if (Time.IsSet()) Changes.Add(TEXT("AutoDisableTime"), MakeShared<FJsonValueNumber>(Time.GetValue()));
		TMap<FString, int32> Counts;
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false);
		for (const auto& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\")) || File.EndsWith(TEXT("definitions.hjson"))) continue;
			FString Text; TSharedPtr<FJsonObject> Object;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Object, Error)) { AddError(Error.ToPublicDetail()); return false; }
			int32 Count = 0; for (const auto& Change : Changes) { int32 Changed = EditSubmixField(Object, Change.Key, Change.Value); Counts.FindOrAdd(Change.Key) += Changed; Count += Changed; }
			if (Count && (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Object.ToSharedRef(), Text, Error)
				|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) { AddError(Error.ToPublicDetail()); return false; }
		}
		for (const auto& Change : Changes) if (!TestEqual(*Change.Key, Counts.FindRef(Change.Key), 1)) return false;
		return true;
	};
	auto Import = [&](int32 Index, const FString& Stage) {
		auto Args = MakeShared<FJsonObject>(); auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage);
		Args->SetStringField(TEXT("target"), Paths[Index]); Args->SetObjectField(TEXT("source"), Source); return Submit(TEXT("content_graph.import_workspace"), Args); };
	if (!Export(0, TEXT("relations")) || !Edit(TEXT("relations"), {2}, TArray<int32>{5, 4}, 1.25f)) return false;
	TArray<TWeakObjectPtr<USoundSubmix>> Before; for (int32 Index = 0; Index < 4; ++Index) Before.Add(Load(Index));
	auto Result = Import(0, TEXT("relations"));
	if (!TestTrue(TEXT("public settings relationships and effect chain apply together"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	auto Published = [&]() { return Load(0)->ChildSubmixes.Num() == 1 && Load(0)->ChildSubmixes[0] == Load(2) && Load(2)->ParentSubmix == Load(0)
		&& Load(1)->ChildSubmixes.IsEmpty() && !Load(3)->ParentSubmix && Load(0)->SubmixEffectChain == TArray<TObjectPtr<USoundEffectSubmixPreset>>{Effect(5), Effect(4)}; };
	if (!TestTrue(TEXT("native relationships and ordered external effect references agree"), Published())
		|| !TestTrue(TEXT("real mixer parent and effect preset order agree"), DeviceMatches(true, {5, 4})) || !UnrelatedPreserved() || !NoPreviewRegistration()) return false;
	for (int32 Index = 0; Index < 4; ++Index) if (!TestTrue(TEXT("affected package independently replaced and persisted"), !Before[Index].IsValid() && !Load(Index)->GetPackage()->IsDirty())) return false;
	TestEqual(TEXT("root scalar persisted"), Load(0)->AutoDisableTime, 1.25f);
	TArray<TArray<uint8>> PublishedDisk; PublishedDisk.SetNum(4); for (int32 Index = 0; Index < 4; ++Index) if (!FFileHelper::LoadFileToArray(PublishedDisk[Index], *Filenames[Index])) return false;
	if (!Export(2, TEXT("cycle")) || !Edit(TEXT("cycle"), {0}, {}, {})) return false;
	Result = Import(2, TEXT("cycle"));
	if (!TestTrue(TEXT("cycle rejected before mutation"), !Result.IsOk() && Result.GetErrorJson().Contains(TEXT("sound_submix_cycle"))) || !TestTrue(TEXT("cycle preserves state"), Published())) return false;
	if (!Export(0, TEXT("bad-effect")) || !Edit(TEXT("bad-effect"), {2}, TArray<int32>{1}, {})) return false;
	Result = Import(0, TEXT("bad-effect"));
	if (!TestTrue(TEXT("wrong effect object reference rejected"), !Result.IsOk()) || !TestTrue(TEXT("wrong effect preserves native and mixer state"), Published() && DeviceMatches(true, {5, 4}))) return false;
	for (int32 Index = 0; Index < 4; ++Index) { TArray<uint8> Bytes; TestTrue(TEXT("rejection preserves clean assets and disk"), !Load(Index)->GetPackage()->IsDirty() && FFileHelper::LoadFileToArray(Bytes, *Filenames[Index]) && Bytes == PublishedDisk[Index]); }
	Load(0)->AutoDisableTime = 2.25f; Load(0)->MarkPackageDirty(); Load(2)->AutoDisableTime = 3.25f; Load(2)->MarkPackageDirty();
	UeAgentReflectionAuthoring::FAssetApplyRequest Late; Late.TypeId = TEXT("sound_submix"); Late.AssetPath = Packages[0]; Late.bSave = true; Late.bCompactValues = true;
	Late.ProviderExtension = MakeShared<FJsonObject>(); Late.ProviderExtension->SetArrayField(TEXT("child_submixes"), {MakeShared<FJsonValueString>(Paths[3])});
	auto Chain = MakeShared<FJsonObject>(); Chain->SetStringField(TEXT("property_path"), TEXT("SubmixEffectChain")); Chain->SetArrayField(TEXT("value"), {MakeShared<FJsonValueString>(Paths[4])}); Late.Properties.Add(MakeShared<FJsonValueObject>(Chain));
	bool SawPublished = false;
	Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		SawPublished = Load(3)->ParentSubmix == Load(0) && !Load(2)->ParentSubmix && DeviceMatches(false, {4});
		Failure.ErrorCode = TEXT("t208_sound_submix_late_rejected"); return false; };
	UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
	TestFalse(TEXT("late rejection fails"), UeAgentReflectionAuthoring::ApplyAsset(Late, LateResult));
	if (!TestTrue(TEXT("late hook observes native published mixer state"), SawPublished)
		|| !TestTrue(TEXT("shared transaction restores submix memory device and dirty state"), LateResult.ErrorCode == TEXT("t208_sound_submix_late_rejected")
			&& LateResult.bRollbackSucceeded && Published() && Load(0)->AutoDisableTime == 2.25f && Load(2)->AutoDisableTime == 3.25f
			&& Load(0)->GetPackage()->IsDirty() && Load(2)->GetPackage()->IsDirty() && DeviceMatches(true, {5, 4})) || !UnrelatedPreserved() || !NoPreviewRegistration()) return false;
	for (int32 Index = 0; Index < 4; ++Index) { TArray<uint8> Bytes; TestTrue(TEXT("late rollback restores disk separately"), FFileHelper::LoadFileToArray(Bytes, *Filenames[Index]) && Bytes == PublishedDisk[Index]); }
	if (!Export(0, TEXT("clear")) || !Edit(TEXT("clear"), {}, TArray<int32>{}, {})) return false;
	Result = Import(0, TEXT("clear"));
	if (!TestTrue(TEXT("clear child and effect arrays applies"), Result.IsOk())) { AddError(Result.GetErrorJson()); return false; }
	TestTrue(TEXT("clear updates native and mixer relationships and chain"), Load(0)->ChildSubmixes.IsEmpty() && !Load(2)->ParentSubmix && Load(0)->SubmixEffectChain.IsEmpty() && DeviceMatches(false, {}));
	UnrelatedPreserved();
	// Exercise the delete owner's actual post-release cancellation boundary.
	// The old object must already be collected so restoration must rebind by path.
	using namespace DomainAdapters::AssetDestructiveImport;
	FSoundSubmixDeleteLifecycle RequiredGuard;
	TestFalse(TEXT("required device submix refused before release"), RequiredGuard.Prepare(Mixer->GetMainSubmixObject(), Error));
	TestEqual(TEXT("required refusal is native device qualification"), Error.Code, FString(TEXT("sound_submix_delete_required_device_use")));
	const TWeakObjectPtr<USoundSubmix> BeforeDelete(Load(0));
	const uint32 DeletedId = Load(0)->GetUniqueID();
	Audio::FMixerSubmixPtr UnrelatedDeviceBefore;
	auto* UnrelatedSubmix = Load(6);
	FAudioThread::RunCommandOnAudioThread([&]() { UnrelatedDeviceBefore = Mixer->GetSubmixInstance(UnrelatedSubmix).Pin(); });
	{ FAudioCommandFence Fence; Fence.BeginFence(); Fence.Wait(); }
	TSharedPtr<IUeAgentAssetDeleteOwner> DeleteOwner;
	UE::UAI::Contract::FDiagnostic DeleteOwnerError;
	if (!TestTrue(TEXT("create common delete owner"), TryCreateAssetDeleteOwner(Directory / TEXT("delete-journal"), TEXT("t208-submix-delete"), DeleteOwner, DeleteOwnerError))) return false;
	bool SawReleased = false;
	auto Cancellation = MakeShared<FSubmixDeleteCancellation>();
	Cancellation->AfterRelease = [&]()
	{
		if (BeforeDelete.IsValid()) return false;
		bool bStillRegistered = true;
		FAudioThread::RunCommandOnAudioThread([&]() { bStillRegistered = Mixer->FindSubmixInstanceByObjectId(DeletedId).IsValid(); });
		FAudioCommandFence Fence; Fence.BeginFence(); Fence.Wait();
		SawReleased = !BeforeDelete.IsValid() && !FindObject<UObject>(nullptr, *Paths[0]) && !bStillRegistered;
		return SawReleased;
	};
	FUeAgentAssetDeleteRequest DeleteRequest; DeleteRequest.RequestId = Token + TEXT("-cancel"); DeleteRequest.CallerId = TEXT("t208"); DeleteRequest.TargetRefs = {Paths[0]};
	const auto CancelledDelete = DeleteOwner->Delete(DeleteRequest, Cancellation);
	TestTrue(TEXT("cancellation observes collected object and released registration"), SawReleased);
	TestTrue(TEXT("delete cancellation restores actual asset and registration by path"), !CancelledDelete.IsSuccess()
		&& CancelledDelete.TerminalState == TEXT("rolled_back") && CancelledDelete.Targets.Num() == 1
		&& CancelledDelete.Targets[0].Diagnostic.Code == TEXT("asset_delete_cancelled") && Load(0) && Load(0)->IsRooted()
		&& Load(0)->HasAllFlags(RF_Public | RF_Standalone) && DeviceMatches(false, {}));
	bool bUnrelatedRegistrationPreserved = false;
	FAudioThread::RunCommandOnAudioThread([&]() { bUnrelatedRegistrationPreserved = UnrelatedDeviceBefore.IsValid() && UnrelatedDeviceBefore == Mixer->GetSubmixInstance(UnrelatedSubmix).Pin(); });
	{ FAudioCommandFence Fence; Fence.BeginFence(); Fence.Wait(); }
	TestTrue(TEXT("delete refusal and rollback preserve non-target registration"), bUnrelatedRegistrationPreserved);
	UnrelatedPreserved();
	return !HasAnyErrors();
}
