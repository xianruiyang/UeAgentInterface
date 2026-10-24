#include "Misc/AutomationTest.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "AnimGraphNode_StateMachine.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "Animation/AnimBlueprint.h"
#include "AnimationGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "DomainAdapters/AnimationBlueprint/UeAgentAnimationBlueprintDomainMechanics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDescriptors.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringApplyProvider.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringRegistration.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringRegistration.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringExport.h"
#include "Animation/AnimSequence.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "PackageTools.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Misc/Paths.h"
#include "EdGraph/EdGraph.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "QueryOwners/AnimationBlueprint/UeAgentAnimationBlueprintQueryOwner.h"
#include "Utilities/AuthoringExport/UeAgentExportContract.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Animation/AnimInstance.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "K2Node_FunctionEntry.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetDeleteOwner.h"

	namespace
{
	using namespace UE::UAI;

	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
				UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	TMap<FString, FString> CaptureAnimationGraphs(UAnimBlueprint& Blueprint, const FString& IgnoreGraph = FString())
	{
		TMap<FString, FString> Result;
		Result.Add(TEXT("skeleton"), GetPathNameSafe(Blueprint.TargetSkeleton));
		TArray<UEdGraph*> Graphs; Blueprint.GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			if (Graph->GetName() == IgnoreGraph) continue;
			Result.Add(TEXT("graph:") + Graph->GetName(), Graph->GraphGuid.ToString() + TEXT(":") + Graph->GetClass()->GetPathName());
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				const FString Key = Node->NodeGuid.ToString();
				Result.Add(Key, Graph->GetName() + TEXT(":") + Node->GetClass()->GetPathName() + FString::Printf(TEXT(":%d:%d"), Node->NodePosX, Node->NodePosY));
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					TArray<FString> Links;
					for (const UEdGraphPin* Link : Pin->LinkedTo) Links.Add(Link->PinId.ToString());
					Links.Sort();
					Result.Add(Key + TEXT(":") + Pin->PinName.ToString(), Pin->PinId.ToString() + TEXT(":") + Pin->DefaultValue + TEXT(":") + FString::Join(Links, TEXT(",")));
				}
			}
		}
		return Result;
	}

	bool ExerciseAnimationWorkspace(FAutomationTestBase& Test, UAnimBlueprint*& Blueprint, const FString& ObjectPath, const FString& PackageName)
	{
		UE::UAI::Contract::FDiagnostic Error;
		UAnimSequence* Reference = LoadObject<UAnimSequence>(nullptr, TEXT("/Game/Characters/PaladinRootMotion/Animations/sword_and_shield_idle_RM.sword_and_shield_idle_RM"));
		if (!Test.TestNotNull(TEXT("read-only animation supplies an actual skeleton"), Reference)) return false;
		Blueprint->TargetSkeleton = Reference->GetSkeleton();
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("GenericEvents"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
		FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
		UK2Node_CallFunction* Print = Creator.CreateNode();
		Print->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString)));
		Creator.Finalize(); Print->NodePosX = 496; Print->NodePosY = 96;
		Print->FindPinChecked(TEXT("InString"))->DefaultValue = TEXT("AnimGraphBefore");
		const FGuid PrintGuid = Print->NodeGuid;
		FEdGraphPinType Type; Type.PinCategory = UEdGraphSchema_K2::PC_Int;
		if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("WorkCount"), Type, TEXT("7")) ||
			!FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("KeepCount"), Type, TEXT("23"))) return false;
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		const FGuid WorkGuid = Blueprint->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("WorkCount"))].VarGuid;
		const FGuid KeepGuid = Blueprint->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("KeepCount"))].VarGuid;
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Blueprint->GetOutermost(), Blueprint, *Filename, Args)) return false;
		Blueprint->GetOutermost()->SetDirtyFlag(false);
		auto Expected = CaptureAnimationGraphs(*Blueprint);
		const FString NodeKey = PrintGuid.ToString();
		Expected[NodeKey] = Expected[NodeKey].Replace(TEXT(":496:96"), TEXT(":768:192"));
		Expected[NodeKey + TEXT(":InString")] = Expected[NodeKey + TEXT(":InString")].Replace(TEXT("AnimGraphBefore"), TEXT("AnimGraphAfter"));
		const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-animation-blueprint-workspace/native") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
		const auto* Profile = UeAgentAnimationAuthoring::FindProfileByTypeId(TEXT("anim_blueprint"));
		UeAgentExportWorkspaceWriter::FWorkspaceResult Exported;
		if (!UeAgentAnimationAuthoring::ExportAssetWorkspace(*Profile, *Blueprint, Workspace, TEXT("llm_authoring"), Exported)) { Test.AddError(Error.ToPublicDetail()); return false; }
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
		int32 Pins = 0, Positions = 0, Members = 0;
		TFunction<void(const TSharedPtr<FJsonValue>&)> Edit;
		Edit = [&](const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid()) return;
			if (Value->Type == EJson::Array) { for (const auto& Item : Value->AsArray()) Edit(Item); return; }
			if (Value->Type != EJson::Object) return;
			auto Object = Value->AsObject();
			FString Function;
			if (Object->TryGetStringField(TEXT("function_name"), Function) && Function == TEXT("PrintString") && Object->HasField(TEXT("pos")))
			{ auto Pos = Object->GetObjectField(TEXT("pos")); Pos->SetNumberField(TEXT("x"), 768); Pos->SetNumberField(TEXT("y"), 192); ++Positions; }
			if (Object->HasField(TEXT("variables")))
			{
				const auto Variables = Object->GetObjectField(TEXT("variables"));
				const TSharedPtr<FJsonObject>* Work = nullptr;
				if (Variables->TryGetObjectField(TEXT("WorkCount"), Work))
				{
					auto Default = MakeShared<FJsonObject>(); Default->SetStringField(TEXT("property_path"), TEXT("WorkCount")); Default->SetNumberField(TEXT("value"), 17);
					(*Work)->SetObjectField(TEXT("default_property"), Default); ++Members;
				}
			}
			for (auto& Field : Object->Values)
			{
				if (Field.Value->Type == EJson::String && Field.Value->AsString() == TEXT("AnimGraphBefore")) { Field.Value = MakeShared<FJsonValueString>(TEXT("AnimGraphAfter")); ++Pins; }
				Edit(Field.Value);
			}
		};
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
			const int32 Previous = Pins + Positions + Members; Edit(MakeShared<FJsonValueObject>(Document));
			if (Previous != Pins + Positions + Members && (!CommandContract::Json::TryCanonicalize(Document, Text, Error) || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
		}
		if (!Test.TestEqual(TEXT("ordinary existing member edits once"), Members, 1) || !Test.TestEqual(TEXT("ordinary node pin edits once"), Pins, 1) || !Test.TestEqual(TEXT("ordinary node layout edits once"), Positions, 1)) return false;
		auto Directory = MakeShared<FJsonObject>(); Directory->SetStringField(TEXT("kind"), TEXT("directory")); Directory->SetStringField(TEXT("path"), Workspace);
		HjsonWorkspace::FResolvedContentGraphImportSourcePtr Source;
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		FString Revision, Authorization; HjsonWorkspace::FWorkspaceError WorkspaceError;
		if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Directory, PackageName, Resources, Source, Revision, Authorization, WorkspaceError)) { Test.AddError(WorkspaceError.GetCode()); return false; }
		auto Params = MakeShared<FJsonObject>(); Params->SetStringField(TEXT("target"), PackageName); Params->SetStringField(TEXT("expected_revision"), Revision); Params->SetObjectField(TEXT("source"), Source->MakeNormalizedSourceObject());
		FString ParamsText; if (!CommandContract::Json::TryCanonicalize(Params, ParamsText, Error)) return false;
		Blueprint = nullptr; Graph = nullptr; Print = nullptr;
		FString Payload; bool ReadOnly = true;
		if (!DomainAdapters::AnimationBlueprint::TryExecuteContentGraph(TEXT("content_graph.import_workspace"), PackageName, ParamsText, Payload, ReadOnly, WorkspaceError))
		{ Test.AddError(WorkspaceError.GetCode() + TEXT(":") + WorkspaceError.GetSourceUnit() + TEXT(":") + WorkspaceError.GetJsonPointer()); return false; }
		UPackage* Package = FindPackage(nullptr, *PackageName);
		if (!Package || !UPackageTools::UnloadPackages({Package})) return false;
		Blueprint = LoadObject<UAnimBlueprint>(nullptr, *ObjectPath);
		if (!Test.TestNotNull(TEXT("ordinary AnimBlueprint independently reloads"), Blueprint)) return false;
		Test.TestTrue(TEXT("all non-target native graph, pin identities and skeleton persist"), Expected.OrderIndependentCompareEqual(CaptureAnimationGraphs(*Blueprint)));
		const FIntProperty* Work = FindFProperty<FIntProperty>(Blueprint->GeneratedClass, TEXT("WorkCount"));
		const FIntProperty* Keep = FindFProperty<FIntProperty>(Blueprint->GeneratedClass, TEXT("KeepCount"));
		Test.TestTrue(TEXT("native member defaults and identities survive compile and reload"), Work && Keep &&
			Work->GetPropertyValue_InContainer(Blueprint->GeneratedClass->GetDefaultObject()) == 17 && Keep->GetPropertyValue_InContainer(Blueprint->GeneratedClass->GetDefaultObject()) == 23 &&
			Blueprint->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("WorkCount"))].VarGuid == WorkGuid &&
			Blueprint->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("KeepCount"))].VarGuid == KeepGuid && !Blueprint->GetOutermost()->IsDirty() && Blueprint->Status != BS_Error);
		return UeAgentAnimationAuthoring::ExportAssetWorkspace(*Profile, *Blueprint, Workspace + TEXT("-readback"), TEXT("llm_authoring"), Exported);
	}

	TSharedPtr<FJsonObject> GenericAnimationEdit(UAnimBlueprint& Blueprint)
	{
		UEdGraph* Graph = *Blueprint.UbergraphPages.FindByPredicate([](const UEdGraph* Item) { return Item && Item->GetFName() == TEXT("GenericEvents"); });
		UK2Node_CallFunction* Print = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes) if (auto* Call = Cast<UK2Node_CallFunction>(Node)) Print = Call;
		auto Pin = MakeShared<FJsonObject>(); Pin->SetStringField(TEXT("pin_name"), TEXT("InString")); Pin->SetNumberField(TEXT("pin_occurrence"), 0); Pin->SetStringField(TEXT("default_value"), TEXT("AnimGraphPublished"));
		auto Pins = MakeShared<FJsonObject>(); Pins->SetObjectField(TEXT("pin:InString:0"), Pin);
		auto Node = MakeShared<FJsonObject>(); Node->SetStringField(TEXT("operation"), TEXT("update")); Node->SetStringField(TEXT("node_guid"), Print->NodeGuid.ToString()); Node->SetObjectField(TEXT("pin_defaults"), Pins);
		auto Nodes = MakeShared<FJsonObject>(); Nodes->SetObjectField(Print->NodeGuid.ToString(), Node);
		auto GraphEdit = MakeShared<FJsonObject>(); GraphEdit->SetStringField(TEXT("operation"), TEXT("update")); GraphEdit->SetStringField(TEXT("graph_kind"), TEXT("event_graph")); GraphEdit->SetStringField(TEXT("name"), Graph->GetName()); GraphEdit->SetObjectField(TEXT("nodes"), Nodes);
		auto Graphs = MakeShared<FJsonObject>(); Graphs->SetObjectField(TEXT("event_graph:") + Graph->GetName(), GraphEdit);
		auto Default = MakeShared<FJsonObject>(); Default->SetStringField(TEXT("property_path"), TEXT("WorkCount")); Default->SetNumberField(TEXT("value"), 27);
		auto Variable = MakeShared<FJsonObject>(); Variable->SetObjectField(TEXT("default_property"), Default);
		auto Variables = MakeShared<FJsonObject>(); Variables->SetObjectField(TEXT("WorkCount"), Variable);
		auto Members = MakeShared<FJsonObject>(); Members->SetObjectField(TEXT("variables"), Variables);
		auto Extension = MakeShared<FJsonObject>(); Extension->SetObjectField(TEXT("members"), Members); Extension->SetObjectField(TEXT("graphs"), Graphs);
		return Extension;
	}

	int32 WorkCount(UAnimBlueprint& Blueprint)
	{
		const FIntProperty* Property = FindFProperty<FIntProperty>(Blueprint.GeneratedClass, TEXT("WorkCount"));
		return Property ? Property->GetPropertyValue_InContainer(Blueprint.GeneratedClass->GetDefaultObject()) : -1;
	}

	TSharedRef<FJsonObject> MakeClassSettings()
	{
		const TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
		Settings->SetBoolField(TEXT("template"), true);
		Settings->SetStringField(
			TEXT("parent_class"), TEXT("/Script/Engine.AnimInstance"));
		return Settings;
	}

	bool TryBuildResolvedAnimationBlueprintImportParams(
		const FString& Target,
		const FString& ParentCandidateName,
		const FString& StateCandidateName,
		FString& OutParams,
		FString& OutError)
	{
		const TSharedRef<FJsonObject> ParentCandidate =
			MakeShared<FJsonObject>();
		const TSharedRef<FJsonObject> StateCandidate =
			MakeShared<FJsonObject>();
		ParentCandidate->SetStringField(TEXT("name"), ParentCandidateName);
		StateCandidate->SetStringField(TEXT("name"), StateCandidateName);

		const TSharedRef<FJsonObject> IdleState = MakeShared<FJsonObject>();
		IdleState->SetStringField(TEXT("name"), TEXT("Idle"));
		IdleState->SetObjectField(TEXT("candidate"), StateCandidate);
		IdleState->SetNumberField(TEXT("pos_x"), 240);
		IdleState->SetNumberField(TEXT("pos_y"), 0);

		const TSharedRef<FJsonObject> StateMachine = MakeShared<FJsonObject>();
		StateMachine->SetStringField(TEXT("name"), TEXT("Locomotion"));
		StateMachine->SetStringField(TEXT("animation_graph"), TEXT("AnimGraph"));
		StateMachine->SetObjectField(TEXT("candidate"), ParentCandidate);
		StateMachine->SetNumberField(TEXT("pos_x"), 320);
		StateMachine->SetNumberField(TEXT("pos_y"), 64);
		StateMachine->SetArrayField(
			TEXT("states"), {MakeShared<FJsonValueObject>(IdleState)});
		StateMachine->SetStringField(TEXT("entry_state"), TEXT("Idle"));
		StateMachine->SetArrayField(TEXT("transitions"), {});

		const TSharedRef<FJsonObject> ProviderExtension =
			MakeShared<FJsonObject>();
		ProviderExtension->SetObjectField(
			TEXT("class_settings"), MakeClassSettings());
		ProviderExtension->SetArrayField(
			TEXT("state_machines"),
			{MakeShared<FJsonValueObject>(StateMachine)});

		const TSharedRef<FJsonObject> DocumentTarget = MakeShared<FJsonObject>();
		DocumentTarget->SetStringField(TEXT("asset_path"), Target);
		const TSharedRef<FJsonObject> AuthoringDocument =
			MakeShared<FJsonObject>();
		AuthoringDocument->SetStringField(TEXT("operation"), TEXT("create"));
		AuthoringDocument->SetObjectField(TEXT("target"), DocumentTarget);
		AuthoringDocument->SetObjectField(
			TEXT("provider_extension"), ProviderExtension);

		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(
			TEXT("type"), TEXT("/Script/Engine.AnimBlueprint"));
		Root->SetObjectField(TEXT("document"), AuthoringDocument);
		const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(
			TEXT("schema"), TEXT("uai.animation_blueprint.edit/1"));
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
			OutError = WorkspaceError.GetCode() + TEXT(":")
				+ WorkspaceError.GetSourceUnit();
			return false;
		}

		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetStringField(TEXT("expected_revision"), TEXT("missing"));
		Params->SetObjectField(
			TEXT("source"), ResolvedSource->MakeNormalizedSourceObject());
		return CommandContract::Json::TryCanonicalize(
			Params, OutParams, OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85AnimationStateMachineCandidateVerticalTest,
	"UAI.Root.T85.AnimationBlueprint.StateMachineCandidateVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85AnimationStateMachineCandidateVerticalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::QueryOwners::AnimationBlueprint;

	const FString PackageName =
		TEXT("/Game/UAI/Transient/T85AnimationBlueprint/ABP_Candidate_")
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

	const TSharedPtr<FJsonObject> ClassSettings = MakeClassSettings();
	FAnimationBlueprintCandidateQueryResult ParentResult;
	FAnimationBlueprintCandidateQueryError CandidateError;
	if (!TestTrue(TEXT("the future AnimGraph native menu is searched"),
		TryQueryAnimationBlueprintCandidates(
			ObjectPath,
			TEXT("animation_graph"),
			ClassSettings,
			TEXT("State Machine"),
			ParentResult,
			CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.PublicMessage);
		return false;
	}
	const FAnimationBlueprintCandidate* const ParentCandidate =
		ParentResult.Candidates.FindByPredicate(
			[](const FAnimationBlueprintCandidate& Candidate)
			{
				return Candidate.bSupported
					&& Candidate.NodeClassPath
						== UAnimGraphNode_StateMachine::StaticClass()->GetPathName();
			});
	if (!TestNotNull(TEXT("one writable native StateMachine action is exposed"),
		ParentCandidate))
	{
		return false;
	}

	FAnimationBlueprintCandidateQueryResult StateResult;
	CandidateError = {};
	if (!TestTrue(TEXT("the future StateMachine schema menu is searched"),
		TryQueryAnimationBlueprintCandidates(
			ObjectPath,
			TEXT("state_machine_graph"),
			ClassSettings,
			TEXT("State"),
			StateResult,
			CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.PublicMessage);
		return false;
	}
	const FAnimationBlueprintCandidate* const StateCandidate =
		StateResult.Candidates.FindByPredicate(
			[](const FAnimationBlueprintCandidate& Candidate)
			{
				return Candidate.bSupported
					&& Candidate.NodeClassPath
						== UAnimStateNode::StaticClass()->GetPathName();
			});
	if (!TestNotNull(TEXT("one writable native State action is exposed"),
		StateCandidate))
	{
		return false;
	}

	FString CanonicalParams;
	FString ParamsError;
	if (!TestTrue(TEXT("name-only Animation Blueprint HJSON canonicalizes"),
		TryBuildResolvedAnimationBlueprintImportParams(
			ObjectPath,
			ParentCandidate->Name,
			StateCandidate->Name,
			CanonicalParams,
			ParamsError)))
	{
		AddError(ParamsError);
		return false;
	}
	TestFalse(TEXT("the HJSON input contains no hidden node class binding"),
		CanonicalParams.Contains(TEXT("\"class\"")));

	FString ImportPayloadJson;
	bool bReadOnly = true;
	HjsonWorkspace::FWorkspaceError ImportError;
	if (!TestTrue(TEXT("name-only HJSON creates and persists native actions"),
		DomainAdapters::AnimationBlueprint::TryExecuteContentGraph(
			TEXT("content_graph.import_workspace"),
			ObjectPath,
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
	TestFalse(TEXT("the Animation Blueprint import is a mutation"), bReadOnly);

	const TCHAR* RequiredAnimationProfiles[] = {
		TEXT("anim_blueprint"),
		TEXT("skeleton"),
		TEXT("skeletal_mesh"),
	};
	for (const TCHAR* TypeId : RequiredAnimationProfiles)
	{
		TestNotNull(
			FString::Printf(
				TEXT("Animation HJSON profile %s is registered"),
				TypeId),
			UeAgentAnimationAuthoring::FindProfileByTypeId(TypeId));
	}
	FString AnimationRegistrationError;
	if (!TestTrue(
			TEXT("Animation authoring export/import owners register"),
			UeAgentAnimationAuthoring::RegisterAuthoring(
				AnimationRegistrationError)))
	{
		AddError(AnimationRegistrationError);
		return false;
	}

	FString CompositionRegistrationError;
	if (!TestTrue(
			TEXT("shared target compositions register for Animation"),
			HjsonWorkspace::RegisterOtherTargetCompositionContracts(
				CompositionRegistrationError)))
	{
		AddError(CompositionRegistrationError);
		return false;
	}
	for (const UeAgentAnimationAuthoring::FProfile& Profile :
		UeAgentAnimationAuthoring::GetProfiles())
	{
		UeAgentExportContract::FContract ExportContract;
		TestTrue(
			TEXT("Animation export contract is registered: ")
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
			TEXT("Animation target composition is registered: ")
				+ CompositionKind,
			HjsonWorkspace::FTargetCompositionRegistry::Get().FindContract(
				CompositionKind,
				TargetComposition));
	}

	TSharedPtr<FJsonObject> ImportPayload;
	FString PayloadError;
	if (!TestTrue(TEXT("the import payload is canonical JSON"),
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

	UAnimBlueprint* Reloaded =
		LoadObject<UAnimBlueprint>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("the persisted AnimBlueprint reloads"), Reloaded))
	{
		return false;
	}
	TestNotNull(TEXT("the native compile generated a class"),
		Reloaded->GeneratedClass.Get());
	TestTrue(TEXT("the reloaded AnimBlueprint is not in compile error"),
		Reloaded->Status != BS_Error);

	TArray<UEdGraph*> Graphs;
	Reloaded->GetAllGraphs(Graphs);
	UEdGraph* AnimGraph = nullptr;
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph
			&& Graph->GetFName() == TEXT("AnimGraph")
			&& Graph->IsA<UAnimationGraph>())
		{
			AnimGraph = Graph;
			break;
		}
	}
	if (!TestNotNull(TEXT("the native AnimGraph survives reload"), AnimGraph))
	{
		return false;
	}
	TArray<UAnimGraphNode_StateMachine*> MachineNodes;
	AnimGraph->GetNodesOfClass<UAnimGraphNode_StateMachine>(MachineNodes);
	UAnimGraphNode_StateMachine* MachineNode = nullptr;
	for (UAnimGraphNode_StateMachine* Node : MachineNodes)
	{
		if (Node
			&& Node->GetClass()
				== UAnimGraphNode_StateMachine::StaticClass()
			&& Node->EditorStateMachineGraph
			&& Node->EditorStateMachineGraph->GetFName()
				== TEXT("Locomotion"))
			{
			MachineNode = Node;
			break;
		}
	}
	if (!TestNotNull(TEXT("the exact native StateMachine node survives reload"),
		MachineNode))
	{
		return false;
	}
	UAnimationStateMachineGraph* const MachineGraph =
		MachineNode->EditorStateMachineGraph;
	TestEqual(TEXT("the StateMachine keeps its native schema"),
		MachineGraph->GetSchema()->GetClass(),
		UAnimationStateMachineSchema::StaticClass());
	TestTrue(TEXT("the StateMachine subgraph remains registered"),
		Graphs.Contains(MachineGraph));

	TArray<UAnimStateEntryNode*> EntryNodes;
	MachineGraph->GetNodesOfClass<UAnimStateEntryNode>(EntryNodes);
	TestEqual(TEXT("the native spawner retains one default Entry"),
		EntryNodes.Num(), 1);
	TArray<UAnimStateNode*> StateNodes;
	MachineGraph->GetNodesOfClass<UAnimStateNode>(StateNodes);
	UAnimStateNode* StateNode = nullptr;
	for (UAnimStateNode* Node : StateNodes)
	{
		if (Node
			&& Node->GetClass() == UAnimStateNode::StaticClass()
			&& Node->GetBoundGraph()
			&& Node->GetBoundGraph()->GetFName() == TEXT("Idle"))
		{
			StateNode = Node;
			break;
		}
	}
	if (!TestNotNull(TEXT("the exact native State action survives reload"),
		StateNode))
	{
		return false;
	}
	TestTrue(TEXT("the State bound graph remains registered"),
		Graphs.Contains(StateNode->GetBoundGraph()));
	TestTrue(TEXT("the State bound graph remains target-owned"),
		StateNode->GetBoundGraph()->GetPathName().StartsWith(ObjectPath));

	if (!ExerciseAnimationWorkspace(*this, Reloaded, ObjectPath, PackageName)) return false;
	TArray<uint8> BeforeLateBytes;
	if (!TestTrue(TEXT("the accepted Animation Blueprint supplies a disk baseline"),
		FFileHelper::LoadFileToArray(BeforeLateBytes, *Filename)))
	{
		return false;
	}
	const bool bSavedThreaded = Reloaded->bUseMultiThreadedAnimationUpdate;
	const bool bSavedWarning = Reloaded->bWarnAboutBlueprintUsage;
	Reloaded->bWarnAboutBlueprintUsage = !bSavedWarning;
	FindFProperty<FIntProperty>(Reloaded->GeneratedClass, TEXT("WorkCount"))->SetPropertyValue_InContainer(Reloaded->GeneratedClass->GetDefaultObject(), 19);
	Reloaded->MarkPackageDirty();
	const auto BeforeDirtyGraphs = CaptureAnimationGraphs(*Reloaded);
	auto PublishedGraphs = BeforeDirtyGraphs;
	for (auto& Pair : PublishedGraphs) Pair.Value = Pair.Value.Replace(TEXT("AnimGraphAfter"), TEXT("AnimGraphPublished"));
	const auto GenericEdit = GenericAnimationEdit(*Reloaded);
	UeAgentAnimationAuthoring::FAssetApplyRequest EarlyRequest;
	EarlyRequest.Kind = UeAgentAnimationAuthoring::EAssetKind::AnimBlueprint;
	EarlyRequest.AssetPath = PackageName; EarlyRequest.BlueprintSurfaces = GenericEdit; EarlyRequest.bCompile = true; EarlyRequest.bSave = true;
	UeAgentAnimationAuthoring::FAssetApplyRequest NonPersistent = EarlyRequest; NonPersistent.bSave = false;
	UeAgentAnimationAuthoring::FAssetApplyResult Rejected;
	TestFalse(TEXT("new unsaved generic mutation rejects before write"), UeAgentAnimationAuthoring::ApplyAsset(NonPersistent, Rejected));
	TestEqual(TEXT("unsaved generic mutation has explicit contract error"), Rejected.Diagnostic.Code, FString(TEXT("animation_blueprint_surfaces_persistent_update_required")));
	class FRejectAnimationStage final : public UeAgentAuthoringOperationApply::IDurablePackageStager
	{
	public:
		TFunction<void()> Observe;
		TWeakObjectPtr<UAnimBlueprint> Original;
		TSharedRef<IDurablePackageStager> Inner = UeAgentAuthoringOperationApply::CreateDefaultDurablePackageStager();
		bool BuildArtifactInventory(const TArray<UeAgentAuthoringOperationApply::FPackageDurablePlan>& Plans, TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts, UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) override { return Inner->BuildArtifactInventory(Plans, Artifacts, Failure); }
		bool StagePackages(const TArray<UeAgentAuthoringOperationApply::FPackageDurablePlan>& Plans, TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts, UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) override
		{
			if (!Inner->StagePackages(Plans, Artifacts, Failure)) return false;
			if (!Original.IsValid() || WorkCount(*Original) != 27) return true;
			Observe(); Failure.Diagnostic.Code = TEXT("test_animation_blueprint_early_rejection"); Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code; return false;
		}
	};
	bool EarlyObserved = false, EarlyFinalCalled = false;
	auto Stager = MakeShared<FRejectAnimationStage>(); Stager->Original = Reloaded;
	Stager->Observe = [&]()
	{
		TArray<uint8> Bytes;
		EarlyObserved = Stager->Original.IsValid() && WorkCount(*Stager->Original) == 27 &&
			PublishedGraphs.OrderIndependentCompareEqual(CaptureAnimationGraphs(*Stager->Original)) && !EarlyFinalCalled &&
			FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes == BeforeLateBytes;
	};
	EarlyRequest.PackageStager = Stager;
	EarlyRequest.FinalReadback = [&](auto&) { EarlyFinalCalled = true; return true; };
	UeAgentAnimationAuthoring::FAssetApplyResult EarlyResult;
	TestFalse(TEXT("post-mutation staging rejection fails before persistent readback"), UeAgentAnimationAuthoring::ApplyAsset(EarlyRequest, EarlyResult));
	TestTrue(TEXT("early failure observes generic changes then restores through checkpoint"), EarlyObserved && !EarlyFinalCalled && EarlyResult.bRollbackAttempted && EarlyResult.bRollbackSucceeded);
	Reloaded = LoadObject<UAnimBlueprint>(nullptr, *ObjectPath);
	TArray<uint8> EarlyRestoredBytes;
	if (!TestTrue(TEXT("early recovery independently preserves dirty memory and original disk"), Reloaded && WorkCount(*Reloaded) == 19 && Reloaded->GetOutermost()->IsDirty() &&
		BeforeDirtyGraphs.OrderIndependentCompareEqual(CaptureAnimationGraphs(*Reloaded)) && FFileHelper::LoadFileToArray(EarlyRestoredBytes, *Filename) && EarlyRestoredBytes == BeforeLateBytes)) return false;
	TWeakObjectPtr<UAnimBlueprint> BeforeNative = Reloaded;

	const TSharedRef<FJsonObject> LateProperty = MakeShared<FJsonObject>();
	LateProperty->SetStringField(
		TEXT("property_path"), TEXT("bUseMultiThreadedAnimationUpdate"));
	LateProperty->SetBoolField(TEXT("value"), !bSavedThreaded);
	UeAgentAnimationAuthoring::FAssetApplyRequest LateRequest;
	LateRequest.Kind = UeAgentAnimationAuthoring::EAssetKind::AnimBlueprint;
	LateRequest.Operation = UeAgentApplyInputContract::EOperation::Update;
	LateRequest.AssetPath = PackageName;
	LateRequest.Properties = {MakeShared<FJsonValueObject>(LateProperty)};
	LateRequest.BlueprintSurfaces = GenericEdit;
	LateRequest.bCompile = true;
	LateRequest.bSave = true;
	bool bObservedPublished = false;
	LateRequest.FinalReadback =
		[&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
		{
			UAnimBlueprint* Current = LoadObject<UAnimBlueprint>(nullptr, *ObjectPath);
			TArray<uint8> PublishedBytes;
			TArray<UEdGraph*> CurrentGraphs;
			if (Current) Current->GetAllGraphs(CurrentGraphs);
			bObservedPublished = !BeforeNative.IsValid() && Current
				&& WorkCount(*Current) == 27 && PublishedGraphs.OrderIndependentCompareEqual(CaptureAnimationGraphs(*Current))
				&& Current->bUseMultiThreadedAnimationUpdate == !bSavedThreaded
				&& Current->bWarnAboutBlueprintUsage == !bSavedWarning
				&& !Current->GetOutermost()->IsDirty()
				&& Current->GeneratedClass && Current->Status != BS_Error
				&& CurrentGraphs.ContainsByPredicate([](const UEdGraph* Graph)
					{
						return Graph && Graph->GetFName() == TEXT("Locomotion");
					})
				&& FFileHelper::LoadFileToArray(PublishedBytes, *Filename)
				&& PublishedBytes != BeforeLateBytes;
			TestTrue(TEXT("late failure occurs after native compile and clean disk publication"),
				bObservedPublished);
			Failure.Diagnostic.Code = TEXT("test_animation_blueprint_late_rejection");
			Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
			return false;
		};
	UeAgentAnimationAuthoring::FAssetApplyResult LateResult;
	TestFalse(TEXT("final readback rejection fails the Animation Blueprint transaction"),
		UeAgentAnimationAuthoring::ApplyAsset(
			LateRequest, LateResult, TEXT("animation-blueprint-atomic-late")));
	TestEqual(TEXT("the original late failure survives recovery"),
		LateResult.Diagnostic.Code,
		FString(TEXT("test_animation_blueprint_late_rejection")));
	TestTrue(TEXT("the common checkpoint restores the Animation Blueprint"),
		bObservedPublished && LateResult.bRollbackAttempted
			&& LateResult.bRollbackSucceeded);
	UAnimBlueprint* Restored = LoadObject<UAnimBlueprint>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("the restored Animation Blueprint resolves"), Restored))
	{
		return false;
	}
	TArray<uint8> RestoredBytes;
	TestTrue(TEXT("late rejection restores the original Animation Blueprint disk bytes"),
		FFileHelper::LoadFileToArray(RestoredBytes, *Filename)
			&& RestoredBytes == BeforeLateBytes);
	TestTrue(TEXT("late rejection restores dirty memory independently of disk"),
		Restored->GetOutermost()->IsDirty()
			&& WorkCount(*Restored) == 19 && BeforeDirtyGraphs.OrderIndependentCompareEqual(CaptureAnimationGraphs(*Restored))
			&& Restored->bUseMultiThreadedAnimationUpdate == bSavedThreaded
			&& Restored->bWarnAboutBlueprintUsage == !bSavedWarning);
	TArray<UEdGraph*> RestoredGraphs;
	Restored->GetAllGraphs(RestoredGraphs);
	TestTrue(TEXT("late recovery preserves the non-target StateMachine graph"),
		Restored->GeneratedClass && Restored->Status != BS_Error
			&& RestoredGraphs.ContainsByPredicate([](const UEdGraph* Graph)
			{
				return Graph && Graph->GetFName() == TEXT("Locomotion");
			}));

	DeleteAssetIfPresent(ObjectPath);
	UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
		*this, PackageName);
	TestFalse(TEXT("the task-owned AnimBlueprint leaves no asset file"),
		IFileManager::Get().FileExists(*Filename));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208AnimationGraphDeclarationTest,
	"UeAgentInterface.T208.AnimationBlueprint.GraphDeclaration", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208AnimationGraphDeclarationTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	UE::UAI::Contract::FDiagnostic Error;
	if (!UeAgentBlueprintAuthoring::RegisterAuthoring(Error)
		|| !UeAgentAnimationAuthoring::RegisterAuthoring(Error)) { AddError(Error.ToPublicDetail()); return false; }
	const FString PackageName = TEXT("/Game/UAI/Transient/T85AnimGraphDeclaration_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/Widget");
	const FString ObjectPath = PackageName + TEXT(".Widget");
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T208/GraphDeclaration") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	class FNeverCancelled final : public Contract::IUeAgentCancellationSignal { public: bool IsCancelled() const override { return false; } };
	TSharedPtr<DomainAdapters::AssetDestructiveImport::IUeAgentAssetDeleteOwner> DeleteOwner;
	UE::UAI::Contract::FDiagnostic DeleteOwnerError;
	if (!DomainAdapters::AssetDestructiveImport::TryCreateAssetDeleteOwner(Workspace, TEXT("graph-declaration"), DeleteOwner, DeleteOwnerError)) return false;
	ON_SCOPE_EXIT
	{
		if (UObject* Asset = FindObject<UObject>(nullptr, *ObjectPath))
		{
			Asset->GetOutermost()->SetDirtyFlag(false);
			TestTrue(TEXT("graph declaration fixture formal cleanup"), DeleteOwner->Delete({TEXT("graph-declaration-cleanup"), {ObjectPath}, false, TEXT("t208")}, MakeShared<FNeverCancelled>()).IsSuccess());
		}
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	auto* Reference = LoadObject<UAnimSequence>(nullptr, TEXT("/Game/Characters/PaladinRootMotion/Animations/sword_and_shield_idle_RM.sword_and_shield_idle_RM"));
	if (!Reference) return false;
	auto* Factory = NewObject<UAnimBlueprintFactory>(); Factory->ParentClass = UAnimInstance::StaticClass(); Factory->TargetSkeleton = Reference->GetSkeleton();
	auto* Blueprint = Cast<UAnimBlueprint>(Factory->FactoryCreateNew(UAnimBlueprint::StaticClass(), CreatePackage(*PackageName), TEXT("Widget"), RF_Public | RF_Standalone, nullptr, GWarn));
	if (!TestNotNull(TEXT("native factory fixture with real skeleton"), Blueprint)) return false;
	FAssetRegistryModule::AssetCreated(Blueprint);
	TArray<UEdGraph*> AllGraphs; Blueprint->GetAllGraphs(AllGraphs);
	UEdGraph** AnimGraph = AllGraphs.FindByPredicate([](UEdGraph* Graph) { return Graph && Graph->IsA<UAnimationGraph>(); });
	if (!AnimGraph) return false;
	FGraphNodeCreator<UAnimGraphNode_StateMachine> MachineCreator(**AnimGraph);
	auto* Machine = MachineCreator.CreateNode(); MachineCreator.Finalize();
	if (!TestNotNull(TEXT("native state-machine fixture graph"), Machine->EditorStateMachineGraph.Get())) return false;
	FGraphNodeCreator<UAnimStateNode> StateCreator(*Machine->EditorStateMachineGraph);
	StateCreator.CreateNode(); StateCreator.Finalize();
	FEdGraphPinType Type; Type.PinCategory = UEdGraphSchema_K2::PC_Int;
	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("WorkCount"), Type, TEXT("7"))) return false;
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	const FGuid MemberId = Blueprint->NewVariables[0].VarGuid;
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Blueprint->GetOutermost(), Blueprint, *Filename, Save)) return false;
	Blueprint->GetOutermost()->SetDirtyFlag(false);
	TArray<uint8> OriginalDisk; if (!FFileHelper::LoadFileToArray(OriginalDisk, *Filename)) return false;
	const bool OriginalWarning = Blueprint->bWarnAboutBlueprintUsage;
	Blueprint->bWarnAboutBlueprintUsage = !OriginalWarning;
	FindFProperty<FIntProperty>(Blueprint->GeneratedClass, TEXT("WorkCount"))->SetPropertyValue_InContainer(Blueprint->GeneratedClass->GetDefaultObject(), 19);
	Blueprint->MarkPackageDirty();
	const auto OriginalGraphs = CaptureAnimationGraphs(*Blueprint);
	auto Header = MakeShared<FJsonObject>(); Header->SetStringField(TEXT("name"), TEXT("DeclaredFunction")); Header->SetStringField(TEXT("graph_kind"), TEXT("function_graph"));
	auto PinType = MakeShared<FJsonObject>(); PinType->SetStringField(TEXT("pin_category"), TEXT("bool"));
	auto Pin = MakeShared<FJsonObject>(); Pin->SetStringField(TEXT("name"), TEXT("Enabled")); Pin->SetObjectField(TEXT("type"), PinType);
	auto Signature = MakeShared<FJsonObject>(); Signature->SetArrayField(TEXT("inputs"), {MakeShared<FJsonValueObject>(Pin)}); Signature->SetArrayField(TEXT("outputs"), {});
	Header->SetObjectField(TEXT("signature"), Signature);
	auto GraphSpec = MakeShared<FJsonObject>(); GraphSpec->SetStringField(TEXT("operation"), TEXT("create")); GraphSpec->SetObjectField(TEXT("graph"), Header);
	auto Graphs = MakeShared<FJsonObject>(); Graphs->SetObjectField(TEXT("function_graph:DeclaredFunction"), GraphSpec);
	auto Extension = MakeShared<FJsonObject>(); Extension->SetObjectField(TEXT("graphs"), Graphs);
	TWeakObjectPtr<UAnimBlueprint> Before = Blueprint;
	bool Published = false;
	UeAgentAnimationAuthoring::FAssetApplyRequest Request;
	Request.Kind = UeAgentAnimationAuthoring::EAssetKind::AnimBlueprint; Request.AssetPath = PackageName;
	Request.BlueprintSurfaces = Extension; Request.bCompile = true; Request.bSave = true;
	Request.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
	{
		auto* Current = LoadObject<UAnimBlueprint>(nullptr, *ObjectPath);
		UEdGraph* NewGraph = nullptr;
		if (Current) for (UEdGraph* Graph : Current->FunctionGraphs) if (Graph && Graph->GetName() == TEXT("DeclaredFunction")) NewGraph = Graph;
		TArray<uint8> PublishedDisk;
		Published = !Before.IsValid() && Current && NewGraph && NewGraph->GetSchema()->GetClass() == UEdGraphSchema_K2::StaticClass()
			&& Current->FunctionGraphs.Contains(NewGraph) && Current->GeneratedClass && Current->Status != BS_Error && !Current->GetOutermost()->IsDirty()
			&& WorkCount(*Current) == 19 && Current->NewVariables[0].VarGuid == MemberId
			&& OriginalGraphs.OrderIndependentCompareEqual(CaptureAnimationGraphs(*Current, TEXT("DeclaredFunction")))
			&& FFileHelper::LoadFileToArray(PublishedDisk, *Filename) && PublishedDisk != OriginalDisk;
		bool Interface = false;
		if (NewGraph) for (UEdGraphNode* Node : NewGraph->Nodes) if (auto* Entry = Cast<UK2Node_FunctionEntry>(Node)) Interface |= Entry->FindPin(TEXT("Enabled")) != nullptr;
		Published &= Interface;
		TestTrue(TEXT("late callback first observes newly created native function/interface compiled saved and reloaded"), Published);
		Failure.Diagnostic.Code = TEXT("test_animation_graph_declaration_late"); Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code; return false;
	};
	Blueprint = nullptr; Machine = nullptr; AllGraphs.Reset();
	UeAgentAnimationAuthoring::FAssetApplyResult Result;
	TestFalse(TEXT("late new graph rejection fails"), UeAgentAnimationAuthoring::ApplyAsset(Request, Result, TEXT("graph-declaration-late")));
	if (!TestTrue(TEXT("new graph checkpoint restores after observed publication"), Published && Result.bRollbackAttempted && Result.bRollbackSucceeded)) { AddError(Result.Diagnostic.Code + TEXT(":") + Result.Diagnostic.InternalContext); return false; }
	Blueprint = LoadObject<UAnimBlueprint>(nullptr, *ObjectPath);
	TArray<uint8> RestoredDisk;
	if (!TestTrue(TEXT("original disk bytes restore independently"), FFileHelper::LoadFileToArray(RestoredDisk, *Filename) && RestoredDisk == OriginalDisk)
		|| !TestTrue(TEXT("dirty memory and all original native identities restore independently"), Blueprint && Blueprint->GetOutermost()->IsDirty()
			&& WorkCount(*Blueprint) == 19 && Blueprint->bWarnAboutBlueprintUsage == !OriginalWarning
			&& Blueprint->NewVariables[0].VarGuid == MemberId && OriginalGraphs.OrderIndependentCompareEqual(CaptureAnimationGraphs(*Blueprint)))) return false;
	const auto* Profile = UeAgentAnimationAuthoring::FindProfileByTypeId(TEXT("anim_blueprint"));
	UeAgentExportWorkspaceWriter::FWorkspaceResult Exported;
	if (!UeAgentAnimationAuthoring::ExportAssetWorkspace(*Profile, *Blueprint, Workspace, TEXT("llm_authoring"), Exported)) { AddError(Error.ToPublicDetail()); return false; }
	TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
	int32 Collections = 0;
	for (const FString& File : Files)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text; TSharedPtr<FJsonObject> Document;
		if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
		const TSharedPtr<FJsonObject>* Declarations = nullptr;
		if (!Document->TryGetObjectField(TEXT("graph_declarations"), Declarations)) continue;
		auto NewGraph = MakeShared<FJsonObject>(); NewGraph->SetStringField(TEXT("graph_kind"), TEXT("function_graph"));
		(*Declarations)->SetObjectField(TEXT("DeclaredFunction"), NewGraph); ++Collections;
		if (!CommandContract::Json::TryCanonicalize(Document, Text, Error) || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	}
	if (!TestEqual(TEXT("ordinary graph declaration collection always exports once"), Collections, 1)) return false;
	auto Directory = MakeShared<FJsonObject>(); Directory->SetStringField(TEXT("kind"), TEXT("directory")); Directory->SetStringField(TEXT("path"), Workspace);
	HjsonWorkspace::FResolvedContentGraphImportSourcePtr Source; auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
	FString Revision, Authorization; HjsonWorkspace::FWorkspaceError WorkspaceError;
	if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Directory, PackageName, Resources, Source, Revision, Authorization, WorkspaceError)) { AddError(WorkspaceError.GetCode()); return false; }
	auto Params = MakeShared<FJsonObject>(); Params->SetStringField(TEXT("target"), PackageName); Params->SetStringField(TEXT("expected_revision"), Revision); Params->SetObjectField(TEXT("source"), Source->MakeNormalizedSourceObject());
	FString ParamsText, Payload; bool ReadOnly = true;
	if (!CommandContract::Json::TryCanonicalize(Params, ParamsText, Error)) return false;
	Blueprint = nullptr;
	if (!DomainAdapters::AnimationBlueprint::TryExecuteContentGraph(TEXT("content_graph.import_workspace"), PackageName, ParamsText, Payload, ReadOnly, WorkspaceError))
	{ AddError(WorkspaceError.GetCode() + TEXT(":") + WorkspaceError.GetSourceUnit()); return false; }
	Blueprint = LoadObject<UAnimBlueprint>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("ordinary declaration creates persistent function and preserves skeleton/state-machine/member identities"), Blueprint
		&& Blueprint->FunctionGraphs.ContainsByPredicate([](const UEdGraph* Graph) { return Graph && Graph->GetName() == TEXT("DeclaredFunction"); })
		&& OriginalGraphs.OrderIndependentCompareEqual(CaptureAnimationGraphs(*Blueprint, TEXT("DeclaredFunction")))
		&& WorkCount(*Blueprint) == 19 && Blueprint->NewVariables[0].VarGuid == MemberId && !Blueprint->GetOutermost()->IsDirty())) return false;
	if (!UeAgentAnimationAuthoring::ExportAssetWorkspace(*Profile, *Blueprint, Workspace + TEXT("-readback"), TEXT("llm_authoring"), Exported)) return false;
	TArray<uint8> BeforeRemoveDisk;
	if (!FFileHelper::LoadFileToArray(BeforeRemoveDisk, *Filename)) return false;
	const auto BeforeRemoveGraphs = CaptureAnimationGraphs(*Blueprint);
	FindFProperty<FIntProperty>(Blueprint->GeneratedClass, TEXT("WorkCount"))->SetPropertyValue_InContainer(Blueprint->GeneratedClass->GetDefaultObject(), 31);
	Blueprint->MarkPackageDirty();
	TWeakObjectPtr<UAnimBlueprint> BeforeRemove = Blueprint;
	auto RemoveSpec = MakeShared<FJsonObject>(); RemoveSpec->SetStringField(TEXT("name"), TEXT("DeclaredFunction"));
	RemoveSpec->SetStringField(TEXT("graph_kind"), TEXT("function_graph")); RemoveSpec->SetStringField(TEXT("operation"), TEXT("remove"));
	auto RemoveGraphs = MakeShared<FJsonObject>(); RemoveGraphs->SetObjectField(TEXT("function_graph:DeclaredFunction"), RemoveSpec);
	auto RemoveExtension = MakeShared<FJsonObject>(); RemoveExtension->SetObjectField(TEXT("graphs"), RemoveGraphs);
	UeAgentAnimationAuthoring::FAssetApplyRequest RemoveRequest;
	RemoveRequest.Kind = UeAgentAnimationAuthoring::EAssetKind::AnimBlueprint; RemoveRequest.AssetPath = PackageName;
	RemoveRequest.BlueprintSurfaces = RemoveExtension; RemoveRequest.bCompile = true; RemoveRequest.bSave = true;
	bool RemovePublished = false;
	RemoveRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
	{
		auto* Current = LoadObject<UAnimBlueprint>(nullptr, *ObjectPath); TArray<uint8> Disk;
		RemovePublished = !BeforeRemove.IsValid() && Current && Current->GeneratedClass && Current->Status != BS_Error
			&& !Current->GetOutermost()->IsDirty() && WorkCount(*Current) == 31
			&& !Current->FunctionGraphs.ContainsByPredicate([](const UEdGraph* Graph) { return Graph && Graph->GetName() == TEXT("DeclaredFunction"); })
			&& OriginalGraphs.OrderIndependentCompareEqual(CaptureAnimationGraphs(*Current))
			&& Current->NewVariables[0].VarGuid == MemberId
			&& FFileHelper::LoadFileToArray(Disk, *Filename) && Disk != BeforeRemoveDisk;
		TestTrue(TEXT("remove late first observes absent function compiled saved reloaded with non-target identities intact"), RemovePublished);
		Failure.Diagnostic.Code = TEXT("test_animation_graph_remove_late"); Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code; return false;
	};
	Blueprint = nullptr;
	UeAgentAnimationAuthoring::FAssetApplyResult RemoveResult;
	TestFalse(TEXT("late graph remove rejection fails"), UeAgentAnimationAuthoring::ApplyAsset(RemoveRequest, RemoveResult, TEXT("graph-remove-late")));
	if (!TestTrue(TEXT("published graph deletion rolls back"), RemovePublished && RemoveResult.bRollbackAttempted && RemoveResult.bRollbackSucceeded)) return false;
	Blueprint = LoadObject<UAnimBlueprint>(nullptr, *ObjectPath); TArray<uint8> RemoveRestoredDisk;
	if (!TestTrue(TEXT("remove restores original disk independently"), FFileHelper::LoadFileToArray(RemoveRestoredDisk, *Filename) && RemoveRestoredDisk == BeforeRemoveDisk)
		|| !TestTrue(TEXT("remove restores dirty memory including deleted graph identity"), Blueprint && Blueprint->GetOutermost()->IsDirty() && WorkCount(*Blueprint) == 31
			&& Blueprint->NewVariables[0].VarGuid == MemberId && BeforeRemoveGraphs.OrderIndependentCompareEqual(CaptureAnimationGraphs(*Blueprint)))) return false;
	return !HasAnyErrors();
}
