#include "Animation/AnimData/IAnimationDataModel.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetDeleteOwner.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringApplyProvider.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringRegistration.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/DestructiveContent/UeAgentDestructiveContentOperationOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UeAgentT85FixtureLifecycle.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	class FAnimationCurveNeverCancelled final : public UE::UAI::Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	struct FAnimationCurveNativeState
	{
		FString Skeleton;
		TArray<FString> Axes;
		TArray<FString> Samples;
		TArray<FName> Tracks;
		TArray<TArray<FTransform>> Keys;
		int32 Frames = 0;
		FFrameRate FrameRate;

		bool Equals(const FAnimationCurveNativeState& Other) const
		{
			if (Skeleton != Other.Skeleton || Axes != Other.Axes || Samples != Other.Samples
				|| Tracks != Other.Tracks || Frames != Other.Frames || FrameRate != Other.FrameRate
				|| Keys.Num() != Other.Keys.Num()) return false;
			for (int32 Track = 0; Track < Keys.Num(); ++Track)
			{
				if (Keys[Track].Num() != Other.Keys[Track].Num()) return false;
				for (int32 Key = 0; Key < Keys[Track].Num(); ++Key)
					if (!Keys[Track][Key].Equals(Other.Keys[Track][Key], 0.0)) return false;
			}
			return true;
		}
	};

	FAnimationCurveNativeState CaptureAnimationCurveNative(UAnimationAsset& Asset)
	{
		FAnimationCurveNativeState State;
		State.Skeleton = GetPathNameSafe(Asset.GetSkeleton());
		if (UBlendSpace* Blend = Cast<UBlendSpace>(&Asset))
		{
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FString Value;
				FBlendParameter::StaticStruct()->ExportText(Value, &Blend->GetBlendParameter(Index), nullptr, Blend, PPF_None, nullptr);
				State.Axes.Add(MoveTemp(Value));
			}
			for (const FBlendSample& Sample : Blend->GetBlendSamples())
			{
				FString Value;
				FBlendSample::StaticStruct()->ExportText(Value, &Sample, nullptr, Blend, PPF_None, nullptr);
				State.Samples.Add(MoveTemp(Value));
			}
		}
		if (UAnimSequence* Sequence = Cast<UAnimSequence>(&Asset))
		{
			const IAnimationDataModel* Model = Sequence->GetDataModel();
			if (Model)
			{
				State.Frames = Model->GetNumberOfFrames();
				State.FrameRate = Model->GetFrameRate();
				Model->GetBoneTrackNames(State.Tracks);
				for (const FName Name : State.Tracks)
					Model->GetBoneTrackTransforms(Name, State.Keys.AddDefaulted_GetRef());
			}
		}
		return State;
	}

	bool RunAnimationCurveAtomic(FAutomationTestBase& Test, const bool bSequence, const bool bPublicRoot = false, const bool bRetainNativeConsumer = false)
	{
		using namespace UeAgentAnimationAuthoring;
		using namespace UE::UAI::DomainAdapters::AssetDestructiveImport;
		UE::UAI::Contract::FDiagnostic Error;
		if (!Test.TestTrue(TEXT("Animation authoring contracts register"), RegisterAuthoring(Error))) return false;
		UAnimSequence* Source = LoadObject<UAnimSequence>(nullptr,
			TEXT("/Game/Characters/PaladinRootMotion/Animations/sword_and_shield_idle_RM.sword_and_shield_idle_RM"));
		if (!Test.TestNotNull(TEXT("the real source animation exists"), Source)
			|| !Test.TestNotNull(TEXT("the real source skeleton exists"), Source->GetSkeleton())) return false;
		const FString Root = TEXT("/Game/UAI/Transient/T85AnimationCurve_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString PackageName = Root + (bSequence ? TEXT("/Sequence") : TEXT("/BlendSpace"));
		const FString Name = FPackageName::GetLongPackageAssetName(PackageName);
		const FString ObjectPath = PackageName + TEXT(".") + Name;
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		TSharedPtr<IUeAgentAssetDeleteOwner> DeleteOwner;
		TStrongObjectPtr<UAnimationAsset> NativeConsumer;
		UE::UAI::Contract::FDiagnostic DeleteOwnerError;
		if (!TryCreateAssetDeleteOwner(FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T208/AnimationCurveDelete"),
			TEXT("animation-curve-atomic"), DeleteOwner, DeleteOwnerError)) { Test.AddError(DeleteOwnerError.ToPublicDetail()); return false; }
		ON_SCOPE_EXIT
		{
			NativeConsumer.Reset();
			if (UObject* Remaining = FindObject<UObject>(nullptr, *ObjectPath)) Remaining->GetOutermost()->SetDirtyFlag(false);
			const auto Deleted = DeleteOwner->Delete({TEXT("animation-curve-cleanup"), {ObjectPath}, false, TEXT("t208")}, MakeShared<FAnimationCurveNeverCancelled>());
			if (!Deleted.IsSuccess()) Test.AddError(Deleted.Targets.IsEmpty() ? TEXT("animation_fixture_cleanup_empty") : Deleted.Targets[0].Diagnostic.Code);
			UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(Test, PackageName);
		};
		UPackage* Package = CreatePackage(*PackageName);
		UAnimationAsset* Asset = nullptr;
		if (bSequence)
		{
			Asset = Cast<UAnimSequence>(StaticDuplicateObject(Source, Package, *Name, RF_AllFlags));
		}
		else
		{
			UBlendSpace* Blend = NewObject<UBlendSpace>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
			Blend->SetSkeleton(Source->GetSkeleton());
			Blend->AddSample(Source, FVector::ZeroVector);
			Blend->ValidateSampleData();
			Blend->ResampleData();
			Asset = Blend;
		}
		if (!Test.TestNotNull(TEXT("an independent native fixture is created"), Asset)) return false;
		Asset->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Asset);
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!Test.TestTrue(TEXT("the native fixture baseline saves"), UPackage::SavePackage(Package, Asset, *Filename, SaveArgs))) return false;
		Package->SetDirtyFlag(false);
		TArray<uint8> DiskBefore;
		if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
		const FAnimationCurveNativeState NativeBefore = CaptureAnimationCurveNative(*Asset);
		Test.TestTrue(TEXT("the native oracle contains real authored data"), bSequence ? !NativeBefore.Tracks.IsEmpty() : !NativeBefore.Samples.IsEmpty());
		const bool InitialRoot = bSequence ? CastChecked<UAnimSequence>(Asset)->bEnableRootMotion : CastChecked<UBlendSpace>(Asset)->bLoop;
		if (bPublicRoot)
		{
			using namespace UE::UAI;
			TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
			TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
			if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return false;
			const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-kit-finalreadback/native") / Name / FGuid::NewGuid().ToString(EGuidFormats::Digits));
			auto Export = [&](const FString& Path) {
				auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Path); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
				const auto R = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), Args, TEXT("root-animation-export"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
				if (!R.IsOk()) Test.AddError(R.GetErrorJson()); return R.IsOk();
			};
			if (!Export(Workspace / TEXT("edit"))) return false;
			TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Workspace / TEXT("edit")), TEXT("*.hjson"), true, false); int32 Changed = 0;
			for (const FString& File : Files) {
				if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
				FString Text; TSharedPtr<FJsonObject> Doc; if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Doc, Error)) return false;
				if (!Doc->HasTypedField<EJson::Object>(TEXT("categories"))) continue;
				for (const auto& Category : Doc->GetObjectField(TEXT("categories"))->Values) {
					const auto Values = Category.Value->AsObject(); if (!Values->HasTypedField<EJson::Object>(TEXT("bLoop"))) continue;
					Values->GetObjectField(TEXT("bLoop"))->SetBoolField(TEXT("value"), !InitialRoot); ++Changed;
					if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Doc.ToSharedRef(), Text, Error) || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
				}
			}
			if (!Test.TestEqual(TEXT("one visible root property edited"), Changed, 1)) return false;
			auto SourceDoc = MakeShared<FJsonObject>(); SourceDoc->SetStringField(TEXT("kind"), TEXT("directory")); SourceDoc->SetStringField(TEXT("path"), Workspace / TEXT("edit"));
			auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), SourceDoc);
			TWeakObjectPtr<UAnimationAsset> Old = Asset; Asset = nullptr; Package = nullptr;
			const auto Imported = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), Args, TEXT("root-animation-import"));
			if (!Imported.IsOk()) { Test.AddError(Imported.GetErrorJson()); return false; }
			UBlendSpace* Current = LoadObject<UBlendSpace>(nullptr, *ObjectPath); TArray<uint8> Disk;
			const bool Verified = !Old.IsValid() && Current && !Current->GetOutermost()->IsDirty() && Current->bLoop == !InitialRoot
				&& NativeBefore.Equals(CaptureAnimationCurveNative(*Current)) && FFileHelper::LoadFileToArray(Disk, *Filename) && Disk != DiskBefore
				&& Export(Workspace / TEXT("readback"));
			Test.AddInfo(FString::Printf(TEXT("root coordinator public: replaced_saved_changed_root_preserved_samples_export=%d"), Verified));
			return Test.TestTrue(TEXT("public root coordinator performs real persistent readback"), Verified);
		}

		float DirtyRate = 0.0f;
		bool DirtySync = false;
		if (bSequence)
		{
			UAnimSequence* Sequence = CastChecked<UAnimSequence>(Asset);
			DirtyRate = Sequence->RateScale + 0.375f;
			Sequence->RateScale = DirtyRate;
		}
		else
		{
			UBlendSpace* Blend = CastChecked<UBlendSpace>(Asset);
			DirtySync = !Blend->bAllowMarkerBasedSync;
			Blend->bAllowMarkerBasedSync = DirtySync;
		}
		Asset->MarkPackageDirty();
		const TWeakObjectPtr<UAnimationAsset> Original = Asset;
		if (bRetainNativeConsumer) NativeConsumer.Reset(Asset);
		FAssetApplyRequest Request;
		Request.Kind = bSequence ? EAssetKind::AnimSequence : EAssetKind::BlendSpace;
		Request.Operation = UeAgentApplyInputContract::EOperation::Update;
		Request.AssetPath = PackageName;
		Request.bCompile = true;
		Request.bSave = true;
		auto Property = MakeShared<FJsonObject>();
		Property->SetStringField(TEXT("property_path"), bSequence ? TEXT("bEnableRootMotion") : TEXT("bLoop"));
		Property->SetBoolField(TEXT("value"), !InitialRoot);
		Request.Properties.Add(MakeShared<FJsonValueObject>(Property));
		bool ObservedPublished = false;
		Request.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
		{
			UAnimationAsset* Published = LoadObject<UAnimationAsset>(nullptr, *ObjectPath);
			TArray<uint8> DiskPublished;
			const bool ChangedRoot = Published && (bSequence ? CastChecked<UAnimSequence>(Published)->bEnableRootMotion : CastChecked<UBlendSpace>(Published)->bLoop) == !InitialRoot;
			const bool PreservedDirty = Published && (bSequence ? CastChecked<UAnimSequence>(Published)->RateScale == DirtyRate : CastChecked<UBlendSpace>(Published)->bAllowMarkerBasedSync == DirtySync);
			ObservedPublished = Published && (bRetainNativeConsumer ? Published != Original.Get() : !Original.IsValid()) && ChangedRoot && PreservedDirty
				&& !Published->GetOutermost()->IsDirty() && NativeBefore.Equals(CaptureAnimationCurveNative(*Published))
				&& (!bSequence || (CastChecked<UAnimSequence>(Published)->IsCompressedDataValid()
					&& !CastChecked<UAnimSequence>(Published)->IsCompressedDataOutOfDate()))
				&& FFileHelper::LoadFileToArray(DiskPublished, *Filename) && DiskPublished != DiskBefore;
			Failure.Diagnostic.Code = TEXT("test_animation_curve_late_rejection");
			Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
			return false;
		};
		Asset = nullptr;
		Package = nullptr;
		FAssetApplyResult Result;
		Test.TestFalse(TEXT("the real Animation provider rejects after persistent readback"), ApplyAsset(Request, Result, TEXT("animation-curve-late")));
		Test.TestEqual(TEXT("late rejection retains its diagnostic"), Result.Diagnostic.Code, FString(TEXT("test_animation_curve_late_rejection")));
		Test.TestTrue(TEXT("publication reloads changed native root while preserving authored data"), ObservedPublished);
		Test.TestTrue(TEXT("the common checkpoint reports successful recovery"), Result.bRollbackAttempted && Result.bRollbackSucceeded);
		UAnimationAsset* Restored = LoadObject<UAnimationAsset>(nullptr, *ObjectPath);
		TArray<uint8> DiskAfter;
		Test.TestTrue(TEXT("recovery preserves native skeleton and authored sample/track data"), Restored && NativeBefore.Equals(CaptureAnimationCurveNative(*Restored)));
		Test.TestTrue(TEXT("restored current package regains its original dirty flag"), Restored && Restored->GetOutermost()->IsDirty());
		Test.TestTrue(TEXT("dirty memory restores separately from the saved package"), Restored && Restored->GetOutermost()->IsDirty()
			&& (bSequence ? CastChecked<UAnimSequence>(Restored)->bEnableRootMotion : CastChecked<UBlendSpace>(Restored)->bLoop) == InitialRoot
			&& (bSequence ? CastChecked<UAnimSequence>(Restored)->RateScale == DirtyRate : CastChecked<UBlendSpace>(Restored)->bAllowMarkerBasedSync == DirtySync)
			&& FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore);
		return !Test.HasAnyErrors();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208BlendSpaceAtomicTest, "UAI.Root.T208.AnimationCurve.BlendSpaceAtomic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208BlendSpaceAtomicTest::RunTest(const FString&) { return RunAnimationCurveAtomic(*this, false); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208BlendSpaceRetainedConsumerAtomicTest, "UAI.Root.T208.AnimationCurve.BlendSpaceRetainedConsumerAtomic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208BlendSpaceRetainedConsumerAtomicTest::RunTest(const FString&) { return RunAnimationCurveAtomic(*this, false, false, true); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208AnimSequenceAtomicTest, "UAI.Root.T208.AnimationCurve.AnimSequenceAtomic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208AnimSequenceAtomicTest::RunTest(const FString&) { return RunAnimationCurveAtomic(*this, true); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208AnimationCloneConfirmationTest, "UAI.Root.T208.AnimationCurve.CloneConfirmation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208AnimationCloneConfirmationTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	using namespace CommandEdge;
	const FString Source = TEXT("/Game/Characters/PaladinRootMotion/Animations/sword_and_shield_idle_RM.sword_and_shield_idle_RM");
	const FString Root = TEXT("/Game/UAI/Transient/T85AnimationClone_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Target = Root + TEXT("/Copy.Copy");
	const FString ChangedTarget = Root + TEXT("/Changed.Changed");
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	UE::UAI::Contract::FDiagnostic Error;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)
		|| !Registry.IsValid()) { AddError(Error.ToPublicDetail()); return false; }
	const auto* Frozen = Registry->FindExact(TEXT("asset.object.clone"));
	if (!TestNotNull(TEXT("the real clone command contract is registered"), Frozen)) return false;
	TSharedPtr<DomainAdapters::AssetDestructiveImport::IUeAgentAssetDeleteOwner> DeleteOwner;
	UE::UAI::Contract::FDiagnostic DeleteOwnerError;
	if (!DomainAdapters::AssetDestructiveImport::TryCreateAssetDeleteOwner(FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T208/CloneDelete"), TEXT("clone-confirmation"), DeleteOwner, DeleteOwnerError)) return false;
	ON_SCOPE_EXIT
	{
		for (const FString& Path : {Target, ChangedTarget})
			if (FindObject<UObject>(nullptr, *Path))
			{
				const auto Result = DeleteOwner->Delete({TEXT("clone-cleanup"), {Path}, false, TEXT("t208")}, MakeShared<FAnimationCurveNeverCancelled>());
				TestTrue(TEXT("owned cloned fixture cleans through the formal delete owner"), Result.IsSuccess());
			}
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, FPackageName::ObjectPathToPackageName(Target));
	};
	auto Submit = [&](const FString& Destination, bool Confirm, bool ExpectedSuccess, bool IncludeConfirm = true)
	{
		const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("source_ref"), Source);
		Params->SetStringField(TEXT("target_ref"), Destination);
		if (IncludeConfirm) Params->SetBoolField(TEXT("confirm"), Confirm);
		const TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetStringField(TEXT("schema"), TEXT("uai.command.v1"));
		Envelope->SetNumberField(TEXT("version"), 1);
		Envelope->SetStringField(TEXT("message_kind"), TEXT("command_request"));
		Envelope->SetStringField(TEXT("correlation_id"), Id);
		Envelope->SetStringField(TEXT("request_id"), Id);
		Envelope->SetStringField(TEXT("command_id"), TEXT("asset.object.clone"));
		Envelope->SetStringField(TEXT("command_binding_hash"), Frozen->GetContractHash());
		Envelope->SetObjectField(TEXT("params"), Params);
		FString Json;
		FJsonSerializer::Serialize(Envelope, TJsonWriterFactory<>::Create(&Json));
		FTCHARToUTF8 Encoded(*Json);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		TOptional<FUeAgentCommandRequest> Request;
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		if (!FUeAgentCommandProtocolCodec().DecodeRequest(Bytes, Request, Diagnostic) || !Request.IsSet()) { AddError(Diagnostic.Code); return false; }
		const auto Result = OperationOwners::DestructiveContent::SubmitProductionDestructiveContentOperation(Request.GetValue(),
			Contract::FUeAgentCallerContext(TEXT("t208-clone-confirmation"), {TEXT("uai:query"), TEXT("uai:operate")}, Id), MakeShared<FAnimationCurveNeverCancelled>());
		TestTrue(TEXT("the delegated result retains request identity"), Result.MatchesRequest(Request.GetValue()));
		if (Result.IsOk() != ExpectedSuccess) { AddError(Result.GetErrorJson()); return false; }
		return true;
	};
	auto IsAbsent = [](const FString& Path)
	{
		return !FindObject<UObject>(nullptr, *Path) && !FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path));
	};
	if (!Submit(Target, false, true)) return false;
	TestTrue(TEXT("explicit preview does not create an asset"), IsAbsent(Target));
	if (!Submit(ChangedTarget, true, true)) return false;
	TestFalse(TEXT("clone does not require a preceding matching confirmation"), IsAbsent(ChangedTarget));
	if (!Submit(Target, true, true, false)) return false;
	UAnimSequence* Clone = LoadObject<UAnimSequence>(nullptr, *Target);
	TestTrue(TEXT("direct clone creates a persistent native AnimSequence"), Clone && FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Target)) && !Clone->GetOutermost()->IsDirty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208AnimationRootWorkspaceTest, "UAI.Root.T208.AnimationCurve.RootPropertyWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208AnimationRootWorkspaceTest::RunTest(const FString&) { return RunAnimationCurveAtomic(*this, false, true); }
