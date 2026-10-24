#include "Misc/AutomationTest.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "OperationOwners/Pcg/UeAgentPcgDomainService.h"

#include "QueryOwners/Pcg/UeAgentPcgNodeCandidateQuery.h"
#include "Domains/PCG/Authoring/UeAgentPCGGraphAuthoring.h"
#include "Domains/PCG/UeAgentPCGDomainBackend.h"

namespace
{
	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
			UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	TSharedPtr<FJsonObject> MakeCandidateGraphDocument(
		const TCHAR* SelectorField,
		const FString& SelectorValue,
		const FString& FromPin = FString(),
		const FString& ToPin = FString())
	{
		TSharedRef<FJsonObject> Candidate = MakeShared<FJsonObject>();
		Candidate->SetStringField(SelectorField, SelectorValue);
		TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetObjectField(TEXT("candidate"), Candidate);
		Node->SetArrayField(TEXT("position"), {
			MakeShared<FJsonValueNumber>(120.0),
			MakeShared<FJsonValueNumber>(80.0),
		});
		TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
		Nodes->SetObjectField(TEXT("CandidateNode"), Node);
		TSharedRef<FJsonObject> Edges = MakeShared<FJsonObject>();
		if (!FromPin.IsEmpty() && !ToPin.IsEmpty())
		{
			TSharedRef<FJsonObject> From = MakeShared<FJsonObject>();
			From->SetStringField(TEXT("node"), TEXT("CandidateNode"));
			From->SetStringField(TEXT("pin"), FromPin);
			TSharedRef<FJsonObject> To = MakeShared<FJsonObject>();
			To->SetStringField(TEXT("node"), UeAgentPCG::OutputNodeId);
			To->SetStringField(TEXT("pin"), ToPin);
			TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
			Edge->SetObjectField(TEXT("from"), From);
			Edge->SetObjectField(TEXT("to"), To);
			Edges->SetObjectField(
				UeAgentPCGAuthoring::MakeEdgeKey(
					TEXT("CandidateNode"), FromPin,
					UeAgentPCG::OutputNodeId, ToPin),
				Edge);
		}
		TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetObjectField(TEXT("nodes"), Nodes);
		Document->SetObjectField(TEXT("edges"), Edges);
		return Document;
	}

