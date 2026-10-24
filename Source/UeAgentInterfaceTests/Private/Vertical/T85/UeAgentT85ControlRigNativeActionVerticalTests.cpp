#include "Misc/AutomationTest.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "ControlRig.h"
#include "ControlRigBlueprint.h"
#include "ControlRigBlueprintFactory.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphDomainMechanics.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetDeleteOwner.h"
#include "RigEditor/IKRigController.h"
#include "UObject/StrongObjectPtr.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringDescriptors.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringApplyProvider.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringRegistration.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringExport.h"
#include "EdGraph/RigVMEdGraph.h"
#include "HAL/FileManager.h"
#include "RetargetEditor/IKRetargetFactory.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigDefinitionFactory.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "QueryOwners/AnimationRigGraph/UeAgentAnimationRigGraphCandidateQuery.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/RigVMGraph.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Utilities/AuthoringExport/UeAgentExportContract.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
	using namespace UE::UAI;

	constexpr const TCHAR* ControlRigBlueprintType =
		TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint");
	constexpr const TCHAR* ExpectedUnitType =
		TEXT("/Script/ControlRig.RigUnit_AddAvailableSpaces");

	class FRiggingDeleteNeverCancelled final : public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

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
		const FString& CandidateName,
		FString& OutParams,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		const TSharedRef<FJsonObject> Candidate = MakeShared<FJsonObject>();
		Candidate->SetStringField(TEXT("name"), CandidateName);
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

		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("inline"));
		Source->SetStringField(TEXT("document"), DocumentText);
		HjsonWorkspace::FResolvedContentGraphImportSourcePtr ResolvedSource;
		HjsonWorkspace::FWorkspaceError WorkspaceError;
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		if (!HjsonWorkspace::TryLoadContentGraphImportSource(
				Source, Target, Resources, ResolvedSource, WorkspaceError)
			|| !ResolvedSource.IsValid())
		{
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(
				WorkspaceError.GetCode().IsEmpty() ? TEXT("test_import_source_missing") : WorkspaceError.GetCode(),
				WorkspaceError.GetDiagnostic());
			return false;
		}

		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetStringField(TEXT("expected_revision"), ExpectedRevision);
		Params->SetObjectField(
			TEXT("source"), ResolvedSource->MakeNormalizedSourceObject());
		return CommandContract::Json::TryCanonicalize(
			Params, OutParams, OutError);
	}

	bool TryBuildControlRigExportParams(
		const FString& Target,
		const FString& WorkspacePath,
		FString& OutParams,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetStringField(TEXT("workspace_path"), WorkspacePath);
		Params->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		return CommandContract::Json::TryCanonicalize(Params, OutParams, OutError);
	}

	bool TryBuildControlRigDirectoryImportParams(
		const FString& Target,
		const FString& WorkspacePath,
		FString& OutParams,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("directory"));
		Source->SetStringField(TEXT("path"), WorkspacePath);
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		HjsonWorkspace::FResolvedContentGraphImportSourcePtr ResolvedSource;
		HjsonWorkspace::FWorkspaceError WorkspaceError;
		FString Revision;
		FString Authorization;
		if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(
			Source, Target, Resources, ResolvedSource, Revision, Authorization,
			WorkspaceError) || !ResolvedSource.IsValid())
		{
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(
				WorkspaceError.GetCode().IsEmpty() ? TEXT("test_import_source_missing") : WorkspaceError.GetCode(),
				WorkspaceError.GetDiagnostic());
			return false;
		}
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetStringField(TEXT("expected_revision"), Revision);
		Params->SetObjectField(
			TEXT("source"), ResolvedSource->MakeNormalizedSourceObject());
		return CommandContract::Json::TryCanonicalize(Params, OutParams, OutError);
	}

	void EditControlRigNodePosition(
		const TSharedPtr<FJsonObject>& Object,
		bool& bOutEdited)
	{
		if (!Object.IsValid()) return;
		double DirectX = 0.0;
		double DirectY = 0.0;
		if (Object->TryGetNumberField(TEXT("x"), DirectX)
			&& Object->TryGetNumberField(TEXT("y"), DirectY)
			&& FMath::IsNearlyEqual(DirectX, 240.0)
			&& FMath::IsNearlyEqual(DirectY, 144.0))
		{
			Object->SetNumberField(TEXT("x"), 320.0);
			bOutEdited = true;
		}
		const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
		if (Object->TryGetArrayField(TEXT("components"), Components)
			&& Components && Components->Num() == 2
			&& FMath::IsNearlyEqual((*Components)[0]->AsNumber(), 240.0)
			&& FMath::IsNearlyEqual((*Components)[1]->AsNumber(), 144.0))
		{
			Object->SetArrayField(TEXT("components"), {
				MakeShared<FJsonValueNumber>(320.0),
				MakeShared<FJsonValueNumber>(144.0)});
			bOutEdited = true;
		}
		const TSharedPtr<FJsonObject>* Position = nullptr;
		if (Object->TryGetObjectField(TEXT("position"), Position)
			&& Position && Position->IsValid())
		{
			double X = 0.0;
			double Y = 0.0;
			if ((*Position)->TryGetNumberField(TEXT("x"), X)
				&& (*Position)->TryGetNumberField(TEXT("y"), Y)
				&& FMath::IsNearlyEqual(X, 240.0)
				&& FMath::IsNearlyEqual(Y, 144.0))
			{
				(*Position)->SetNumberField(TEXT("x"), 320.0);
				bOutEdited = true;
			}
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
		{
			if (!Field.Value.IsValid()) continue;
			if (Field.Value->Type == EJson::Object)
				EditControlRigNodePosition(Field.Value->AsObject(), bOutEdited);
			else if (Field.Value->Type == EJson::Array)
			{
				for (const TSharedPtr<FJsonValue>& Item : Field.Value->AsArray())
				{
					if (Item.IsValid() && Item->Type == EJson::Object)
						EditControlRigNodePosition(Item->AsObject(), bOutEdited);
				}
			}
		}
	}

	void EditControlRigRootProperty(const TSharedPtr<FJsonObject>& Object,
		int32& EditedCount, bool& BeforeValue)
	{
		if (!Object.IsValid()) return;
		for (const auto& Field : Object->Values)
		{
			if (!Field.Value.IsValid() || Field.Value->Type != EJson::Object) continue;
			const auto Child = Field.Value->AsObject();
			if (Field.Key == TEXT("bAllowMultipleInstances")
				&& Child->TryGetBoolField(TEXT("value"), BeforeValue))
			{
				Child->SetBoolField(TEXT("value"), !BeforeValue);
				++EditedCount;
			}
			else EditControlRigRootProperty(Child, EditedCount, BeforeValue);
		}
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
				CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.PublicMessage);
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
	TestFalse(TEXT("native Control Rig search leaves the package clean"),
		Package->IsDirty());

	FString CanonicalParams;
	FString ParamsError;
	if (!TestTrue(
			TEXT("name-only Control Rig HJSON canonicalizes without a struct binding"),
			TryBuildResolvedControlRigImportParams(
				PackageName,
				CandidateResult.TargetRevision,
				Selected->Name,
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
			TEXT("name-only HJSON replays the same native RigVM menu action"),
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

	const TCHAR* RequiredRiggingProfiles[] = {
		TEXT("control_rig"),
		TEXT("ik_rig"),
		TEXT("ik_retargeter"),
	};
	for (const TCHAR* TypeId : RequiredRiggingProfiles)
	{
		TestNotNull(
			FString::Printf(
				TEXT("Rigging HJSON profile %s is registered"),
				TypeId),
			UeAgentRiggingAuthoring::FindProfileByTypeId(TypeId));
	}
	UE::UAI::Contract::FDiagnostic RiggingRegistrationError;
	if (!TestTrue(
			TEXT("Rigging authoring export/import owners register"),
			UeAgentRiggingAuthoring::RegisterAuthoring(
				RiggingRegistrationError)))
	{
		AddError(RiggingRegistrationError.ToPublicDetail());
		return false;
	}

	UE::UAI::Contract::FDiagnostic CompositionRegistrationError;
	if (!TestTrue(
			TEXT("shared target compositions register for Rigging"),
			HjsonWorkspace::RegisterOtherTargetCompositionContracts(
				CompositionRegistrationError)))
	{
		AddError(CompositionRegistrationError.ToPublicDetail());
		return false;
	}
	for (const UeAgentRiggingAuthoring::FProfile& Profile :
		UeAgentRiggingAuthoring::GetProfiles())
	{
		UeAgentExportContract::FContract ExportContract;
		TestTrue(
			TEXT("Rigging export contract is registered: ")
				+ Profile.ExportContractId,
			UeAgentExportContract::FRegistry::Get().FindContract(
				Profile.ExportContractId,
				ExportContract)
				&& ExportContract.IsValid());

		const FString CompositionKind =
			HjsonWorkspace::ResolveTargetCompositionKind(
				Profile.TargetKind,
				Profile.TypeId);
		HjsonWorkspace::FTargetCompositionContract TargetComposition;
		TestTrue(
			TEXT("Rigging target composition is registered: ")
				+ CompositionKind,
			HjsonWorkspace::FTargetCompositionRegistry::Get().FindContract(
				CompositionKind,
				TargetComposition));
	}
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
	TestTrue(TEXT("the import exposes only its compact success status"),
		ImportPayload.IsValid()
			&& ImportPayload->Values.Num() == 1
			&& ImportPayload->GetStringField(TEXT("status")) == TEXT("succeeded"));
	// Cover a native function-reference family alongside the existing unit
	// through the ordinary public workspace, save and reload path below.
	UControlRigBlueprint* BeforeExport = LoadObject<UControlRigBlueprint>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("current Control Rig after import"), BeforeExport)) return false;
	URigVMNode* FunctionNode = nullptr;
	FAnimationRigGraphCandidate FunctionCandidate;
	if (!TestTrue(TEXT("IKTwoBone native function reference is created"),
		TryPerformAnimationRigGraphCandidate(*BeforeExport, TEXT("IKTwoBone"),
			FVector2f(480.0f, 144.0f), FunctionNode, FunctionCandidate, CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.PublicMessage);
		return false;
	}
	if (!TestNotNull(TEXT("IKTwoBone uses the native function-reference family"),
		Cast<URigVMFunctionReferenceNode>(FunctionNode))) return false;
	const FName FunctionNodeName = FunctionNode->GetFName();
	if (!TestTrue(TEXT("native function baseline is saved"),
		SaveBlueprint(*BeforeExport->GetOutermost(), *BeforeExport, Filename))) return false;

	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / (TEXT("Automation/UAI/T85/ControlRigExport_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*WorkspacePath, false, true);
	};
	FString ExportParams;
	if (!TestTrue(TEXT("the Control Rig export request uses public workspace fields"),
		TryBuildControlRigExportParams(PackageName, WorkspacePath, ExportParams,
			ParamsError)))
	{
		AddError(ParamsError);
		return false;
	}
	FString ExportPayloadJson;
	bool bExportReadOnly = true;
	HjsonWorkspace::FWorkspaceError ExportError;
	if (!TestTrue(TEXT("the Control Rig exports through content_graph.export_workspace"),
		DomainAdapters::AnimationRigGraph::TryExecuteContentGraph(
			TEXT("content_graph.export_workspace"), PackageName, ExportParams,
			ExportPayloadJson, bExportReadOnly, ExportError)))
	{
		AddError(ExportError.GetCode() + TEXT(":") + ExportError.GetSourceUnit());
		return false;
	}
	TestTrue(TEXT("Control Rig export remains read-only"), bExportReadOnly);
	TMap<FName, FVector2D> BeforeNodePositions;
	{
		UControlRigBlueprint* BeforeEdit = LoadObject<UControlRigBlueprint>(nullptr, *ObjectPath);
		if (!BeforeEdit || !BeforeEdit->GetDefaultModel()) return false;
		for (const URigVMNode* Node : BeforeEdit->GetDefaultModel()->GetNodes())
			if (Node) BeforeNodePositions.Add(Node->GetFName(), Node->GetPosition());
	}
	TArray<FString> WorkspaceDocuments;
	bool bEditedVisibleNode = false;
	int32 RootPropertyEdits = 0;
	bool BeforeRootProperty = false;
	IFileManager::Get().FindFilesRecursive(WorkspaceDocuments, *WorkspacePath,
		TEXT("*.hjson"), true, false);
	for (const FString& WorkspaceDocument : WorkspaceDocuments)
	{
		FString RelativePath = WorkspaceDocument;
		FPaths::MakePathRelativeTo(RelativePath, *(WorkspacePath + TEXT("/")));
		FPaths::NormalizeFilename(RelativePath);
		if (RelativePath.StartsWith(TEXT(".uai/"))) continue;
		FString ExportText;
		TSharedPtr<FJsonObject> ExportDocument;
		if (!FFileHelper::LoadFileToString(ExportText, *WorkspaceDocument)
			|| !FUeAgentStructuredHjsonParser::ParseObject(
				ExportText, WorkspaceDocument, ExportDocument, ParamsError))
		{
			AddError(ParamsError);
			return false;
		}
		bool bEditedDocument = false;
		EditControlRigNodePosition(ExportDocument, bEditedDocument);
		bEditedVisibleNode |= bEditedDocument;
		const int32 PreviousRootEdits = RootPropertyEdits;
		EditControlRigRootProperty(ExportDocument, RootPropertyEdits, BeforeRootProperty);
		if (!bEditedDocument && PreviousRootEdits == RootPropertyEdits) continue;
		FString EditedExport;
		if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(
				ExportDocument.ToSharedRef(), EditedExport, ParamsError)
			|| !FFileHelper::SaveStringToFile(EditedExport, *WorkspaceDocument,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			AddError(ParamsError);
			return false;
		}
	}
	if (!TestTrue(TEXT("ordinary Control Rig workspace exposes the native node position"),
		bEditedVisibleNode)
		|| !TestEqual(TEXT("the same ordinary import edits one root property"), RootPropertyEdits, 1)) return false;
	{
		FString DirectoryImportParams;
		if (!TestTrue(TEXT("edited visible RigVM graph directory re-import parameters canonicalize"),
			TryBuildControlRigDirectoryImportParams(PackageName, WorkspacePath,
				DirectoryImportParams, ParamsError)))
		{
			AddError(ParamsError);
			return false;
		}
		FString DirectoryImportPayload;
		bool bDirectoryReadOnly = true;
		HjsonWorkspace::FWorkspaceError DirectoryImportError;
		if (!TestTrue(TEXT("edited visible RigVM graph re-imports through content_graph.import_workspace"),
			DomainAdapters::AnimationRigGraph::TryExecuteContentGraph(
				TEXT("content_graph.import_workspace"), PackageName,
				DirectoryImportParams, DirectoryImportPayload, bDirectoryReadOnly,
				DirectoryImportError)))
		{
			AddError(DirectoryImportError.GetCode() + TEXT(":")
				+ DirectoryImportError.GetSourceUnit() + TEXT(":")
				+ DirectoryImportError.GetJsonPointer());
		}
		TestFalse(TEXT("edited RigVM graph directory import mutates"), bDirectoryReadOnly);
	}

	UControlRigBlueprint* const Reloaded =
		LoadObject<UControlRigBlueprint>(nullptr, *ObjectPath);
	if (!TestNotNull(
			TEXT("the persisted Control Rig independently reloads"), Reloaded))
	{
		return false;
	}
	URigVMGraph* const ReloadedModel = Reloaded->GetDefaultModel();
	TestEqual(TEXT("root property persists with the document layout in one import"),
		Reloaded->bAllowMultipleInstances, !BeforeRootProperty);
	if (ReloadedModel)
	{
		TestNotNull(TEXT("ordinary workspace preserves the native function reference"),
			Cast<URigVMFunctionReferenceNode>(ReloadedModel->FindNodeByName(FunctionNodeName)));
		TestEqual(TEXT("layout import preserves every native node"),
			ReloadedModel->GetNodes().Num(), BeforeNodePositions.Num());
		for (const URigVMNode* Node : ReloadedModel->GetNodes())
		{
			if (!Node || Node->GetFName() == FName(TEXT("T85Native"))) continue;
			const FVector2D* BeforePosition = BeforeNodePositions.Find(Node->GetFName());
			TestTrue(TEXT("non-target native node position is unchanged"), BeforePosition
				&& Node->GetPosition().Equals(*BeforePosition));
		}
	}
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
	TestTrue(TEXT("edited visible RigVM placement persists on X"),
		FMath::IsNearlyEqual(PersistedUnit->GetPosition().X, 320.0));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT208ControlRigRootPropertyAtomicTest,
	"UAI.Root.T208.ControlRig.RootPropertyAtomic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208ControlRigRootPropertyAtomicTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString PackageName = TEXT("/Game/UAI/Transient/T85ControlRigNativeAction/CR_Atomic_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	DeleteAssetIfPresent(ObjectPath);
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UPackage* Package = CreatePackage(*PackageName);
	UControlRigBlueprintFactory* Factory = NewObject<UControlRigBlueprintFactory>();
	Factory->ParentClass = UControlRig::StaticClass();
	UControlRigBlueprint* Blueprint = Package
		? Cast<UControlRigBlueprint>(Factory->FactoryCreateNew(
			UControlRigBlueprint::StaticClass(), Package, *AssetName,
			RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn))
		: nullptr;
	if (!TestNotNull(TEXT("a Control Rig atomic fixture is created"), Blueprint)) return false;
	Blueprint->InitializeModelIfRequired(false);
	FAssetRegistryModule::AssetCreated(Blueprint);
	if (!TestTrue(TEXT("the Control Rig atomic fixture is saved"),
		SaveBlueprint(*Package, *Blueprint, Filename))) return false;
	UE::UAI::Contract::FDiagnostic RegistrationError;
	if (!TestTrue(TEXT("Rigging authoring registers for the atomic fixture"),
		UeAgentRiggingAuthoring::RegisterAuthoring(RegistrationError)))
	{
		AddError(RegistrationError.ToPublicDetail());
		return false;
	}
	if (!TestTrue(TEXT("shared compositions register for the atomic fixture"),
		HjsonWorkspace::RegisterOtherTargetCompositionContracts(RegistrationError)))
	{
		AddError(RegistrationError.ToPublicDetail());
		return false;
	}
	URigVMGraph* InitialModel = Blueprint->GetDefaultModel();
	if (!TestNotNull(TEXT("the non-target RigVM graph exists"), InitialModel)) return false;
	const FString InitialGraphName = InitialModel->GetName();
	const int32 InitialNodeCount = InitialModel->GetNodes().Num();
	TArray<uint8> BeforeLateBytes;
	if (!FFileHelper::LoadFileToArray(BeforeLateBytes, *Filename)) return false;
	const bool bSavedTarget = Blueprint->bAllowMultipleInstances;
	const bool bSavedDirty = Blueprint->bRunConstructionScriptInSequencer;
	Blueprint->bRunConstructionScriptInSequencer = !bSavedDirty;
	Blueprint->MarkPackageDirty();
	TWeakObjectPtr<UControlRigBlueprint> BeforeNative = Blueprint;
	const TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
	Property->SetStringField(TEXT("property_path"), TEXT("bAllowMultipleInstances"));
	Property->SetBoolField(TEXT("value"), !bSavedTarget);
	UeAgentRiggingAuthoring::FAssetApplyRequest Request;
	Request.Kind = UeAgentRiggingAuthoring::EAssetKind::ControlRig;
	Request.Operation = UeAgentApplyInputContract::EOperation::Update;
	Request.AssetPath = PackageName;
	Request.Properties = {MakeShared<FJsonValueObject>(Property)};
	Request.bCompile = true;
	Request.bSave = true;
	bool bObservedPublished = false;
	Request.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
	{
		UControlRigBlueprint* Current = LoadObject<UControlRigBlueprint>(nullptr, *ObjectPath);
		URigVMGraph* Model = Current ? Current->GetDefaultModel() : nullptr;
		TArray<uint8> PublishedBytes;
		bObservedPublished = !BeforeNative.IsValid() && Current && Current->GeneratedClass
			&& Current->bAllowMultipleInstances == !bSavedTarget
			&& Current->bRunConstructionScriptInSequencer == !bSavedDirty
			&& !Current->GetOutermost()->IsDirty() && Model
			&& Model->GetName() == InitialGraphName && Model->GetNodes().Num() == InitialNodeCount
			&& FFileHelper::LoadFileToArray(PublishedBytes, *Filename)
			&& PublishedBytes != BeforeLateBytes;
		TestTrue(TEXT("late rejection observes compiled clean Control Rig publication"), bObservedPublished);
		Failure.Diagnostic.Code = TEXT("test_control_rig_late_rejection");
		Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
		return false;
	};
	Blueprint = nullptr;
	UeAgentRiggingAuthoring::FAssetApplyResult Result;
	TestFalse(TEXT("late readback rejects the Control Rig transaction"),
		UeAgentRiggingAuthoring::ApplyAsset(Request, Result, TEXT("control-rig-atomic-late")));
	TestEqual(TEXT("the original late error survives"), Result.Diagnostic.Code,
		FString(TEXT("test_control_rig_late_rejection")));
	TestTrue(TEXT("the common checkpoint restores the Control Rig"),
		bObservedPublished && Result.bRollbackAttempted && Result.bRollbackSucceeded);
	UControlRigBlueprint* Restored = LoadObject<UControlRigBlueprint>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("the restored Control Rig resolves"), Restored)) return false;
	TArray<uint8> RestoredBytes;
	URigVMGraph* RestoredModel = Restored->GetDefaultModel();
	TestTrue(TEXT("the original Control Rig disk bytes are restored"),
		FFileHelper::LoadFileToArray(RestoredBytes, *Filename) && RestoredBytes == BeforeLateBytes);
	TestTrue(TEXT("dirty Control Rig memory is restored independently"),
		Restored->GetOutermost()->IsDirty()
			&& Restored->bAllowMultipleInstances == bSavedTarget
			&& Restored->bRunConstructionScriptInSequencer == !bSavedDirty);
	TestTrue(TEXT("the non-target RigVM graph is restored"), Restored->GeneratedClass
		&& RestoredModel && RestoredModel->GetName() == InitialGraphName
		&& RestoredModel->GetNodes().Num() == InitialNodeCount);
	DeleteAssetIfPresent(ObjectPath);
	UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	TestFalse(TEXT("the Control Rig atomic fixture leaves no file"),
		IFileManager::Get().FileExists(*Filename));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT208RiggingIKAtomicTest,
	"UAI.Root.T208.Rigging.IKAtomic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208RiggingIKAtomicTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	auto SaveObject = [](UPackage& Package, UObject& Asset, const FString& Filename)
	{
		Package.MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const bool bSaved = UPackage::SavePackage(&Package, &Asset, *Filename, Args);
		if (bSaved) Package.SetDirtyFlag(false);
		return bSaved;
	};
	UE::UAI::Contract::FDiagnostic RegistrationError;
	if (!UeAgentRiggingAuthoring::RegisterAuthoring(RegistrationError)
		|| !HjsonWorkspace::RegisterOtherTargetCompositionContracts(RegistrationError))
	{
		AddError(RegistrationError.ToPublicDetail());
		return false;
	}
	const FString Root = TEXT("/Game/UAI/Transient/T85RiggingResident_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/");
	const FString RigPackage = Root + TEXT("IK_Atomic_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString RigName = FPackageName::GetLongPackageAssetName(RigPackage);
	const FString RigObject = RigPackage + TEXT(".") + RigName;
	const FString RigFile = FPackageName::LongPackageNameToFilename(
		RigPackage, FPackageName::GetAssetPackageExtension());
	const FString RetargetPackage = Root + TEXT("RTG_Atomic_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString RetargetName = FPackageName::GetLongPackageAssetName(RetargetPackage);
	const FString RetargetObject = RetargetPackage + TEXT(".") + RetargetName;
	const FString RetargetFile = FPackageName::LongPackageNameToFilename(
		RetargetPackage, FPackageName::GetAssetPackageExtension());
	TSharedPtr<DomainAdapters::AssetDestructiveImport::IUeAgentAssetDeleteOwner> DeleteOwner;
	if (!TestTrue(TEXT("the formal resident asset-delete owner composes"),
		DomainAdapters::AssetDestructiveImport::TryCreateAssetDeleteOwner(
			FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T208/RiggingDeleteJournal"),
			TEXT("rigging-resident-delete"), DeleteOwner, RegistrationError))) return false;
	auto DeleteResident = [&](const FString& Target)
	{
		return DeleteOwner->Delete({TEXT("rigging-resident-delete"), {Target}, false,
			TEXT("rigging-resident-test")}, MakeShared<FRiggingDeleteNeverCancelled>());
	};
	ON_SCOPE_EXIT
	{
		if (FindObject<UObject>(nullptr, *RetargetObject)) DeleteResident(RetargetObject);
		if (FindObject<UObject>(nullptr, *RigObject)) DeleteResident(RigObject);
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, RigPackage);
	};
	for (const auto& MissingDocument : {
		TPair<UeAgentRiggingAuthoring::EAssetKind, FString>(
			UeAgentRiggingAuthoring::EAssetKind::IKRig, RigPackage),
		TPair<UeAgentRiggingAuthoring::EAssetKind, FString>(
			UeAgentRiggingAuthoring::EAssetKind::IKRetargeter, RetargetPackage)})
	{
		UeAgentRiggingAuthoring::FAssetApplyRequest CreateRequest;
		CreateRequest.Kind = MissingDocument.Key;
		CreateRequest.Operation = UeAgentApplyInputContract::EOperation::Create;
		CreateRequest.AssetPath = MissingDocument.Value;
		CreateRequest.bCompile = false;
		UeAgentRiggingAuthoring::FAssetApplyResult CreateResult;
		TestFalse(TEXT("Create without required IK document still rejects"),
			UeAgentRiggingAuthoring::ApplyAsset(CreateRequest, CreateResult,
				TEXT("ik-create-missing-document")));
		TestEqual(TEXT("Create retains document dependency preflight"), CreateResult.Diagnostic.Code,
			FString(TEXT("rigging_document_preflight_failed")));
		TestNull(TEXT("rejected Create publishes no native asset"),
			UeAgentRiggingAuthoring::LoadAsset(MissingDocument.Key, MissingDocument.Value));
	}

	UPackage* RigOuter = CreatePackage(*RigPackage);
	UIKRigDefinitionFactory* RigFactory = NewObject<UIKRigDefinitionFactory>();
	UIKRigDefinition* Rig = RigOuter ? Cast<UIKRigDefinition>(RigFactory->FactoryCreateNew(
		UIKRigDefinition::StaticClass(), RigOuter, *RigName,
		RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn)) : nullptr;
	USkeletalMesh* PreviewMesh = LoadObject<USkeletalMesh>(nullptr,
		TEXT("/Game/Characters/PaladinRootMotion/Mesh/Paladin_WProp_J_Nordstrom_RM.Paladin_WProp_J_Nordstrom_RM"));
	if (!TestNotNull(TEXT("an IK Rig atomic fixture is created"), Rig)
		|| !TestNotNull(TEXT("the IK Rig preview mesh resolves"), PreviewMesh)
		|| !TestTrue(TEXT("the IK Rig atomic fixture is saved"),
			Rig && (Rig->SetPreviewMesh(PreviewMesh, false), SaveObject(*RigOuter, *Rig, RigFile)))) return false;
	FAssetRegistryModule::AssetCreated(Rig);
	TArray<uint8> RigBeforeBytes;
	if (!FFileHelper::LoadFileToArray(RigBeforeBytes, *RigFile)) return false;
	const bool bSavedDrawGoals = Rig->DrawGoals;
	const float SavedGoalSize = Rig->GoalSize;
	Rig->GoalSize = SavedGoalSize + 1.0f;
	Rig->MarkPackageDirty();
	TWeakObjectPtr<UIKRigDefinition> RigBeforeNative = Rig;
	const TSharedRef<FJsonObject> RigProperty = MakeShared<FJsonObject>();
	RigProperty->SetStringField(TEXT("property_path"), TEXT("DrawGoals"));
	RigProperty->SetBoolField(TEXT("value"), !bSavedDrawGoals);
	UeAgentRiggingAuthoring::FAssetApplyRequest RigRequest;
	RigRequest.Kind = UeAgentRiggingAuthoring::EAssetKind::IKRig;
	RigRequest.Operation = UeAgentApplyInputContract::EOperation::Update;
	RigRequest.AssetPath = RigPackage;
	RigRequest.Properties = {MakeShared<FJsonValueObject>(RigProperty)};
	RigRequest.bCompile = true;
	RigRequest.bSave = true;
	// Explicit document input must still reject before root-property mutation.
	UeAgentRiggingAuthoring::FAssetApplyRequest InvalidRigRequest = RigRequest;
	InvalidRigRequest.Document = MakeShared<FJsonObject>();
	InvalidRigRequest.Document->SetBoolField(TEXT("preview_mesh"), false);
	UeAgentRiggingAuthoring::FAssetApplyResult InvalidRigResult;
	TestFalse(TEXT("explicit invalid IK Rig document rejects in preflight"),
		UeAgentRiggingAuthoring::ApplyAsset(InvalidRigRequest, InvalidRigResult,
			TEXT("ik-rig-invalid-document")));
	TestEqual(TEXT("invalid IK Rig document retains the document preflight error"),
		InvalidRigResult.Diagnostic.Code, FString(TEXT("rigging_document_preflight_failed")));
	TArray<uint8> RigAfterInvalidBytes;
	TestTrue(TEXT("explicit document rejection leaves dirty memory and disk untouched"),
		Rig->DrawGoals == bSavedDrawGoals && Rig->GetOutermost()->IsDirty()
		&& FMath::IsNearlyEqual(Rig->GoalSize, SavedGoalSize + 1.0f)
		&& FFileHelper::LoadFileToArray(RigAfterInvalidBytes, *RigFile)
		&& RigAfterInvalidBytes == RigBeforeBytes);
	bool bRigPublished = false;
	RigRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
	{
		UIKRigDefinition* Current = LoadObject<UIKRigDefinition>(nullptr, *RigObject);
		TArray<uint8> Bytes;
		bRigPublished = !RigBeforeNative.IsValid() && Current
			&& Current->DrawGoals == !bSavedDrawGoals
			&& FMath::IsNearlyEqual(Current->GoalSize, SavedGoalSize + 1.0f)
			&& !Current->GetOutermost()->IsDirty()
			&& FFileHelper::LoadFileToArray(Bytes, *RigFile) && Bytes != RigBeforeBytes;
		TestTrue(TEXT("IK Rig late rejection follows reinitialization and publication"), bRigPublished);
		Failure.Diagnostic.Code = TEXT("test_ik_rig_late_rejection");
		Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
		return false;
	};
	Rig = nullptr;
	UeAgentRiggingAuthoring::FAssetApplyResult RigResult;
	TestFalse(TEXT("IK Rig late readback rejection fails the transaction"),
		UeAgentRiggingAuthoring::ApplyAsset(RigRequest, RigResult, TEXT("ik-rig-atomic-late")));
	TestEqual(TEXT("IK Rig preserves the late readback failure"), RigResult.Diagnostic.Code,
		FString(TEXT("test_ik_rig_late_rejection")));
	TestTrue(TEXT("IK Rig common checkpoint restores after native reload"),
		bRigPublished && RigResult.bRollbackAttempted && RigResult.bRollbackSucceeded);
	UIKRigDefinition* RigRestored = LoadObject<UIKRigDefinition>(nullptr, *RigObject);
	TArray<uint8> RigRestoredBytes;
	TestTrue(TEXT("IK Rig restores original disk and dirty memory"), RigRestored
		&& FFileHelper::LoadFileToArray(RigRestoredBytes, *RigFile)
		&& RigRestoredBytes == RigBeforeBytes && RigRestored->GetOutermost()->IsDirty()
		&& RigRestored->DrawGoals == bSavedDrawGoals
		&& FMath::IsNearlyEqual(RigRestored->GoalSize, SavedGoalSize + 1.0f));

	UPackage* RetargetOuter = CreatePackage(*RetargetPackage);
	UIKRetargetFactory* RetargetFactory = NewObject<UIKRetargetFactory>();
	UIKRetargeter* Retargeter = RetargetOuter ? Cast<UIKRetargeter>(RetargetFactory->FactoryCreateNew(
		UIKRetargeter::StaticClass(), RetargetOuter, *RetargetName,
		RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn)) : nullptr;
	UIKRetargeterController* RetargetController = Retargeter
		? UIKRetargeterController::GetController(Retargeter) : nullptr;
	if (RetargetController)
	{
		RetargetController->SetIKRig(ERetargetSourceOrTarget::Source, RigRestored);
		RetargetController->SetIKRig(ERetargetSourceOrTarget::Target, RigRestored);
	}
	if (!TestNotNull(TEXT("an IK Retargeter atomic fixture is created"), Retargeter)
		|| !TestNotNull(TEXT("the IK Retargeter controller resolves"), RetargetController)
		|| !TestTrue(TEXT("the IK Retargeter fixture is saved"),
			Retargeter && SaveObject(*RetargetOuter, *Retargeter, RetargetFile))) return false;
	FAssetRegistryModule::AssetCreated(Retargeter);
	const bool bSavedDebugDraw = Retargeter->bDebugDraw;
	const TSharedRef<FJsonObject> RetargetProperty = MakeShared<FJsonObject>();
	RetargetProperty->SetStringField(TEXT("property_path"), TEXT("bDebugDraw"));
	RetargetProperty->SetBoolField(TEXT("value"), !bSavedDebugDraw);
	UeAgentRiggingAuthoring::FAssetApplyRequest RetargetRequest;
	RetargetRequest.Kind = UeAgentRiggingAuthoring::EAssetKind::IKRetargeter;
	RetargetRequest.Operation = UeAgentApplyInputContract::EOperation::Update;
	RetargetRequest.AssetPath = RetargetPackage;
	RetargetRequest.Properties = {MakeShared<FJsonValueObject>(RetargetProperty)};
	RetargetRequest.bCompile = true;
	RetargetRequest.bSave = true;
	bool bRetargetReadback = false;
	RetargetRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure&)
	{
		UIKRetargeter* Current = LoadObject<UIKRetargeter>(nullptr, *RetargetObject);
		bRetargetReadback = Current && Current->bDebugDraw == !bSavedDebugDraw
			&& !Current->GetOutermost()->IsDirty();
		return bRetargetReadback;
	};
	Retargeter = nullptr;
	UeAgentRiggingAuthoring::FAssetApplyResult RetargetResult;
	TestTrue(TEXT("IK Retargeter CleanAsset path reaches persistent final readback"),
		UeAgentRiggingAuthoring::ApplyAsset(
			RetargetRequest, RetargetResult, TEXT("ik-retargeter-atomic-success")));
	TestTrue(TEXT("IK Retargeter final readback observes the native-cleaned value"),
		bRetargetReadback && RetargetResult.IsApplied());
	UIKRetargeter* RetargetReadback = LoadObject<UIKRetargeter>(nullptr, *RetargetObject);
	const UeAgentRiggingAuthoring::FProfile* RetargetProfile =
		UeAgentRiggingAuthoring::FindProfile(UeAgentRiggingAuthoring::EAssetKind::IKRetargeter);
	const FString ExportPath = FPaths::ProjectSavedDir()
		/ TEXT("Automation/UAI/T208/IKRetargetExport")
		/ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*ExportPath, false, true); };
	UeAgentExportWorkspaceWriter::FWorkspaceResult ExportResult;
	FString RetargetExportError;
	if (!TestTrue(TEXT("IK Retargeter native export diagnoses its public workspace"),
		RetargetReadback && RetargetProfile
		&& UeAgentRiggingAuthoring::ExportAssetWorkspace(*RetargetProfile, *RetargetReadback, ExportPath, TEXT("llm_authoring"), ExportResult)))
	{
		AddError(ExportResult.Diagnostic.Code + TEXT(":") + ExportResult.Diagnostic.InternalContext
			+ TEXT(":") + RetargetExportError);
	}

	if (!TestTrue(TEXT("the restored owned IK fixture can be saved before reference validation"),
		RigRestored && SaveObject(*RigRestored->GetOutermost(), *RigRestored, RigFile))) return false;
	FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({RigFile, RetargetFile});
	TArray<uint8> BeforeBlockedDelete, AfterBlockedDelete;
	if (!FFileHelper::LoadFileToArray(BeforeBlockedDelete, *RigFile)) return false;
	const auto BlockedDelete = DeleteResident(RigObject);
	TestFalse(TEXT("a legitimate saved Retargeter reference still rejects IK deletion"),
		BlockedDelete.IsSuccess());
	TestTrue(TEXT("reference rejection preserves the IK asset and original disk bytes"),
		BlockedDelete.Targets.Num() == 1
		&& BlockedDelete.Targets[0].ErrorCode == TEXT("asset_delete_package_referenced")
		&& FindObject<UIKRigDefinition>(nullptr, *RigObject) == RigRestored
		&& FFileHelper::LoadFileToArray(AfterBlockedDelete, *RigFile)
		&& BeforeBlockedDelete == AfterBlockedDelete);
	const auto DeletedRetargeter = DeleteResident(RetargetObject);
	if (!TestTrue(TEXT("the native Retargeter deletes through the formal owner"),
		DeletedRetargeter.IsSuccess())) return false;
	{
		TStrongObjectPtr<UIKRetargeter> LegitimateResidentReference(NewObject<UIKRetargeter>(GetTransientPackage(), NAME_None, RF_Transient));
		UIKRetargeterController::GetController(LegitimateResidentReference.Get())->SetIKRig(ERetargetSourceOrTarget::Source, RigRestored);
		const auto ResidentRejected = DeleteResident(RigObject);
		TestFalse(TEXT("a real resident strong reference still refuses native deletion"), ResidentRejected.IsSuccess());
		UIKRigDefinition* AfterRejection = LoadObject<UIKRigDefinition>(nullptr, *RigObject);
		UIKRigController* RestoredController = AfterRejection ? UIKRigController::GetController(AfterRejection) : nullptr;
		TestTrue(TEXT("native refusal restores a usable controller and the original asset bytes"),
			ResidentRejected.Targets.Num() == 1
			&& ResidentRejected.Targets[0].ErrorCode == TEXT("asset_delete_native_delete_failed")
			&& AfterRejection && RestoredController && RestoredController->GetAsset() == AfterRejection
			&& FMath::IsNearlyEqual(AfterRejection->GoalSize, SavedGoalSize + 1.0f)
			&& FFileHelper::LoadFileToArray(AfterBlockedDelete, *RigFile)
			&& BeforeBlockedDelete == AfterBlockedDelete);
		UIKRetargeterController::GetController(LegitimateResidentReference.Get())->SetIKRig(ERetargetSourceOrTarget::Source, nullptr);
	}
	const auto DeletedRig = DeleteResident(RigObject);
	if (!TestTrue(TEXT("released proposed Retargeter state cannot retain the resident IK Rig"),
		DeletedRig.IsSuccess()))
	{
		AddError(DeletedRig.Targets.IsEmpty() ? TEXT("empty_delete_result")
			: DeletedRig.Targets[0].ErrorCode + TEXT(":") + DeletedRig.Targets[0].ErrorDetail);
		return false;
	}
	TestTrue(TEXT("resident IK deletion proves file/package/registry/object absence"),
		DeletedRig.Targets.Num() == 1 && DeletedRig.Targets[0].bFileAbsent
		&& (DeletedRig.Targets[0].bPackageAbsent || DeletedRig.Targets[0].bPackageEmpty) && DeletedRig.Targets[0].bRegistryAbsent
		&& DeletedRig.Targets[0].bObjectAbsent);
	UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, RigPackage);
	TestFalse(TEXT("IK Rig fixture leaves no file"), IFileManager::Get().FileExists(*RigFile));
	TestFalse(TEXT("IK Retargeter fixture leaves no file"), IFileManager::Get().FileExists(*RetargetFile));
	return !HasAnyErrors();
}
