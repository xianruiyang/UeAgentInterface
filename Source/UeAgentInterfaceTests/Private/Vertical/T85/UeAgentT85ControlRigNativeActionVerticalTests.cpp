#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "ControlRig.h"
#include "ControlRigBlueprint.h"
#include "ControlRigBlueprintFactory.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphDomainMechanics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/RigVMEdGraph.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "QueryOwners/AnimationRigGraph/UeAgentAnimationRigGraphCandidateQuery.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/RigVMGraph.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
	using namespace UE::UAI;

	constexpr const TCHAR* ControlRigBlueprintType =
		TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint");
	constexpr const TCHAR* ExpectedUnitType =
		TEXT("/Script/ControlRig.RigUnit_AddAvailableSpaces");
	constexpr const TCHAR* Fingerprint =
		TEXT("sha256:4444444444444444444444444444444444444444444444444444444444444444");

	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
			UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	bool SaveBlueprint(
		UPackage& Package,
		UControlRigBlueprint& Blueprint,
		const FString& Filename)
	{
		Package.MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const bool bSaved = UPackage::SavePackage(
			&Package, &Blueprint, *Filename, Args);
		if (bSaved)
		{
			Package.SetDirtyFlag(false);
		}
		return bSaved;
	}

	bool TryBuildResolvedControlRigImportParams(
		const FString& Target,
		const FString& ExpectedRevision,
		const FString& CandidateId,
		FString& OutParams,
		FString& OutError)
	{
		const TSharedRef<FJsonObject> Candidate = MakeShared<FJsonObject>();
		Candidate->SetStringField(TEXT("id"), CandidateId);
		const TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
		Position->SetNumberField(TEXT("x"), 240.0);
		Position->SetNumberField(TEXT("y"), 144.0);
		const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetObjectField(TEXT("candidate"), Candidate);
		Value->SetObjectField(TEXT("position"), Position);

		const TSharedRef<FJsonObject> Edit = MakeShared<FJsonObject>();
		Edit->SetArrayField(TEXT("depends_on"), {});
		Edit->SetStringField(TEXT("intent"), TEXT("create"));
		Edit->SetStringField(
			TEXT("semantic_id"), TEXT("rigvm_node:T85Native"));
		Edit->SetObjectField(TEXT("value"), Value);

		const TSharedRef<FJsonObject> NativeParams = MakeShared<FJsonObject>();
		NativeParams->SetArrayField(
			TEXT("edits"), {MakeShared<FJsonValueObject>(Edit)});
		const TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
		Operation->SetStringField(
			TEXT("command_id"),
			TEXT("animation.control_rig.hierarchy_graph.apply"));
		Operation->SetObjectField(TEXT("params"), NativeParams);
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), ControlRigBlueprintType);
		Root->SetObjectField(TEXT("operation"), Operation);
		const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(
			TEXT("schema"), TEXT("uai.animation_rig_graph.edit/1"));
		Document->SetObjectField(TEXT("root"), Root);
		FString DocumentText;
		if (!CommandContract::Json::TryCanonicalize(
				Document, DocumentText, OutError))
		{
			return false;
		}

		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), TEXT("index.hjson"));
		Entry->SetStringField(TEXT("document"), DocumentText);
		Entry->SetStringField(TEXT("content_hash"), Fingerprint);
		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("resolved"));
		Source->SetStringField(TEXT("carrier"), TEXT("inline"));
		Source->SetStringField(
			TEXT("source_unit"),
			TEXT("t85-controlrig-native-action-vertical"));
		Source->SetStringField(TEXT("target_identity"), Target);
		Source->SetStringField(TEXT("base_revision"), TEXT(""));
		Source->SetStringField(TEXT("entry_file"), TEXT("index.hjson"));
		Source->SetStringField(TEXT("source_fingerprint"), Fingerprint);
		Source->SetStringField(TEXT("semantic_fingerprint"), Fingerprint);
		Source->SetArrayField(
			TEXT("documents"), {MakeShared<FJsonValueObject>(Entry)});

		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetStringField(TEXT("expected_revision"), ExpectedRevision);
		Params->SetObjectField(TEXT("source"), Source);
		return CommandContract::Json::TryCanonicalize(
			Params, OutParams, OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85ControlRigNativeActionVerticalTest,
	"UAI.Root.T85.ControlRig.NativeActionVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85ControlRigNativeActionVerticalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::QueryOwners::AnimationRigGraph;

	const FString PackageName =
		TEXT("/Game/UAI/Transient/T85ControlRigNativeAction/CR_NativeAction_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	DeleteAssetIfPresent(ObjectPath);
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
			*this, PackageName);
	};

	UPackage* const Package = CreatePackage(*PackageName);
	UControlRigBlueprintFactory* const Factory =
		NewObject<UControlRigBlueprintFactory>();
	Factory->ParentClass = UControlRig::StaticClass();
	UControlRigBlueprint* const Blueprint = Package
		? Cast<UControlRigBlueprint>(Factory->FactoryCreateNew(
			UControlRigBlueprint::StaticClass(),
			Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional,
			nullptr,
			GWarn))
		: nullptr;
	if (!TestNotNull(
			TEXT("a persistent Control Rig Blueprint fixture is created"),
			Blueprint))
	{
		return false;
	}
	Blueprint->InitializeModelIfRequired(false);
	FAssetRegistryModule::AssetCreated(Blueprint);
	if (!TestTrue(
			TEXT("the initial Control Rig Blueprint fixture is saved"),
			SaveBlueprint(*Package, *Blueprint, Filename)))
	{
		return false;
	}

	FAnimationRigGraphCandidateQueryResult CandidateResult;
	FAnimationRigGraphCandidateQueryError CandidateError;
	if (!TestTrue(
			TEXT("search enumerates the native Control Rig blank-context menu"),
			TryQueryAnimationRigGraphCandidates(
				*Blueprint,
				TEXT("Add Spaces"),
				CandidateResult,
				CandidateError,
				64,
				0)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.Detail);
		return false;
	}
	const FAnimationRigGraphCandidate* const Selected =
		CandidateResult.Candidates.FindByPredicate(
			[](const FAnimationRigGraphCandidate& Candidate)
			{
				return Candidate.bSupported
					&& Candidate.Name.Equals(
						TEXT("Add Spaces"), ESearchCase::CaseSensitive);
			});
	if (!TestNotNull(
			TEXT("the exact native Add Spaces action is searchable"),
			Selected))
	{
		return false;
	}
	TestTrue(TEXT("the public Control Rig selector is a compact id"),
		Selected->Id.StartsWith(TEXT("candidate:")));
	TestTrue(TEXT("search covers the complete native blank context"),
		CandidateResult.bCoverageComplete
			&& CandidateResult.NativeActionCount > 0);
	TestFalse(TEXT("non-unit RigVM actions keep creation coverage partial"),
		CandidateResult.bCreationCoverageComplete);
	TestFalse(TEXT("native Control Rig search leaves the package clean"),
		Package->IsDirty());

	FString CanonicalParams;
	FString ParamsError;
	if (!TestTrue(
			TEXT("id-only Control Rig HJSON canonicalizes without a struct binding"),
			TryBuildResolvedControlRigImportParams(
				PackageName,
				CandidateResult.TargetRevision,
				Selected->Id,
				CanonicalParams,
				ParamsError)))
	{
		AddError(ParamsError);
		return false;
	}

	FString ImportPayloadJson;
	bool bReadOnly = true;
	HjsonWorkspace::FWorkspaceError ImportError;
	if (!TestTrue(
			TEXT("id-only HJSON replays the same native RigVM menu action"),
			DomainAdapters::AnimationRigGraph::TryExecuteContentGraph(
				TEXT("content_graph.import_workspace"),
				PackageName,
				CanonicalParams,
				ImportPayloadJson,
				bReadOnly,
				ImportError)))
	{
		AddError(ImportError.GetCode() + TEXT(":")
			+ ImportError.GetSourceUnit() + TEXT(":")
			+ ImportError.GetJsonPointer());
		return false;
	}
	TestFalse(TEXT("the Control Rig import is a mutation"), bReadOnly);
	TSharedPtr<FJsonObject> ImportPayload;
	FString PayloadError;
	if (!TestTrue(
			TEXT("the Control Rig import payload is canonical JSON"),
			CommandContract::Json::TryParseObject(
				ImportPayloadJson, ImportPayload, PayloadError)))
	{
		AddError(PayloadError);
		return false;
	}
	FString TerminalStatus;
	bool bSaved = false;
	bool bReloaded = false;
	TestTrue(TEXT("the import reports the persisted reload lifecycle"),
		ImportPayload.IsValid()
			&& ImportPayload->TryGetStringField(
				TEXT("terminal_status"), TerminalStatus)
			&& TerminalStatus == TEXT("succeeded")
			&& ImportPayload->TryGetBoolField(TEXT("saved"), bSaved)
			&& bSaved
			&& ImportPayload->TryGetBoolField(TEXT("reloaded"), bReloaded)
			&& bReloaded);

	UControlRigBlueprint* const Reloaded =
		LoadObject<UControlRigBlueprint>(nullptr, *ObjectPath);
	if (!TestNotNull(
			TEXT("the persisted Control Rig independently reloads"), Reloaded))
	{
		return false;
	}
	URigVMGraph* const ReloadedModel = Reloaded->GetDefaultModel();
	URigVMUnitNode* PersistedUnit = nullptr;
	if (ReloadedModel)
	{
		for (URigVMNode* Node : ReloadedModel->GetNodes())
		{
			if (Node && Node->GetFName() == FName(TEXT("T85Native")))
			{
				PersistedUnit = Cast<URigVMUnitNode>(Node);
				break;
			}
		}
	}
	if (!TestNotNull(
			TEXT("one native action-created RigVM unit survives reload"),
			PersistedUnit))
	{
		return false;
	}
	TestNotNull(TEXT("the persisted unit owns a script struct"),
		PersistedUnit->GetScriptStruct());
	if (PersistedUnit->GetScriptStruct())
	{
		TestEqual(TEXT("the searched action created the intended unit type"),
			PersistedUnit->GetScriptStruct()->GetPathName(),
			FString(ExpectedUnitType));
	}
	TestEqual(TEXT("the native method survives reload"),
		PersistedUnit->GetMethodName(), FName(TEXT("Execute")));
	TestTrue(TEXT("the persisted unit retains native pins"),
		!PersistedUnit->GetPins().IsEmpty());
	TestTrue(TEXT("placement remains outside candidate and persists on X"),
		FMath::IsNearlyEqual(PersistedUnit->GetPosition().X, 240.0));
	TestTrue(TEXT("placement remains outside candidate and persists on Y"),
		FMath::IsNearlyEqual(PersistedUnit->GetPosition().Y, 144.0));
	URigVMEdGraph* const ReloadedEditorGraph = Cast<URigVMEdGraph>(
		Reloaded->GetEdGraph(ReloadedModel));
	TestNotNull(TEXT("the persisted model node has an editor graph node"),
		ReloadedEditorGraph
			? ReloadedEditorGraph->FindNodeForModelNodeName(
				FName(TEXT("T85Native")))
			: nullptr);

	DeleteAssetIfPresent(ObjectPath);
	UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
		*this, PackageName);
	TestFalse(TEXT("the task-owned Control Rig fixture leaves no asset file"),
		IFileManager::Get().FileExists(*Filename));
	return !HasAnyErrors();
}