	bool JsonArraysEqual(
		const TSharedPtr<FJsonObject>& Left,
		const TSharedPtr<FJsonObject>& Right,
		const TCHAR* Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* LeftValues = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* RightValues = nullptr;
		if (!Left.IsValid() || !Right.IsValid()
			|| !Left->TryGetArrayField(Field, LeftValues) || !LeftValues
			|| !Right->TryGetArrayField(Field, RightValues) || !RightValues
			|| LeftValues->Num() != RightValues->Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < LeftValues->Num(); ++Index)
		{
			if (!(*LeftValues)[Index].IsValid()
				|| !(*RightValues)[Index].IsValid()
				|| !FJsonValue::CompareEqual(
					*(*LeftValues)[Index], *(*RightValues)[Index]))
			{
				return false;
			}
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T19PcgDomainTest,
	"UAI.StrictV2.T19.PCG.DOMAIN",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T19PcgDomainTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::OperationOwners::Pcg;

	TSharedPtr<IImportInitializationDomainProvider> Provider;
	FWorkspaceError Error;
	if (!TestTrue(
		TEXT("the PCG domain provider is created"),
		TryCreatePcgDomainProvider(Provider, Error))
		|| !Provider.IsValid())
	{
		AddError(Error.GetCode());
		return false;
	}

	struct FCase
	{
		const TCHAR* Type;
		EInitializationNodeRole Role;
		const TCHAR* Contract;
	};
	const TArray<FCase> Cases = {
		{TEXT("/Script/PCG.PCGGraph"), EInitializationNodeRole::RootTarget, TEXT("uai.init.pcg_graph.root/1")},
		{TEXT("/Script/PCG.PCGNode"), EInitializationNodeRole::OwnedObject, TEXT("uai.init.pcg.node/1")},
		{TEXT("uai.pcg.endpoint"), EInitializationNodeRole::ContainerItem, TEXT("uai.init.pcg.endpoint/1")},
		{TEXT("uai.pcg.parameter"), EInitializationNodeRole::ContainerItem, TEXT("uai.init.pcg.parameter/1")},
	};
	for (const FCase& Case : Cases)
	{
		const FImportInitializationDomainRequest Request(
			TEXT("content_graph.import_workspace"),
			TEXT("uai.pcg.edit/1"),
			TEXT("/Game/UAI/StrictV2/T19/Target.Target"),
			Case.Type,
			Case.Role,
			TEXT("{}"));
		FString Contract;
		TestTrue(
			*FString::Printf(TEXT("the PCG initialization surface resolves: %s"), Case.Type),
			Provider->Matches(Request)
				&& Provider->TryResolveInitializationContract(Request, Contract, Error));
		TestEqual(TEXT("the PCG initialization contract is exact"), Contract,
			FString(Case.Contract));
	}

	const FImportInitializationDomainRequest Unsupported(
		TEXT("content_graph.import_workspace"),
		TEXT("uai.pcg.edit/1"),
		TEXT("/Game/UAI/StrictV2/T19/Unsupported.Unsupported"),
		TEXT("/Script/Engine.SoundWave"),
		EInitializationNodeRole::RootTarget,
		TEXT("{}"));
	TestFalse(TEXT("an unregistered PCG root is rejected before write"),
		Provider->Matches(Unsupported));

	using namespace UE::UAI::QueryOwners::Pcg;
	UPCGGraph* CandidateTarget = NewObject<UPCGGraph>(GetTransientPackage());
	UClass* const ExpectedSettingsClass = LoadObject<UClass>(
		nullptr, TEXT("/Script/PCG.PCGCreatePointsGridSettings"));
	const UPCGSettings* const ExpectedSettings = ExpectedSettingsClass
		? Cast<UPCGSettings>(ExpectedSettingsClass->GetDefaultObject()) : nullptr;
	if (!TestNotNull(
		TEXT("the representative native PCG settings class resolves"),
		ExpectedSettings))
	{
		return false;
	}
	const FString ExpectedMenuName =
		ExpectedSettings->GetDefaultNodeTitle().ToString();
	FString ExpectedPublicName = ExpectedSettingsClass->GetName();
	ExpectedPublicName.RemoveFromStart(TEXT("PCG"), ESearchCase::IgnoreCase);
	ExpectedPublicName.RemoveFromEnd(TEXT("Settings"), ESearchCase::IgnoreCase);
	ExpectedPublicName = FName::NameToDisplayString(ExpectedPublicName, false);
	FPcgNodeCandidateQueryResult CandidateResult;
	FPcgNodeCandidateQueryError CandidateError;
	if (!TestTrue(
		TEXT("the PCG owner resolves real node candidates"),
		TryQueryPcgNodeCandidates(
			*CandidateTarget,
			ExpectedMenuName,
			CandidateResult,
			CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.PublicMessage);
		return false;
	}
	if (!TestTrue(TEXT("the real PCG node catalog is non-empty"),
		!CandidateResult.Candidates.IsEmpty()))
	{
		return false;
	}
	const FPcgNodeCandidate* const CandidatePtr =
		CandidateResult.Candidates.FindByPredicate(
			[&ExpectedPublicName](const FPcgNodeCandidate& Value)
			{
				return Value.bSupported
					&& Value.Name.Equals(ExpectedPublicName, ESearchCase::CaseSensitive);
			});
	if (!TestNotNull(
		TEXT("the exact native Create Points Grid action is returned"),
		CandidatePtr))
	{
		return false;
	}
	const FPcgNodeCandidate& Candidate = *CandidatePtr;
	TestFalse(TEXT("candidate.name is a readable original name"),
		Candidate.Name.IsEmpty());
	TestFalse(TEXT("candidate.name is the native menu label, not a class path"),
		Candidate.Name.StartsWith(TEXT("/Script/")));
	TestEqual(TEXT("candidate kind is explicit"), Candidate.Kind,
		FString(TEXT("pcg_node")));
	TestEqual(TEXT("the exact result is a verified native schema action"),
		Candidate.ActionKind,
		FString(TEXT("FPCGEditorGraphSchemaAction_NewNativeElement")));
	TestTrue(TEXT("the first PCG native-action equivalence class is executable"),
		Candidate.bSupported);
	FPcgNodeCandidate ResolvedCandidate;
	CandidateError = {};
	TestTrue(TEXT("the readable name re-enumerates one exact native action"),
		TryResolvePcgNodeCandidate(
			*CandidateTarget, Candidate.Name, FString(),
			ResolvedCandidate, CandidateError));
	TestEqual(TEXT("name resolution is exact"),
		ResolvedCandidate.Name, Candidate.Name);

	TArray<FPcgNodeCandidate> AllCandidates;
	{
		FPcgNodeCandidateQueryResult Result;
		CandidateError = {};
		if (!TestTrue(TEXT("the native catalog resolves"),
			TryQueryPcgNodeCandidates(
				*CandidateTarget, FString(), Result, CandidateError)))
	{
			AddError(CandidateError.Code + TEXT(":") + CandidateError.PublicMessage);
			return false;
		}
		AllCandidates = MoveTemp(Result.Candidates);
	}
	TMap<FString, int32> CandidateNameCounts;
	for (const FPcgNodeCandidate& Value : AllCandidates)
	{
		++CandidateNameCounts.FindOrAdd(Value.Name.ToLower());
	}
	TestEqual(TEXT("query deduplicates original names"),
		CandidateNameCounts.Num(), AllCandidates.Num());
	// Resolution above, rather than query deduplication, establishes uniqueness.
	const FPcgNodeCandidate* UniqueCandidate = CandidatePtr;

	UPCGGraph* const DirectGraph = NewObject<UPCGGraph>(GetTransientPackage());
	UPCGNode* DirectNode = nullptr;
	FPcgNodeCandidate DirectCandidate;
	CandidateError = {};
	if (!TestTrue(TEXT("the comparison node is created by the exact native action"),
		TryPerformPcgNodeCandidate(
			*DirectGraph,
			UniqueCandidate->Name,
			FString(),
			FVector2f(120.0f, 80.0f),
			DirectNode,
			DirectCandidate,
			CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.PublicMessage);
		return false;
	}
	if (!TestNotNull(TEXT("the exact native action returns its PCG node"),
		DirectNode))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> DirectNodeState =
		UeAgentPCG::NodeToJson(DirectGraph, DirectNode, TEXT("CandidateNode"));
	const FString FromPin = DirectNode->GetOutputPins().IsEmpty()
		? FString()
		: DirectNode->GetOutputPins()[0]->Properties.Label.ToString();
	const FString ToPin = DirectGraph->GetOutputNode()->GetInputPins().IsEmpty()
		? FString()
		: DirectGraph->GetOutputNode()->GetInputPins()[0]->Properties.Label.ToString();
	if (!TestTrue(TEXT("the native comparison node exposes an edgeable output"),
		!FromPin.IsEmpty() && !ToPin.IsEmpty()))
	{
		return false;
	}
	{
		TSharedPtr<FJsonObject> SettingsDocument =
			MakeCandidateGraphDocument(TEXT("name"), UniqueCandidate->Name);
		const TSharedPtr<FJsonObject>* Nodes = nullptr;
		const TSharedPtr<FJsonObject>* Node = nullptr;
		if (!SettingsDocument->TryGetObjectField(TEXT("nodes"), Nodes)
			|| !Nodes || !Nodes->IsValid()
			|| !(*Nodes)->TryGetObjectField(TEXT("CandidateNode"), Node)
			|| !Node || !Node->IsValid())
		{
			AddError(TEXT("settings-class compatibility fixture is invalid"));
			return false;
		}
		(*Node)->RemoveField(TEXT("candidate"));
		(*Node)->SetStringField(
			TEXT("settings_class"),
			DirectNode->GetSettings()->GetClass()->GetPathName());
		UPCGGraph* const Graph = NewObject<UPCGGraph>(GetTransientPackage());
		UeAgentPCGAuthoring::FGraphApplyResult ApplyResult;
		FString ApplyError;
		TestTrue(TEXT("an unhandled node retains settings_class compatibility"),
			UeAgentPCGAuthoring::ApplyGraphDocument(
				*Graph, SettingsDocument, false, ApplyResult, ApplyError));
		TestTrue(TEXT("settings_class compatibility creates the HJSON node"),
			UeAgentPCG::BuildExportNodeMapById(Graph).Contains(
				TEXT("CandidateNode")));
	}
	{
		UPCGGraph* const Graph = NewObject<UPCGGraph>(GetTransientPackage());
		UeAgentPCGAuthoring::FGraphApplyResult PreflightResult;
		FString PreflightError;
		TestTrue(TEXT("candidate creation uses the native action in preflight"),
			UeAgentPCGAuthoring::PreflightGraphDocument(
				Graph,
				MakeCandidateGraphDocument(
					TEXT("name"), UniqueCandidate->Name, FromPin, ToPin),
				false,
				PreflightResult,
				PreflightError));
		TestEqual(TEXT("preflight validates the native-created edge"),
			PreflightResult.EdgeCount, 1);
		TestFalse(TEXT("preflight does not mutate the caller graph"),
			UeAgentPCG::BuildExportNodeMapById(Graph).Contains(
				TEXT("CandidateNode")));
	}

	for (const TPair<const TCHAR*, FString> Selector : {
		TPair<const TCHAR*, FString>(TEXT("name"), UniqueCandidate->Name),
	})
	{
		UPCGGraph* const Graph = NewObject<UPCGGraph>(GetTransientPackage());
		UeAgentPCGAuthoring::FGraphApplyResult ApplyResult;
		FString ApplyError;
		if (!TestTrue(
			*FString::Printf(
				TEXT("HJSON invokes the exact native action by %s"),
				Selector.Key),
			UeAgentPCGAuthoring::ApplyGraphDocument(
				*Graph,
				MakeCandidateGraphDocument(
					Selector.Key, Selector.Value, FromPin, ToPin),
				false,
				ApplyResult,
				ApplyError)))
		{
			AddError(ApplyError);
			return false;
		}
		const TMap<FString, UPCGNode*> NodesById =
			UeAgentPCG::BuildExportNodeMapById(Graph);
		UPCGNode* const* CreatedNode = NodesById.Find(TEXT("CandidateNode"));
		if (!TestNotNull(TEXT("the native node is bound to the HJSON identity"),
			CreatedNode ? *CreatedNode : nullptr))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> CreatedNodeState =
			UeAgentPCG::NodeToJson(
				Graph, *CreatedNode, TEXT("CandidateNode"));
		TestEqual(TEXT("native settings class is preserved"),
			CreatedNodeState->GetStringField(TEXT("settings_class")),
			DirectNodeState->GetStringField(TEXT("settings_class")));
		TestTrue(TEXT("native settings initialization is preserved"),
			JsonArraysEqual(
				CreatedNodeState, DirectNodeState,
				TEXT("settings_properties")));
		TestTrue(TEXT("native input pin initialization is preserved"),
			JsonArraysEqual(
				CreatedNodeState, DirectNodeState, TEXT("input_pins")));
		TestTrue(TEXT("native output pin initialization is preserved"),
			JsonArraysEqual(
				CreatedNodeState, DirectNodeState, TEXT("output_pins")));
		TestEqual(TEXT("the backend connects edges to the native-created node"),
			ApplyResult.EdgeCount, 1);
#if WITH_EDITOR
		int32 PositionX = 0;
		int32 PositionY = 0;
		(*CreatedNode)->GetNodePosition(PositionX, PositionY);
		TestEqual(TEXT("the backend applies the HJSON x position"),
			PositionX, 120);
		TestEqual(TEXT("the backend applies the HJSON y position"),
			PositionY, 80);
#endif
	}

	{
		UPCGGraph* const Graph = NewObject<UPCGGraph>(GetTransientPackage());
		TSharedPtr<FJsonObject> Missing = MakeCandidateGraphDocument(
			TEXT("name"), TEXT("__uai_missing_pcg_candidate__"));
		const TSharedPtr<FJsonObject>* MissingNodes = nullptr;
		const TSharedPtr<FJsonObject>* MissingNode = nullptr;
		if (Missing->TryGetObjectField(TEXT("nodes"), MissingNodes)
			&& MissingNodes && MissingNodes->IsValid()
			&& (*MissingNodes)->TryGetObjectField(
				TEXT("CandidateNode"), MissingNode)
			&& MissingNode && MissingNode->IsValid())
		{
			(*MissingNode)->SetStringField(
				TEXT("settings_class"),
				ExpectedSettings->GetClass()->GetPathName());
		}
		UeAgentPCGAuthoring::FGraphApplyResult ApplyResult;
		FString ApplyError;
		TestFalse(TEXT("a failed candidate does not fall back to settings_class"),
			UeAgentPCGAuthoring::ApplyGraphDocument(
				*Graph, Missing, false, ApplyResult, ApplyError));
		TestTrue(TEXT("missing candidate failure is explicit"),
			ApplyError == TEXT("candidate_name_not_found"));
		TestFalse(TEXT("failed override creates no settings-class fallback node"),
			UeAgentPCG::BuildExportNodeMapById(Graph).Contains(
				TEXT("CandidateNode")));
	}

	const FString FixturePackageName =
		TEXT("/Game/UAI/StrictV2/T19R63/PG_T19PublicR63");
	const FString FixtureAssetName = TEXT("PG_T19PublicR63");
	const FString FixtureObjectPath =
		FixturePackageName + TEXT(".") + FixtureAssetName;
	const FString FixtureFilename = FPackageName::LongPackageNameToFilename(
		FixturePackageName,
		FPackageName::GetAssetPackageExtension());
	DeleteAssetIfPresent(FixtureObjectPath);
	if (IFileManager::Get().FileExists(*FixtureFilename))
	{
		TestTrue(
			TEXT("a stale task-owned PCG fixture is removed before setup"),
			IFileManager::Get().Delete(*FixtureFilename, false, true));
	}
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(FixtureObjectPath);
	};
	UPackage* const FixturePackage = CreatePackage(*FixturePackageName);
	UPCGGraph* const FixtureGraph = FixturePackage
		? NewObject<UPCGGraph>(
			FixturePackage,
			*FixtureAssetName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!TestNotNull(TEXT("the task-owned PCG fixture is created"), FixtureGraph))
	{
		return false;
	}
	FixtureGraph->SetFlags(RF_Public | RF_Standalone);
	FixturePackage->MarkPackageDirty();
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	TestTrue(
		TEXT("the task-owned PCG fixture is persisted for the public vertical"),
		UPackage::SavePackage(
			FixturePackage,
			FixtureGraph,
			*FixtureFilename,
			SaveArgs));
	FixturePackage->SetDirtyFlag(false);
	DeleteAssetIfPresent(FixtureObjectPath);
	TestFalse(TEXT("the task-owned PCG fixture leaves no asset file"),
		IFileManager::Get().FileExists(*FixtureFilename));
	return !HasAnyErrors();
}
