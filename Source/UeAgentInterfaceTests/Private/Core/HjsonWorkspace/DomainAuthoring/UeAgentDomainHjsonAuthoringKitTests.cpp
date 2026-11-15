#include "Misc/AutomationTest.h"

#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Containers/StringConv.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Engine/Texture2D.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

namespace UE::UAI::Tests::DomainHjsonAuthoring
{
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;

	const FDomainHjsonDocumentContract FixtureContract(
		TEXT("fixture-domain"),
		TEXT("uai.fixture.edit/1"),
		TEXT("/Script/CoreUObject.Object"));

	UE::UAI::ContextResource::FUeAgentResourceTracker MakeExportResources()
	{
		using namespace ContextResource;
		return FUeAgentResourceTracker();
	}

	TSharedRef<FJsonObject> ExportDocument(
		const FString& Schema = TEXT("uai.fixture.edit/1"),
		const FString& RootType = TEXT("/Script/CoreUObject.Object"))
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), RootType);
		Root->SetArrayField(
			TEXT("properties"), TArray<TSharedPtr<FJsonValue>>{});
		const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("schema"), Schema);
		Document->SetObjectField(TEXT("root"), Root);
		return Document;
	}

	FString Params(
		const FString& Target = TEXT("/Game/UAI/Fixture"),
		const FString& TargetIdentity = TEXT("/Game/UAI/Fixture"),
		const FString& Entry = TEXT("root.hjson"),
		const FString& Schema = TEXT("uai.fixture.edit/1"),
		const FString& RootType = TEXT("/Script/CoreUObject.Object"),
		const bool bDuplicateEntry = false,
		const FString& Carrier = TEXT("inline"),
		const FString& BaseRevision = FString(),
		const FString& DocumentOverride = FString())
	{
		const FString Document = DocumentOverride.IsEmpty()
			? FString::Printf(
				TEXT("{\"schema\":\"%s\",\"root\":{")
				TEXT("\"type\":\"%s\",\"properties\":[]}}"),
				*Schema,
				*RootType)
			: DocumentOverride;
		const TSharedRef<FJsonObject> PublicSource = MakeShared<FJsonObject>();
		PublicSource->SetStringField(TEXT("kind"), TEXT("inline"));
		PublicSource->SetStringField(TEXT("document"), Document);
		auto Resources = MakeExportResources();
		FResolvedContentGraphImportSourcePtr Resolved;
		FWorkspaceError Error;
		if (!TryLoadContentGraphImportSource(
				PublicSource,
				TargetIdentity,
				Resources,
				Resolved,
				Error)
			|| !Resolved.IsValid())
		{
			return FString();
		}
		const TSharedRef<FJsonObject> Source =
			Resolved->MakeNormalizedSourceObject();
		Source->SetStringField(TEXT("carrier"), Carrier);
		Source->SetStringField(TEXT("target_identity"), TargetIdentity);
		Source->SetStringField(TEXT("base_revision"), BaseRevision);
		Source->SetStringField(TEXT("entry_file"), Entry);
		TArray<TSharedPtr<FJsonValue>> Documents =
			Source->GetArrayField(TEXT("documents"));
		const TSharedPtr<FJsonObject> EntryDocument = Documents[0]->AsObject();
		EntryDocument->SetStringField(TEXT("path"), Entry);
		if (bDuplicateEntry)
		{
			const TSharedRef<FJsonObject> Duplicate = MakeShared<FJsonObject>();
			Duplicate->Values = EntryDocument->Values;
			Documents.Add(MakeShared<FJsonValueObject>(Duplicate));
		}
		Source->SetArrayField(TEXT("documents"), MoveTemp(Documents));
		const TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
		ParamsObject->SetStringField(TEXT("target"), Target);
		ParamsObject->SetStringField(TEXT("expected_revision"), TEXT("rev-1"));
		ParamsObject->SetObjectField(TEXT("source"), Source);
		FString CanonicalParams;
		return TryCanonicalizeObject(ParamsObject, CanonicalParams, Error)
			? CanonicalParams
			: FString();
	}

	void CheckCandidateSelectorAccepted(
		FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& Candidate,
		const FString& Pointer,
		const FString& ExpectedName,
		const int32 ExpectedSelectCount)
	{
		FDomainHjsonCandidateSelector Selector;
		FWorkspaceError Error;
		Test.TestTrue(TEXT("candidate selector is accepted"),
			TryReadCandidateSelector(Candidate, Pointer, Selector, Error));
		Test.TestEqual(TEXT("selector name is trimmed"),
			Selector.Name, ExpectedName);
		Test.TestEqual(TEXT("selector has the expected semantic width"),
			Selector.Select.Num(), ExpectedSelectCount);
		Test.TestTrue(TEXT("accepted selector is valid"), Selector.IsValid());
	}

	void CheckCandidateSelectorRejected(
		FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& Candidate,
		const FString& Pointer,
		const FString& ExpectedCode)
	{
		FDomainHjsonCandidateSelector Selector;
		FWorkspaceError Error;
		Test.TestFalse(TEXT("invalid candidate selector fails closed"),
			TryReadCandidateSelector(Candidate, Pointer, Selector, Error));
		Test.TestEqual(TEXT("candidate selector error is typed"),
			Error.GetCode(), ExpectedCode);
	}

	StateSemantic::FUeAgentCanonicalValueRef RootPropertyValue(
		const FString& Text,
		UE::UAI::ContextResource::FUeAgentResourceTracker& Resources)
	{
		StateSemantic::FUeAgentCanonicalValuePtr Value;
		StateSemantic::FUeAgentStateSemanticError Error;
		check(StateSemantic::FUeAgentCanonicalValue::TryString(
			Text, Resources, Value, Error));
		return Value.ToSharedRef();
	}

	FAuthoringModelRef RootPropertyModel(
		const FString& Value,
		UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
		const FString& BaseRevision = TEXT("generation:root"),
		const FString& PropertyOwnerPath = TEXT("/Game/UAI/Fixture.Fixture"),
		const FString& PropertyPath = TEXT("bUsedWithWater"))
	{
		using namespace StateSemantic;
		const FString DefinitionId = TEXT("definition:material.property");
		const FUeAgentCanonicalIdentity PropertyIdentity(
			TEXT("material.property"), TEXT("authoring_field"), PropertyOwnerPath,
			{{TEXT("owner_path"), PropertyOwnerPath},
				{TEXT("property_path"), PropertyPath},
				{TEXT("notification_owner_path"), PropertyOwnerPath}},
			TEXT("fixture_asset"), TEXT("generation:root"));
		TArray<FSemanticUnit> Units;
		Units.Emplace(
			TEXT("target:root"), ESemanticUnitKind::TargetState, TEXT(""),
			TEXT("ue.fixture.asset"), TEXT("authoring_target"), TEXT(""),
			TMap<FString, FUeAgentCanonicalValueRef>(),
			TArray<FString>({TEXT("properties:root")}), TArray<FString>(),
			TEXT(""), TEXT(""), TArray<FString>(), TArray<FString>({TEXT("state")}),
			TEXT("fixture:root"), TEXT("fixture_asset|fixture_asset|asset_path|root"));
		Units.Emplace(
			TEXT("properties:root"), ESemanticUnitKind::PropertySet,
			TEXT("target:root"), TEXT("material.property"),
			TEXT("root_properties"), TEXT("material.property"),
			TMap<FString, FUeAgentCanonicalValueRef>({
				{TEXT("material.property"), RootPropertyValue(Value, Resources)}}),
			TArray<FString>(), TArray<FString>(), TEXT(""), TEXT(""),
			TArray<FString>({DefinitionId}), TArray<FString>({TEXT("state")}),
			TEXT("fixture:root"), PropertyIdentity.GetCanonicalKey());
		Units.Emplace(
			DefinitionId, ESemanticUnitKind::CollectionState, TEXT("target:root"),
			TEXT("uai.authoring.property_definition/1"), TEXT("property_definition"),
			DefinitionId,
			TMap<FString, FUeAgentCanonicalValueRef>({
				{TEXT("type"), [&Resources]()
				{
					FUeAgentCanonicalValuePtr Type;
					FUeAgentStateSemanticError Error;
					check(FUeAgentCanonicalValue::TryObject(
						{{TEXT("kind"), RootPropertyValue(TEXT("string"), Resources)}},
						Resources, Type, Error));
					return Type.ToSharedRef();
				}()}}),
			TArray<FString>(), TArray<FString>(), TEXT(""), TEXT(""),
			TArray<FString>(), TArray<FString>({TEXT("state")}),
			TEXT("fixture:root"), TEXT(""));
		FAuthoringModelPtr Model;
		FWorkspaceError Error;
		check(TryCompileAuthoringModel(
			TEXT("uai.authoring.composition/2"), 2, TEXT("llm_authoring"),
			BaseRevision, MoveTemp(Units), {TEXT("target:root")},
			Resources, Model, Error));
		return Model.ToSharedRef();
	}

	StateSemantic::FUeAgentCanonicalSnapshotRef RootPropertySnapshot(
		const FString& RootPath,
		UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
		const FString& PropertyPath = TEXT("bUsedWithWater"))
	{
		using namespace StateSemantic;
		const FUeAgentStateRevision Revision(
			TEXT("root:persisted"), TEXT("generation:root"), 1);
		const FUeAgentCanonicalValueRef Before =
			RootPropertyValue(TEXT("before"), Resources);
		const FUeAgentCanonicalIdentity Identity(
			TEXT("material.property"), TEXT("authoring_field"), RootPath,
			{{TEXT("owner_path"), RootPath},
				{TEXT("property_path"), PropertyPath},
				{TEXT("notification_owner_path"), RootPath}},
			TEXT("fixture_asset"), TEXT("generation:root"));
		TArray<FUeAgentStateEntry> Entries;
		Entries.Emplace(
			Identity,
			FUeAgentCanonicalType(
				TEXT("string"), EUeAgentValueKind::String, false, TEXT("scalar")),
			Before,
			FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::Schema, Identity.GetCanonicalKey(),
				TEXT("fixture:root"), Before),
			FUeAgentStateDetails(
				TEXT("Root property"), TEXT(""), {TEXT("RootProperty")}, true,
				TEXT("available"), {}),
			Revision, EUeAgentAuthoredPresence::Present,
			EUeAgentOverrideState::Overridden);
		FUeAgentCanonicalSnapshotPtr Snapshot;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalSnapshot::TryCreate(
			TEXT("snapshot:root"), EUeAgentStatePlane::Persisted, Revision,
			FUeAgentTemporalPoint(TEXT("fixture"), TEXT("1")),
			FUeAgentStateProvenance(
				TEXT("independent_readback"), TEXT("fixture:root"),
				TEXT("evidence:root")),
			Entries, Resources, Snapshot, Error));
		return Snapshot.ToSharedRef();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonRootPropertyCoordinatorContractTest,
	"UAI.Root.DomainAuthoring.RootPropertyCoordinatorContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainHjsonRootPropertyCoordinatorContractTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	using namespace UE::UAI::StateSemantic;
	using namespace UE::UAI::Tests::DomainHjsonAuthoring;
	(void)Parameters;
	UBlueprint* PropertyOwner = NewObject<UBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
	PropertyOwner->BlueprintDescription = TEXT("before");
	const FString RootPath = PropertyOwner->GetPathName();
	auto Resources = MakeExportResources();
	const FAuthoringModelRef CurrentModel =
		RootPropertyModel(TEXT("before"), Resources, TEXT("generation:root"), RootPath, TEXT("BlueprintDescription"));
	const FAuthoringModelRef EditedModel =
		RootPropertyModel(TEXT("after"), Resources, TEXT("generation:root"), RootPath, TEXT("BlueprintDescription"));
	const FUeAgentCanonicalSnapshotRef Snapshot =
		RootPropertySnapshot(RootPath, Resources, TEXT("BlueprintDescription"));
	auto MakeWorkspace = [&Snapshot, &EditedModel](TArray<FStructuralEdit> StructuralEdits)
	{
		return MakeShared<FResolvedSemanticWorkspace>(
			EWorkspaceSourceKind::Directory, ESemanticWorkspaceMode::State,
			EditedModel, Snapshot->GetRevision(), MoveTemp(StructuralEdits),
			TArray<FString>({TEXT("properties.hjson")}), TEXT(""), TEXT(""),
			TArray<FString>(), EditedModel->GetSemanticFingerprint());
	};
	FRootPropertyImportContract Contract;
	Contract.ProviderId = TEXT("fixture-provider");
	Contract.TargetIdentity = RootPath;
	Contract.ObjectPath = RootPath;
	Contract.RequestPrefix = TEXT("fixture-root");
	Contract.PrepareFailedErrorCode = TEXT("fixture_prepare_failed");
	Contract.ApplyFailedErrorCode = TEXT("fixture_apply_failed");
	Contract.ReadbackPayloadErrorCode = TEXT("fixture_readback_invalid");
	Contract.CompileNotSettledErrorCode = TEXT("fixture_compile_not_settled");

	// Callback wiring/error contract only. Production save/reload/rollback is
	// exercised by the public domain workspace tests, not this synthetic fixture.
	for (int32 Mode = 0; Mode != 4; ++Mode)
	{
		bool bApplyCalled = false, bReadbackCalled = false, bReadbackDuringApply = false, bApplying = false;
		FString Payload;
		FWorkspaceError Error;
		const bool Result = TryCoordinateRootPropertyImport(
			MakeWorkspace({}), CurrentModel, Snapshot,
			FUeAgentDescriptorIdentity(TEXT("uai.fixture.root.import"), 1,
				TEXT("sha256:3333333333333333333333333333333333333333333333333333333333333333")),
			RootPath, Resources, Contract,
			[&](const FString& RequestId, TArray<TSharedPtr<FJsonValue>>&& Properties, FRootPropertyApplyOutcome& Outcome)
			{
				bApplyCalled = true;
				if (RequestId.IsEmpty() || Properties.Num() != 1 || !Outcome.FinalReadback) return false;
				if (Mode == 3) { Outcome.Diagnostic.Code = TEXT("fixture_apply_error"); Outcome.Phase = TEXT("apply"); Outcome.Diagnostic.PublicMessage = TEXT("source failure"); return false; }
				if (Mode == 1) { Outcome.bApplied = true; return true; }
				bApplying = true;
				UeAgentAuthoringWorkspaceTransaction::FOperationFailure Failure;
				Outcome.bApplied = Outcome.FinalReadback(Failure);
				bApplying = false;
				if (!Outcome.bApplied) { Outcome.Diagnostic = Failure.Diagnostic; Outcome.Phase = TEXT("final_readback"); }
				return Outcome.bApplied;
			},
			[&](FString& ReadbackPayload, FString& Revision, FWorkspaceError& ReadError)
			{
				bReadbackCalled = true; bReadbackDuringApply = bApplying;
				if (Mode == 2) { ReadError.Set(TEXT("fixture_readback_error")); return false; }
				ReadbackPayload = TEXT("{}"); Revision = TEXT("rev-root-final"); return true;
			}, Payload, Error);
		if (!Result) AddInfo(FString::Printf(TEXT("root coordinator mode=%d error=%s detail=%s"), Mode, *Error.GetCode(), *Error.GetJsonPointer()));
		TestTrue(TEXT("apply receives projected property and final callback"), bApplyCalled);
		TestFalse(TEXT("a synthetic callback cannot prove an executing lifecycle"), Result);
		TestEqual(TEXT("readback is called only by apply"), bReadbackCalled, Mode == 0 || Mode == 2);
		TestEqual(TEXT("readback executes before apply returns"), bReadbackDuringApply, Mode == 0 || Mode == 2);
		TestEqual(TEXT("coordinator preserves typed failure"), Error.GetCode(), FString(Mode == 0
			? TEXT("domain_hjson_readback_lifecycle_evidence_missing") : Mode == 1
			? TEXT("domain_hjson_atomic_readback_not_executed") : Mode == 2 ? TEXT("fixture_readback_error") : TEXT("fixture_apply_error")));
	}

	bool bRejectedApplyCalled = false;
	FString RejectedPayload;
	FWorkspaceError RejectedError;
	TestFalse(TEXT("structural edits are rejected before apply"),
		TryCoordinateRootPropertyImport(
			MakeWorkspace({FStructuralEdit(
				TEXT("add:child"), EStructuralEditKind::Add,
				TEXT("target:root"), TEXT("child"))}),
			CurrentModel, Snapshot,
			FUeAgentDescriptorIdentity(
				TEXT("uai.fixture.root.import"), 1,
				TEXT("sha256:4444444444444444444444444444444444444444444444444444444444444444")),
			RootPath, Resources, Contract,
			[&bRejectedApplyCalled](const FString&,
				TArray<TSharedPtr<FJsonValue>>&&,
				FRootPropertyApplyOutcome&)
			{
				bRejectedApplyCalled = true;
				return true;
			},
			[](FString&, FString&, FWorkspaceError&) { return true; },
			RejectedPayload, RejectedError));
	TestFalse(TEXT("rejected structural edit never reaches apply"),
		bRejectedApplyCalled);
	TestEqual(TEXT("structural edit rejection is typed"),
		RejectedError.GetCode(),
		FString(TEXT("hjson_import_root_property_structural_edits_unsupported")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonDirectoryExportPlanTest,
	"UAI.Root.T62.DomainAuthoring.DirectoryExportPlan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainHjsonDirectoryExportPlanTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	using namespace UE::UAI::Tests::DomainHjsonAuthoring;
	(void)Parameters;
	UE::UAI::ContextResource::FUeAgentResourceTracker Resources =
		MakeExportResources();
	FExportWorkspacePlanPtr Plan;
	FWorkspaceError Error;
	TestTrue(TEXT("a domain-owned directory export plan builds"),
		TryBuildDomainHjsonDirectoryExportPlan(
			TEXT("/Game/UAI/Fixture.Fixture"),
			FixtureContract,
			TEXT("rev-1"),
			TEXT("fixture-profile"),
			TEXT("fixture-provider"),
			TEXT("root.hjson"),
			ExportDocument(),
			Resources,
			Plan,
			Error));
	if (!Plan.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("the plan is a directory workspace"),
		Plan->GetWorkspaceKind(), EWorkspaceKind::Directory);
	TestEqual(TEXT("the entry identity is exact"),
		Plan->GetEntryFile(), FString(TEXT("root.hjson")));
	TestEqual(TEXT("manifest plus one entry are planned"),
		Plan->GetFiles().Num(), 2);
	const FExportFilePlan* Manifest = Plan->GetFiles().FindByPredicate(
		[](const FExportFilePlan& File)
		{
			return File.GetRelativePath() == TEXT(".uai/workspace.hjson");
		});
	const FExportFilePlan* Entry = Plan->GetFiles().FindByPredicate(
		[](const FExportFilePlan& File)
		{
			return File.GetRelativePath() == TEXT("root.hjson");
		});
	TestNotNull(TEXT("the workspace manifest is planned"), Manifest);
	TestNotNull(TEXT("the unique entry document is planned"), Entry);
	if (Manifest)
	{
		const FUTF8ToTCHAR Text(
			reinterpret_cast<const ANSICHAR*>(Manifest->GetCanonicalUtf8().GetData()),
			Manifest->GetCanonicalUtf8().Num());
		const FString ManifestText(Text.Length(), Text.Get());
		TestTrue(TEXT("the manifest schema is exact"),
			ManifestText.Contains(TEXT("uai.content_graph.workspace/1")));
		TestTrue(TEXT("the manifest target is exact"),
			ManifestText.Contains(TEXT("/Game/UAI/Fixture.Fixture")));
		TestTrue(TEXT("the manifest base revision is exact"),
			ManifestText.Contains(TEXT("\"base_revision\":\"rev-1\"")));
		TestTrue(TEXT("the manifest entry is exact"),
			ManifestText.Contains(TEXT("\"entry_file\":\"root.hjson\"")));
		TestTrue(TEXT("the manifest declares an empty reference set"),
			ManifestText.Contains(TEXT("\"references\":[]")));
	}
	if (Entry)
	{
		const FUTF8ToTCHAR Text(
			reinterpret_cast<const ANSICHAR*>(Entry->GetCanonicalUtf8().GetData()),
			Entry->GetCanonicalUtf8().Num());
		const FString EntryText(Text.Length(), Text.Get());
		TestTrue(TEXT("the entry schema is exact"),
			EntryText.Contains(TEXT("uai.fixture.edit/1")));
		TestTrue(TEXT("the entry root type is exact"),
			EntryText.Contains(TEXT("/Script/CoreUObject.Object")));
	}
	TestTrue(TEXT("the plan fingerprint is SHA-256"),
		Plan->GetPlanFingerprint().StartsWith(TEXT("sha256:")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentReadableDomainHjsonDirectoryExportPlanTest,
	"UAI.Root.T62.DomainAuthoring.ReadableDirectoryExportPlan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentReadableDomainHjsonDirectoryExportPlanTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	using namespace UE::UAI::Tests::DomainHjsonAuthoring;
	(void)Parameters;
	UE::UAI::ContextResource::FUeAgentResourceTracker Resources =
		MakeExportResources();
	FExportWorkspacePlanPtr Plan;
	FWorkspaceError Error;
	if (!TestTrue(TEXT("the readable domain plan builds"),
		TryBuildReadableDomainHjsonDirectoryExportPlan(
			TEXT("/Game/UAI/Fixture.Fixture"), FixtureContract,
			TEXT("rev-1"), TEXT("fixture-profile"),
			TEXT("fixture-provider"), TEXT("root.hjson"),
			ExportDocument(), Resources, Plan, Error))
		|| !Plan.IsValid())
	{
		return false;
	}
	const FExportFilePlan* Entry = Plan->GetFiles().FindByPredicate(
		[](const FExportFilePlan& File)
		{
			return File.GetRelativePath() == TEXT("root.hjson");
		});
	const FExportFilePlan* Manifest = Plan->GetFiles().FindByPredicate(
		[](const FExportFilePlan& File)
		{
			return File.GetRelativePath() == TEXT(".uai/workspace.hjson");
		});
	TestNotNull(TEXT("the readable entry is planned"), Entry);
	TestNotNull(TEXT("the standard machine manifest is planned"), Manifest);
	if (Entry)
	{
		const FUTF8ToTCHAR Text(
			reinterpret_cast<const ANSICHAR*>(Entry->GetCanonicalUtf8().GetData()),
			Entry->GetCanonicalUtf8().Num());
		const FString EntryText(Text.Length(), Text.Get());
		TestTrue(TEXT("the model entry is multi-line HJSON"),
			EntryText.Contains(TEXT("\n")));
		TestTrue(TEXT("the model entry uses the common HJSON dialect"),
			EntryText.Contains(TEXT("schema:")));
		UE::UAI::ContextResource::FUeAgentResourceTracker ParseResources =
			MakeExportResources();
		FHjsonDocumentPtr ParsedEntry;
		FWorkspaceError ParseError;
		TestTrue(TEXT("the model entry reparses through the common workspace parser"),
			TryParseHjsonDocument(
				EntryText,
				TEXT("test:readable-domain-entry"),
				ParseResources,
				ParsedEntry,
				ParseError));
		TestTrue(TEXT("the reparsed model entry has an object root"),
			ParsedEntry.IsValid()
				&& ParsedEntry->GetRoot()->GetKind() == EHjsonNodeKind::Object);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonDirectoryExportFailureTest,
	"UAI.Root.T62.DomainAuthoring.DirectoryExportFailures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainHjsonDirectoryExportFailureTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	using namespace UE::UAI::Tests::DomainHjsonAuthoring;
	(void)Parameters;
	auto Rejects = [this](
		const TSharedPtr<FJsonObject>& Document,
		const FString& Entry,
		const FString& ExpectedCode)
	{
		UE::UAI::ContextResource::FUeAgentResourceTracker Resources =
			MakeExportResources();
		FExportWorkspacePlanPtr Plan;
		FWorkspaceError Error;
		TestFalse(*ExpectedCode,
			TryBuildDomainHjsonDirectoryExportPlan(
				TEXT("/Game/UAI/Fixture.Fixture"),
				FixtureContract,
				TEXT("rev-1"),
				TEXT("fixture-profile"),
				TEXT("fixture-provider"),
				Entry,
				Document,
				Resources,
				Plan,
				Error));
		TestEqual(TEXT("the rejection is typed"),
			Error.GetCode(), ExpectedCode);
	};
	Rejects(ExportDocument(TEXT("uai.other/1")), TEXT("root.hjson"),
		TEXT("domain_hjson_export_schema_mismatch"));
	Rejects(ExportDocument(TEXT("uai.fixture.edit/1"), TEXT("/Script/Engine.Actor")),
		TEXT("root.hjson"), TEXT("domain_hjson_export_root_type_mismatch"));
	Rejects(ExportDocument(), TEXT("../root.hjson"),
		TEXT("domain_hjson_export_entry_invalid"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonDirectoryExportStagingVerifyTest,
	"UAI.Root.T62.DomainAuthoring.DirectoryExportStagingVerify",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainHjsonDirectoryExportStagingVerifyTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::ArtifactPublisher;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	using namespace UE::UAI::Tests::DomainHjsonAuthoring;
	(void)Parameters;
	const FString Output = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UAI/Tests/T62DomainExport_"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().DeleteDirectory(*Output, false, true);
	UE::UAI::ContextResource::FUeAgentResourceTracker BuildResources =
		MakeExportResources();
	FExportWorkspacePlanPtr Plan;
	FWorkspaceError Error;
	if (!TestTrue(TEXT("the publish fixture plan builds"),
		TryBuildDomainHjsonDirectoryExportPlan(
			TEXT("/Game/UAI/Fixture.Fixture"), FixtureContract,
			TEXT("rev-1"), TEXT("fixture-profile"),
			TEXT("fixture-provider"), TEXT("root.hjson"),
			ExportDocument(), BuildResources, Plan, Error))
		|| !Plan.IsValid())
	{
		return false;
	}
	UE::UAI::ContextResource::FUeAgentResourceTracker PublishResources =
		MakeExportResources();
	FArtifactPublishReceipt Receipt;
	FArtifactPublishError PublishError;
	const FArtifactPublishRequest Request(
		Plan.ToSharedRef(), Output, TEXT("fixture-domain-export"));
	TestTrue(TEXT("ArtifactPublisher publishes the owned workspace"),
		TryPublishExportPlan(
			Request, PublishResources, Receipt, PublishError));
	UE::UAI::ContextResource::FUeAgentResourceTracker VerifyResources =
		MakeExportResources();
	const auto Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("kind"), TEXT("directory"));
	Source->SetStringField(TEXT("path"), Output);
	FResolvedContentGraphImportSourcePtr Resolved;
	TestTrue(TEXT("generated workspace is accepted by the actual input owner"),
		TryLoadContentGraphImportSource(Source, TEXT("/Game/UAI/Fixture.Fixture"), VerifyResources, Resolved, Error));
	if (Resolved) TestEqual(TEXT("input retains the target revision"), Resolved->GetBaseRevision(), FString(TEXT("rev-1")));
	TestTrue(TEXT("the owned workspace cleanup succeeds"),
		IFileManager::Get().DeleteDirectory(*Output, false, true));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonAuthoringDecodeTest,
	"UAI.Root.T36.DomainAuthoring.DecodeResolvedDocument",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainHjsonAuthoringDecodeTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	using namespace UE::UAI::Tests::DomainHjsonAuthoring;
	(void)Parameters;
	FResolvedDomainHjsonDocument Decoded;
	FWorkspaceError Error;
	const int32 NestedDepth = 257;
	const FString DeepValue = FString::ChrN(NestedDepth, TEXT('['))
		+ TEXT("9007199254740993")
		+ FString::ChrN(NestedDepth, TEXT(']'));
	const FString Document =
		TEXT("{\"schema\":\"uai.fixture.edit/1\",\"root\":{")
		TEXT("\"type\":\"/Script/CoreUObject.Object\",\"properties\":[{")
		TEXT("\"type\":\"integer\",\"value\":9007199254740993,")
		TEXT("\"nested\":{\"label\":\"kept\",\"value\":9007199254740993,")
		TEXT("\"deep\":") + DeepValue + TEXT("}}]}} ");
	TestTrue(TEXT("a canonical resolved source decodes"),
		TryDecodeResolvedDocument(
			TEXT("/Game/UAI/Fixture"),
			Params(
				TEXT("/Game/UAI/Fixture"),
				TEXT("/Game/UAI/Fixture"),
				TEXT("root.hjson"),
				TEXT("uai.fixture.edit/1"),
				TEXT("/Script/CoreUObject.Object"),
				false,
				TEXT("inline"),
				FString(),
				Document),
			FixtureContract,
			Decoded,
			Error));
	TestEqual(TEXT("the exact target is retained"),
		Decoded.TargetIdentity, FString(TEXT("/Game/UAI/Fixture")));
	TestEqual(TEXT("the expected revision is retained"),
		Decoded.ExpectedRevision, FString(TEXT("rev-1")));
	TestTrue(TEXT("the domain root is parsed"), Decoded.Root.IsValid());
	TestEqual(TEXT("the carrier is retained"),
		Decoded.Carrier, FString(TEXT("inline")));
	TestTrue(TEXT("the semantic fingerprint is retained"),
		Decoded.SemanticFingerprint.StartsWith(TEXT("sha256:")));
	const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
	TestTrue(
		TEXT("the actual decoded entry retains properties"),
		Decoded.Root.IsValid()
			&& Decoded.Root->TryGetArrayField(TEXT("properties"), Properties));
	TestEqual(
		TEXT("the actual decoded entry retains one property"),
		Properties ? Properties->Num() : 0,
		1);
	if (Properties && Properties->Num() == 1)
	{
		const TSharedPtr<FJsonObject> Property = (*Properties)[0]->AsObject();
		const TSharedPtr<FJsonValue> Exact = Property.IsValid()
			? Property->TryGetField(TEXT("value"))
			: nullptr;
		FString ExactLexeme;
		TestTrue(
			TEXT("the actual decoded entry keeps number-string representation"),
			Exact.IsValid()
				&& Exact->PreferStringRepresentation()
				&& Exact->TryGetString(ExactLexeme));
		TestEqual(
			TEXT("the actual decoded entry retains the exact integer"),
			ExactLexeme,
			FString(TEXT("9007199254740993")));
		const TSharedPtr<FJsonObject>* Nested = nullptr;
		TestTrue(
			TEXT("the actual decoded entry retains the nested object"),
			Property.IsValid()
				&& Property->TryGetObjectField(TEXT("nested"), Nested));
		if (Nested && Nested->IsValid())
		{
			const TSharedPtr<FJsonValue> NestedExact =
				(*Nested)->TryGetField(TEXT("value"));
			FString NestedLexeme;
			TestTrue(
				TEXT("nested number keeps number-string representation"),
				NestedExact.IsValid()
					&& NestedExact->PreferStringRepresentation()
					&& NestedExact->TryGetString(NestedLexeme));
			TestEqual(
				TEXT("nested exact integer is retained"),
				NestedLexeme,
				FString(TEXT("9007199254740993")));
			TestEqual(
				TEXT("nested string is retained"),
				(*Nested)->GetStringField(TEXT("label")),
				FString(TEXT("kept")));
			TSharedPtr<FJsonValue> Deep = (*Nested)->TryGetField(TEXT("deep"));
			for (int32 Depth = 0; Depth < NestedDepth && Deep.IsValid(); ++Depth)
			{
				if (Deep->Type != EJson::Array)
				{
					Deep.Reset();
					break;
				}
				const TArray<TSharedPtr<FJsonValue>>& Items = Deep->AsArray();
				Deep = Items.Num() == 1 ? Items[0] : nullptr;
			}
			FString DeepLexeme;
			TestTrue(
				TEXT("deep entry projection remains iterative and exact"),
				Deep.IsValid()
					&& Deep->PreferStringRepresentation()
					&& Deep->TryGetString(DeepLexeme));
			TestEqual(
				TEXT("deep exact integer is retained"),
				DeepLexeme,
				FString(TEXT("9007199254740993")));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticDomainHjsonRevisionBindingTest,
	"UeAgentInterface.Core.HjsonWorkspace.DomainAuthoring.SemanticRevisionBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticDomainHjsonRevisionBindingTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::ArtifactPublisher;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	using namespace UE::UAI::Tests::DomainHjsonAuthoring;
	(void)Parameters;

	const FString Target = TEXT("/Game/UAI/Fixture");
	const FString RootPath = TEXT("/Game/UAI/Fixture.Fixture");
	const FString Output = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UAI/Tests/SemanticRevisionBinding_"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().DeleteDirectory(*Output, false, true);

	auto Resources = MakeExportResources();
	const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshotRef CurrentSnapshot =
		RootPropertySnapshot(RootPath, Resources);
	const FAuthoringModelRef CurrentModel =
		RootPropertyModel(
			TEXT("before"), Resources, TEXT("generation:root"), RootPath);
	const FAuthoringModelRef EditedModel =
		RootPropertyModel(
			TEXT("after"), Resources, TEXT("generation:root"), RootPath);
	FExportWorkspacePlanPtr Plan;
	FWorkspaceError Error;
	TestTrue(TEXT("semantic plan binds the capture revision"),
		TryBuildCompositionExportPlan(
			CurrentModel,
			{
				CurrentSnapshot->GetSnapshotId(),
				CurrentSnapshot->GetRevision(),
				TEXT("evidence:semantic-revision-binding"),
			},
			{{TEXT("estimated_tokens"), 1000000}},
			Resources,
			Plan,
			Error));
	if (!Plan.IsValid())
	{
		IFileManager::Get().DeleteDirectory(*Output, false, true);
		return false;
	}
	FArtifactPublishReceipt Receipt;
	FArtifactPublishError PublishError;
	TestTrue(TEXT("semantic plan is published"),
		TryPublishExportPlan(
			FArtifactPublishRequest(
				Plan.ToSharedRef(), Output,
				TEXT("semantic-revision-binding-test")),
			Resources,
			Receipt,
			PublishError));
	const FExportFilePlan* EditedUnitFile = Plan->GetFiles().FindByPredicate(
		[](const FExportFilePlan& File)
		{
			if (File.GetRelativePath() == TEXT(".uai/workspace.hjson"))
			{
				return false;
			}
			const FUTF8ToTCHAR Text(
				reinterpret_cast<const ANSICHAR*>(File.GetCanonicalUtf8().GetData()),
				File.GetCanonicalUtf8().Num());
			return FString(Text.Length(), Text.Get()).Contains(TEXT("before"));
		});
	TestNotNull(TEXT("semantic plan exposes the editable value document"), EditedUnitFile);
	if (EditedUnitFile)
	{
		const FString EditedPath = FPaths::Combine(
			Output, EditedUnitFile->GetRelativePath());
		FString EditedText;
		TestTrue(TEXT("semantic value document can be read"),
			FFileHelper::LoadFileToString(EditedText, *EditedPath));
		EditedText.ReplaceInline(TEXT("before"), TEXT("after"));
		TestTrue(TEXT("semantic value edit is persisted"),
			FFileHelper::SaveStringToFile(
				EditedText,
				*EditedPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	}

	const TSharedRef<FJsonObject> PublicSource = MakeShared<FJsonObject>();
	PublicSource->SetStringField(TEXT("kind"), TEXT("directory"));
	PublicSource->SetStringField(TEXT("path"), Output);
	FResolvedContentGraphImportSourcePtr PublicResolved;
	FString MachineExpectedRevision;
	FString MachineAuthorization;
	TestTrue(TEXT("semantic directory resolves with hidden machine context"),
		TryRecoverContentGraphImportMachineContext(
			PublicSource,
			Target,
			Resources,
			PublicResolved,
			MachineExpectedRevision,
			MachineAuthorization,
			Error));
	TestFalse(TEXT("machine impact authorization remains internal and present"),
		MachineAuthorization.IsEmpty());
	if (!PublicResolved.IsValid())
	{
		IFileManager::Get().DeleteDirectory(*Output, false, true);
		return false;
	}

	const TSharedRef<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
	ParamsObject->SetStringField(TEXT("target"), Target);
	ParamsObject->SetStringField(
		TEXT("expected_revision"), MachineExpectedRevision);
	ParamsObject->SetObjectField(
		TEXT("source"), PublicResolved->MakeNormalizedSourceObject());
	FString CanonicalParams;
	TestTrue(TEXT("semantic import params canonicalize"),
		TryCanonicalizeObject(ParamsObject, CanonicalParams, Error));
	FResolvedDomainHjsonDocument Decoded;
	TestTrue(TEXT("machine-bound semantic source decodes with its hidden baseline"),
		TryDecodeResolvedDocument(
			Target, CanonicalParams, FixtureContract, Decoded, Error));
	TestTrue(TEXT("decoded source is the semantic workspace"),
		Decoded.bSemanticWorkspaceDirectory && Decoded.SemanticWorkspace.IsValid());
	TestEqual(TEXT("decoded expected revision comes from the machine manifest"),
		Decoded.ExpectedRevision, FString(TEXT("generation:root")));
	TestEqual(TEXT("decoded base revision comes from the machine manifest"),
		Decoded.BaseRevision, FString(TEXT("generation:root")));

	if (Decoded.SemanticWorkspace.IsValid())
	{
		FWorkspaceError BindingError;
		TestTrue(TEXT("fresh recapture has the same semantic revision"),
			TryValidateSemanticWorkspaceBinding(
				Decoded.SemanticWorkspace.ToSharedRef(),
				ESemanticWorkspaceMode::State,
				CurrentModel,
				CurrentSnapshot->GetRevision(),
				FString(),
				FString(),
				BindingError));

		const FAuthoringModelRef StaleModel =
			RootPropertyModel(
				TEXT("before"), Resources, TEXT("generation:stale"), RootPath);
		TArray<TSharedPtr<FJsonValue>> StaleProperties;
		UE::UAI::Composition::HjsonImport::FSemanticWorkspaceImportError
			ImportError;
		TestFalse(TEXT("stale recapture is rejected before property projection"),
			UE::UAI::Composition::HjsonImport::TryPrepareRootObjectPropertyChanges(
				Decoded.SemanticWorkspace.ToSharedRef(),
				StaleModel,
				CurrentSnapshot,
				UE::UAI::Contract::FUeAgentDescriptorIdentity(
					TEXT("uai.fixture.semantic.import"), 1,
					TEXT("sha256:5555555555555555555555555555555555555555555555555555555555555555")),
				TEXT("semantic-revision-stale"),
				TEXT("semantic-revision-stale:1"),
				UE::UAI::Contract::FUeAgentCallerContext(
					TEXT("semantic-revision-test"),
					{TEXT("uai:query"), TEXT("uai:operate"),
						TEXT("authoring.write")},
					TEXT("semantic-revision-stale")),
				RootPath,
				Resources,
				StaleProperties,
				ImportError));
		TestEqual(TEXT("stale recapture reports the semantic base mismatch"),
			ImportError.Code,
			FString(TEXT("hjson_import_stale_base_revision")));
		TestEqual(TEXT("stale recapture produces no property projection"),
			StaleProperties.Num(), 0);
	}

	TestTrue(TEXT("semantic revision fixture cleanup succeeds"),
		IFileManager::Get().DeleteDirectory(*Output, false, true));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonAuthoringPrewriteFailureTest,
	"UAI.Root.T36.DomainAuthoring.PrewriteFailures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainHjsonAuthoringPrewriteFailureTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	using namespace UE::UAI::Tests::DomainHjsonAuthoring;
	(void)Parameters;
	auto Rejects = [this](const FString& Input, const FString& Code)
	{
		FResolvedDomainHjsonDocument Decoded;
		FWorkspaceError Error;
		TestFalse(*Code,
			TryDecodeResolvedDocument(
				TEXT("/Game/UAI/Fixture"), Input, FixtureContract, Decoded, Error));
		TestEqual(*FString::Printf(TEXT("%s is exact"), *Code),
			Error.GetCode(), Code);
	};
	Rejects(
		Params(TEXT("/Game/UAI/Fixture"), TEXT("/Game/UAI/Other")),
		TEXT("content_graph_normalized_target_identity_mismatch"));
	Rejects(
		Params(TEXT("/Game/UAI/Fixture"), TEXT("/Game/UAI/Fixture"),
			TEXT("root.hjson"), TEXT("uai.other/1")),
		TEXT("domain_hjson_schema_mismatch"));
	Rejects(
		Params(TEXT("/Game/UAI/Fixture"), TEXT("/Game/UAI/Fixture"),
			TEXT("root.hjson"), TEXT("uai.fixture.edit/1"),
			TEXT("/Script/Engine.Actor")),
		TEXT("domain_hjson_root_type_mismatch"));
	Rejects(
		Params(TEXT("/Game/UAI/Fixture"), TEXT("/Game/UAI/Fixture"),
			TEXT("root.hjson"), TEXT("uai.fixture.edit/1"),
			TEXT("/Script/CoreUObject.Object"), true),
		TEXT("content_graph_normalized_source_invalid"));
	Rejects(
		Params(TEXT("/Game/UAI/Fixture"), TEXT("/Game/UAI/Fixture"),
			TEXT("root.hjson"), TEXT("uai.fixture.edit/1"),
			TEXT("/Script/CoreUObject.Object"), false,
			TEXT("directory"), TEXT("rev-0")),
		TEXT("content_graph_normalized_base_revision_mismatch"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonAuthoringCaptureResultTest,
	"UAI.Root.T36.DomainAuthoring.LifecycleReceipt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainHjsonAuthoringCaptureResultTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainAdapters::ContentGraphRead;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	(void)Parameters;
	FUeAgentContentGraphCapture Capture;
	Capture.DomainId = TEXT("fixture-domain");
	Capture.Revision = TEXT("rev-2");
	FUeAgentContentGraphItem Item;
	Item.ItemId = TEXT("fixture:item");
	Item.ItemKind = TEXT("fixture");
	Item.ClassPath = TEXT("/Script/CoreUObject.Object");
	Capture.Items.Add(MoveTemp(Item));
	TSharedPtr<FJsonObject> Snapshot;
	FWorkspaceError Error;
	TestTrue(TEXT("a canonical capture snapshot builds"),
		TryBuildCaptureSnapshot(
			TEXT("/Game/UAI/Fixture"),
			TEXT("/Script/CoreUObject.Object"), Capture, Snapshot, Error));
	FString Payload;
	FDomainHjsonImportLifecycleReceipt LifecycleReceipt;
	TestFalse(TEXT("an incomplete lifecycle receipt is rejected"),
		TryBuildImportSuccessPayload(
			TEXT("fixture-provider"), TEXT("/Game/UAI/Fixture"),
			Capture.Revision, LifecycleReceipt, Snapshot, nullptr,
			Payload, Error));
	TestEqual(TEXT("the lifecycle failure is exact"),
		Error.GetCode(),
		FString(TEXT("domain_hjson_lifecycle_receipt_incomplete")));
	LifecycleReceipt.bMutationApplied = true;
	LifecycleReceipt.bNativeUpdateSettled = true;
	LifecycleReceipt.bPackageSaved = true;
	LifecycleReceipt.PersistenceReceiptId = TEXT("fixture-persisted:rev-2");
	TestTrue(TEXT("an independent readback result builds"),
		TryBuildImportSuccessPayload(
			TEXT("fixture-provider"), TEXT("/Game/UAI/Fixture"),
			Capture.Revision, LifecycleReceipt, Snapshot, nullptr,
			Payload, Error));
	TestEqual(TEXT("successful public payload is the compact status object"),
		Payload, FString(TEXT("{\"status\":\"succeeded\"}")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonTargetPresenceTest,
	"UAI.Root.T39.DomainAuthoring.TargetPresence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainHjsonTargetPresenceTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	(void)Parameters;
	const FString PackageName = TEXT("/Game/UAI/Tests/T39Presence_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ObjectPath = PackageName + TEXT(".Target");
	UPackage* const EmptyPackage = CreatePackage(*PackageName);
	if (!TestNotNull(TEXT("an empty task-owned package is created"), EmptyPackage))
	{
		return false;
	}

	bool bPresent = true;
	TestTrue(TEXT("an empty loaded package has a valid target identity"),
		TryInspectObjectTargetPresence(ObjectPath, bPresent));
	TestFalse(TEXT("an empty loaded package is not an existing asset"), bPresent);

	UObject* const Target = NewObject<UTexture2D>(
		EmptyPackage, TEXT("Target"), RF_Transient);
	TestNotNull(TEXT("a task-owned target object is created"), Target);
	bPresent = false;
	TestTrue(TEXT("a loaded target has a valid target identity"),
		TryInspectObjectTargetPresence(ObjectPath, bPresent));
	TestTrue(TEXT("a loaded target is reported as present"), bPresent);

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonFailedLoadEmptyPackagePresenceTest,
	"UAI.Root.T180.DomainAuthoring.FailedLoadEmptyPackagePresence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainHjsonFailedLoadEmptyPackagePresenceTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	(void)Parameters;
	const FString PackageName =
		TEXT("/Game/UAI/Experiments/Foundation/T180_ProbeOnly/Absent_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ObjectPath = PackageName + TEXT(".Target");

	const bool bPackageExistsBefore = FPackageName::DoesPackageExist(PackageName);
	UPackage* const PackageBefore = FindPackage(nullptr, *PackageName);
	UObject* const ObjectBefore = StaticFindObject(
		UObject::StaticClass(), nullptr, *ObjectPath);
	UObject* const LoadResult = StaticLoadObject(
		UObject::StaticClass(), nullptr, *ObjectPath);
	UPackage* const PackageAfter = FindPackage(nullptr, *PackageName);
	UObject* const ObjectAfter = StaticFindObject(
		UObject::StaticClass(), nullptr, *ObjectPath);

	AddInfo(FString::Printf(
		TEXT("T180 observations: DoesPackageExistBefore=%s; ")
		TEXT("FindPackageBefore=%s; StaticLoadObject=%s; ")
		TEXT("FindPackageAfter=%s; StaticFindObjectAfter=%s"),
		bPackageExistsBefore ? TEXT("true") : TEXT("false"),
		PackageBefore ? TEXT("non-null") : TEXT("null"),
		LoadResult ? TEXT("non-null") : TEXT("null"),
		PackageAfter ? TEXT("non-null") : TEXT("null"),
		ObjectAfter ? TEXT("non-null") : TEXT("null")));

	TestFalse(TEXT("the unique probe package does not exist on disk"),
		bPackageExistsBefore);
	TestNull(TEXT("the unique probe package is initially unloaded"),
		PackageBefore);
	TestNull(TEXT("the unique probe object is initially absent"), ObjectBefore);
	TestNull(TEXT("loading the nonexistent object fails"), LoadResult);
	TestNull(TEXT("the nonexistent object remains absent"), ObjectAfter);
	if (!TestNotNull(
		TEXT("the failed load leaves an empty package to inspect"), PackageAfter))
	{
		return false;
	}

	bool bPresent = true;
	TestTrue(TEXT("the failed-load target identity remains inspectable"),
		TryInspectObjectTargetPresence(ObjectPath, bPresent));
	TestFalse(TEXT("an empty package is not an existing target"), bPresent);

	ResetLoaders(PackageAfter);
	PackageAfter->MarkAsGarbage();
	CollectGarbage(RF_NoFlags, true);
	TestNull(TEXT("the probe package is unloaded after cleanup"),
		FindPackage(nullptr, *PackageName));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonCandidateSelectorTest,
	"UAI.Root.T85.DomainAuthoring.CandidateSelector",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainHjsonCandidateSelectorTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	using namespace UE::UAI::Tests::DomainHjsonAuthoring;
	(void)Parameters;
	const FString Pointer = TEXT("/edits/0/value/candidate");

	TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
	Candidate->SetStringField(TEXT("name"), TEXT("  Native Cube  "));
	CheckCandidateSelectorAccepted(
		*this, Candidate, Pointer, TEXT("Native Cube"), 0);

	Candidate = MakeShared<FJsonObject>();
	Candidate->SetStringField(TEXT("name"), TEXT("Native Cube"));
	const TSharedRef<FJsonObject> Select = MakeShared<FJsonObject>();
	Select->SetStringField(TEXT("source_kind"), TEXT(" metasound "));
	Select->SetStringField(TEXT("version"), TEXT(" 1.0 "));
	Candidate->SetObjectField(TEXT("select"), Select);
	CheckCandidateSelectorAccepted(
		*this, Candidate, Pointer, TEXT("Native Cube"), 2);
	FDomainHjsonCandidateSelector ParsedSelector;
	FWorkspaceError ParsedError;
	TestTrue(TEXT("semantic selector parses for readback"),
		TryReadCandidateSelector(
			Candidate, Pointer, ParsedSelector, ParsedError));
	FString SourceKind;
	TestTrue(TEXT("semantic key lookup is case insensitive"),
		ParsedSelector.TryGetSelectValue(TEXT("SOURCE_KIND"), SourceKind));
	TestEqual(TEXT("semantic value is trimmed"), SourceKind,
		FString(TEXT("metasound")));

	Candidate = MakeShared<FJsonObject>();
	Candidate->SetStringField(TEXT("ignored_assertion"), TEXT("preserved"));
	CheckCandidateSelectorRejected(*this, Candidate, Pointer,
		TEXT("domain_hjson_candidate_selector_field_unsupported"));

	Candidate = MakeShared<FJsonObject>();
	Candidate->SetStringField(TEXT("id"), TEXT("candidate:cube"));
	CheckCandidateSelectorRejected(*this, Candidate, Pointer,
		TEXT("domain_hjson_candidate_selector_field_unsupported"));

	Candidate = MakeShared<FJsonObject>();
	Candidate->SetStringField(TEXT("name"), TEXT("Native Cube"));
	Candidate->SetStringField(TEXT("candidate_id"), TEXT("candidate:cube"));
	CheckCandidateSelectorRejected(*this, Candidate, Pointer,
		TEXT("domain_hjson_candidate_selector_field_unsupported"));

	Candidate = MakeShared<FJsonObject>();
	Candidate->SetStringField(TEXT("name"), TEXT("Native Cube"));
	const TSharedRef<FJsonObject> MachineSelect = MakeShared<FJsonObject>();
	MachineSelect->SetStringField(TEXT("node_guid"), TEXT("machine-value"));
	Candidate->SetObjectField(TEXT("select"), MachineSelect);
	CheckCandidateSelectorRejected(*this, Candidate, Pointer,
		TEXT("domain_hjson_candidate_selector_select_field_unsupported"));

	Candidate = MakeShared<FJsonObject>();
	Candidate->SetField(TEXT("name"), MakeShared<FJsonValueNumber>(7.0));
	CheckCandidateSelectorRejected(*this, Candidate, Pointer,
		TEXT("domain_hjson_candidate_selector_invalid"));

	Candidate = MakeShared<FJsonObject>();
	Candidate->SetStringField(TEXT("name"), TEXT("Native Cube"));
	Candidate->SetField(TEXT("select"), MakeShared<FJsonValueBoolean>(true));
	CheckCandidateSelectorRejected(*this, Candidate, Pointer,
		TEXT("domain_hjson_candidate_selector_select_invalid"));

	Candidate = MakeShared<FJsonObject>();
	Candidate->SetStringField(TEXT("name"), TEXT("Native Cube"));
	const TSharedRef<FJsonObject> NumericSelect = MakeShared<FJsonObject>();
	NumericSelect->SetField(TEXT("version"), MakeShared<FJsonValueNumber>(1.0));
	Candidate->SetObjectField(TEXT("select"), NumericSelect);
	CheckCandidateSelectorRejected(*this, Candidate, Pointer,
		TEXT("domain_hjson_candidate_selector_select_invalid"));

	Candidate = MakeShared<FJsonObject>();
	Candidate->SetStringField(TEXT("candidate_id"), TEXT("alias"));
	Candidate->SetField(TEXT("name"), MakeShared<FJsonValueNull>());
	CheckCandidateSelectorRejected(*this, Candidate, Pointer,
		TEXT("domain_hjson_candidate_selector_field_unsupported"));

	Candidate = MakeShared<FJsonObject>();
	Candidate->SetStringField(TEXT("name"), TEXT("  \t  "));
	CheckCandidateSelectorRejected(*this, Candidate, Pointer,
		TEXT("domain_hjson_candidate_selector_required"));

	FDomainHjsonCandidateSelector Selector;
	FWorkspaceError Error;
	TestFalse(TEXT("null candidate fails closed"),
		TryReadCandidateSelector(nullptr, Pointer, Selector, Error));
	TestEqual(TEXT("null candidate error is typed"), Error.GetCode(),
		FString(TEXT("domain_hjson_candidate_selector_invalid")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCommonPropertyKindRoundTripTest,
	"UAI.Root.T166.CommonProperty.KindRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCommonPropertyKindRoundTripTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FProperty* const Property = FindFProperty<FProperty>(
		AActor::StaticClass(), TEXT("bCanBeDamaged"));
	if (!TestNotNull(TEXT("representative reflected bool property exists"), Property))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Descriptor =
		UeAgentPropertyValueCodec::BuildTypeDescriptor(Property);
	if (!TestTrue(TEXT("common type descriptor is available"), Descriptor.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("top-level type field is kind"),
		Descriptor->GetStringField(TEXT("kind")), FString(TEXT("bool")));
	TestFalse(TEXT("legacy value_kind field is absent"),
		Descriptor->HasField(TEXT("value_kind")));

	FString TypeError;
	TestTrue(TEXT("common kind metadata validates against reflection"),
		UeAgentPropertyValueCodec::ValidateTypeMetadata(
			Property, Descriptor, TypeError));
	TestTrue(TEXT("successful type validation has no error"),
		TypeError.IsEmpty());

	UObject* const DefaultObject = AActor::StaticClass()->GetDefaultObject();
	const void* const SourceValue =
		Property->ContainerPtrToValuePtr<void>(DefaultObject);
	const TSharedPtr<FJsonValue> Encoded = UeAgentPropertyValueCodec::Encode(
		Property, SourceValue, DefaultObject, UeAgentPropertyValueCodec::FOptions());
	if (!TestTrue(TEXT("representative value is exported"), Encoded.IsValid()))
	{
		return false;
	}

	void* const DecodedValue = FMemory::Malloc(
		Property->GetSize(), Property->GetMinAlignment());
	Property->InitializeValue(DecodedValue);
	FString DecodeError;
	const bool bDecoded = UeAgentPropertyValueCodec::Decode(
		Property, Encoded, DecodedValue, DefaultObject,
		UeAgentPropertyValueCodec::FOptions(), DecodeError);
	TestTrue(TEXT("representative value imports through the common codec"), bDecoded);
	if (bDecoded)
	{
		TestTrue(TEXT("imported value matches the exported value"),
			Property->Identical(SourceValue, DecodedValue));
	}
	Property->DestroyValue(DecodedValue);
	FMemory::Free(DecodedValue);
	return !HasAnyErrors();
}
