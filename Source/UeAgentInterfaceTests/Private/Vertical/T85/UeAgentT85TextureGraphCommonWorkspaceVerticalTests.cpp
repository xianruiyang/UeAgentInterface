#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentSnapshotComposition.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "DomainAdapters/TextureGraph/UeAgentTextureGraphDomainMechanics.h"
#include "DomainAdapters/TextureGraph/UeAgentTextureGraphAuthoringApply.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntimeResolvers.h"
#include "OperationOwners/TextureGraph/UeAgentTextureGraphOperationOwner.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Expressions/Input/TG_Expression_Scalar.h"
#include "Expressions/Maths/TG_Expression_IfThenElse.h"
#include "UObject/UnrealType.h"
#include "Expressions/Output/TG_Expression_Output.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "TG_Graph.h"
#include "TG_Node.h"
#include "TG_Pin.h"
#include "TextureGraph.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
	using namespace UE::UAI;

	constexpr const TCHAR* InitialParameterAlias = TEXT("InitialGain");
	constexpr const TCHAR* AuthoredParameterAlias = TEXT("T85Gain");
	constexpr float AuthoredParameterValue = 0.375f;

	class FNeverCancelled final
		: public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	CommandEdge::FUeAgentCommandResult SubmitPublicCommand(
		const TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>&
			Registry,
		const TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const FString& RequestId,
		const FString& ParamsJson)
	{
		const auto* Command = Registry.IsValid()
			? Registry->FindExact(CommandId) : nullptr;
		if (!Command || !Dispatch.IsValid())
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t85-texture-graph"), RequestId, CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("texture_graph_public_command_missing"),
				TEXT("the public command is unavailable"));
		}
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t85-texture-graph\",")
			TEXT("\"request_id\":\"%s\",\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*RequestId, *CommandId, *Command->GetContractHash(), *ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		CommandEdge::FUeAgentCommandProtocolDiagnostic Diagnostic;
		TOptional<CommandEdge::FUeAgentCommandRequest> Request;
		if (!CommandEdge::FUeAgentCommandProtocolCodec().DecodeRequest(
				Bytes, Request, Diagnostic)
			|| !Request.IsSet())
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t85-texture-graph"), RequestId, CommandId,
				Command->GetContractHash(),
				TEXT("texture_graph_public_request_invalid"),
				TEXT("the public request could not be decoded"));
		}
		return Dispatch->Submit(
			Request.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("t85-texture-graph"),
				{TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")},
				TEXT("corr-t85-texture-graph")),
			MakeShared<FNeverCancelled>());
	}

	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
			UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	bool SaveGraph(
		UPackage& Package,
		UTextureGraph& Graph,
		const FString& Filename)
	{
		Package.MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const bool bSaved = UPackage::SavePackage(
			&Package, &Graph, *Filename, Args);
		if (bSaved)
		{
			Package.SetDirtyFlag(false);
		}
		return bSaved;
	}

	bool TryBuildExportParams(
		const FString& Target,
		const FString& Workspace,
		FString& OutParams,
		FString& OutError)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetStringField(TEXT("workspace_path"), Workspace);
		Params->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		return CommandContract::Json::TryCanonicalize(
			Params, OutParams, OutError);
	}

	bool TryBuildImportParams(
		const FString& Target,
		const FString& Workspace,
		FString& OutParams,
		FString& OutError)
	{
		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("directory"));
		Source->SetStringField(TEXT("path"), Workspace);
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetObjectField(TEXT("source"), Source);
		return CommandContract::Json::TryCanonicalize(
			Params, OutParams, OutError);
	}

	bool TryLoadTextureGraphDocument(
		const FString& Workspace,
		FString& OutFile,
		TSharedPtr<FJsonObject>& OutDocument,
		TSharedPtr<FJsonObject>& OutGraph,
		FString& OutError)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(
			Files, *Workspace, TEXT("*.hjson"), true, false);
		Files.Sort();
		for (const FString& File : Files)
		{
			if (File.EndsWith(TEXT(".interfaces.hjson"))
				|| File.EndsWith(TEXT("workspace.hjson")))
			{
				continue;
			}
			FString Text;
			TSharedPtr<FJsonObject> Document;
			if (FFileHelper::LoadFileToString(Text, *File)
				&& FUeAgentStructuredHjsonParser::ParseObject(
					Text, File, Document, OutError)
				&& Document.IsValid()
				&& Document->HasTypedField<EJson::Object>(TEXT("nodes"))
				&& Document->HasTypedField<EJson::Array>(TEXT("connections")))
			{
				OutFile = File;
				OutDocument = Document;
				OutGraph = MoveTemp(Document);
				return true;
			}
		}
		OutError = TEXT("texture_graph_visible_graph_not_found");
		return false;
	}

	bool TryEditReadableTextureGraph(
		const FString& Workspace,
		const FString& OutputAlias,
		FString& OutError,
		bool bReplaceConnections = false)
	{
		FString File;
		TSharedPtr<FJsonObject> Document;
		TSharedPtr<FJsonObject> Graph;
		if (!TryLoadTextureGraphDocument(
			Workspace, File, Document, Graph, OutError))
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* Nodes = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* ExistingConnections = nullptr;
		if (!Graph->TryGetObjectField(TEXT("nodes"), Nodes)
			|| !Nodes || !Nodes->IsValid()
			|| !Graph->TryGetArrayField(TEXT("connections"), ExistingConnections)
			|| !ExistingConnections)
		{
			OutError = TEXT("texture_graph_readable_collections_missing");
			return false;
		}
		TSharedPtr<FJsonObject> ScalarNode;
		FString ScalarAlias;
		FString ResolvedOutputAlias;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Nodes)->Values)
		{
			const TSharedPtr<FJsonObject> Candidate = Pair.Value.IsValid()
				&& Pair.Value->Type == EJson::Object ? Pair.Value->AsObject() : nullptr;
			FString Name = Pair.Key;
			if (Candidate.IsValid()) Candidate->TryGetStringField(TEXT("name"), Name);
			if (Candidate.IsValid() && Name == InitialParameterAlias)
			{
				ScalarNode = Candidate;
				ScalarAlias = Pair.Key;
			}
			if (Candidate.IsValid() && Name == OutputAlias)
			{
				ResolvedOutputAlias = Pair.Key;
			}
		}
		const TSharedPtr<FJsonObject>* Inputs = nullptr;
		const TSharedPtr<FJsonObject>* ScalarInput = nullptr;
		if (!ScalarNode.IsValid() || ScalarAlias.IsEmpty() || ResolvedOutputAlias.IsEmpty())
		{
			FString Visible;
			CommandContract::Json::TryCanonicalize(Document, Visible, OutError);
			OutError = TEXT("texture_graph_readable_node_missing:") + Visible;
			return false;
		}
		if (!ScalarNode->TryGetObjectField(TEXT("inputs"), Inputs)
			|| !Inputs || !Inputs->IsValid()
			|| !(*Inputs)->TryGetObjectField(GET_MEMBER_NAME_CHECKED(UTG_Expression_Scalar, Scalar).ToString(), ScalarInput)
			|| !ScalarInput || !ScalarInput->IsValid())
		{
			FString Visible;
			CommandContract::Json::TryCanonicalize(ScalarNode, Visible, OutError);
			OutError = TEXT("texture_graph_readable_parameter_missing:") + Visible;
			return false;
		}
		(*ScalarInput)->SetStringField(TEXT("alias"), AuthoredParameterAlias);
		(*ScalarInput)->SetNumberField(TEXT("value"), AuthoredParameterValue);

		TSharedRef<FJsonObject> Connection = MakeShared<FJsonObject>();
		Connection->SetStringField(
			TEXT("from"), ScalarAlias + TEXT(".ValueOut"));
		Connection->SetStringField(
			TEXT("to"), ResolvedOutputAlias + TEXT(".Source"));
		TArray<TSharedPtr<FJsonValue>> Connections = *ExistingConnections;
		if (bReplaceConnections) Connections.Reset();
		Connections.Add(MakeShared<FJsonValueObject>(Connection));
		Graph->SetArrayField(TEXT("connections"), MoveTemp(Connections));

		FString Updated;
		if (!CommandContract::Json::TryCanonicalize(
				Document, Updated, OutError)
			|| !FFileHelper::SaveStringToFile(
				Updated, *File,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("texture_graph_visible_hjson_write_failed");
			}
			return false;
		}
		return true;
	}

	bool TryReadbackReadableTextureGraph(
		const FString& Workspace,
		const FString& OutputAlias,
		FString& OutError)
	{
		FString File;
		TSharedPtr<FJsonObject> Document;
		TSharedPtr<FJsonObject> Graph;
		if (!TryLoadTextureGraphDocument(
			Workspace, File, Document, Graph, OutError))
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* Nodes = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
		if (!Graph->TryGetObjectField(TEXT("nodes"), Nodes)
			|| !Nodes || !Nodes->IsValid()
			|| !Graph->TryGetArrayField(TEXT("connections"), Connections)
			|| !Connections)
		{
			OutError = TEXT("texture_graph_readback_collections_missing");
			return false;
		}
		bool bParameterFound = false;
		FString ScalarAlias;
		FString ResolvedOutputAlias;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Nodes)->Values)
		{
			const TSharedPtr<FJsonObject> Node = Pair.Value.IsValid()
				&& Pair.Value->Type == EJson::Object ? Pair.Value->AsObject() : nullptr;
			const TSharedPtr<FJsonObject>* Inputs = nullptr;
			const TSharedPtr<FJsonObject>* ScalarInput = nullptr;
			if (!Node.IsValid()) continue;
			FString Name = Pair.Key;
			Node->TryGetStringField(TEXT("name"), Name);
			if (Name == OutputAlias) ResolvedOutputAlias = Pair.Key;
			if (!Node->TryGetObjectField(TEXT("inputs"), Inputs)
				|| !Inputs || !Inputs->IsValid()
				|| !(*Inputs)->TryGetObjectField(GET_MEMBER_NAME_CHECKED(UTG_Expression_Scalar, Scalar).ToString(), ScalarInput)
				|| !ScalarInput || !ScalarInput->IsValid()) continue;
			FString Alias;
			double Value = 0.0;
			if ((*ScalarInput)->TryGetStringField(TEXT("alias"), Alias)
				&& Alias == AuthoredParameterAlias
				&& (*ScalarInput)->TryGetNumberField(TEXT("value"), Value)
				&& FMath::IsNearlyEqual(
					static_cast<float>(Value), AuthoredParameterValue))
			{
				bParameterFound = true;
				ScalarAlias = Pair.Key;
			}
		}
		if (!bParameterFound)
		{
			OutError = TEXT("texture_graph_readback_parameter_missing");
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Connections)
		{
			const TSharedPtr<FJsonObject> Connection =
				Value.IsValid() && Value->Type == EJson::Object
					? Value->AsObject() : nullptr;
			FString From;
			FString To;
			if (Connection.IsValid()
				&& Connection->TryGetStringField(TEXT("from"), From)
				&& Connection->TryGetStringField(TEXT("to"), To)
				&& From == ScalarAlias + TEXT(".ValueOut")
				&& To == ResolvedOutputAlias + TEXT(".Source"))
			{
				return true;
			}
		}
		OutError = TEXT("texture_graph_readback_edge_missing");
		return false;
	}

	bool TryUnloadPackage(const FString& PackageName, FString& OutError)
	{
		UPackage* Package = FindPackage(nullptr, *PackageName);
		if (!Package)
		{
			OutError.Reset();
			return true;
		}
		Package->SetDirtyFlag(false);
		FText UnloadError;
		if (!UPackageTools::UnloadPackages({Package}, UnloadError, true))
		{
			OutError = UnloadError.ToString();
			return false;
		}
		CollectGarbage(RF_NoFlags, true);
		if (FindPackage(nullptr, *PackageName))
		{
			OutError = TEXT("texture_graph_package_remained_loaded");
			return false;
		}
		OutError.Reset();
		return true;
	}

	struct FNativeGraphValues
	{
		TMap<int32, FString> Nodes;
		TMap<FString, float> Parameters;
		TArray<FString> Edges;
		bool operator==(const FNativeGraphValues& Other) const
		{ return Nodes.OrderIndependentCompareEqual(Other.Nodes)
			&& Parameters.OrderIndependentCompareEqual(Other.Parameters) && Edges == Other.Edges; }
	};

	bool CaptureNativeValues(UTextureGraph* Asset, FNativeGraphValues& Out)
	{
		Out = {};
		if (!Asset || !Asset->Graph()) return false;
		bool bValid = true;
		Asset->Graph()->ForEachNodes([&](const UTG_Node* Node, const uint32)
		{
			if (!Node || !Node->GetExpression()) { bValid = false; return; }
			Out.Nodes.Add(Node->GetId().NodeIdx(), Node->GetExpression()->GetClass()->GetPathName()
				+ TEXT("|") + Node->GetExpression()->GetTitleName().ToString());
		});
		Asset->Graph()->ForEachPins([&](const UTG_Pin* Pin, const uint32, const uint32)
		{
			if (!Pin || !Pin->IsInput() || !Pin->IsParam()) return;
			float Value = 0;
			if (!Pin->IsArgScalar() || !Pin->GetValue(Value)) { bValid = false; return; }
			Out.Parameters.Add(Pin->GetAliasName().ToString(), Value);
		});
		Asset->Graph()->ForEachEdges([&](const UTG_Pin* From, const UTG_Pin* To)
		{
			if (!From || !To) { bValid = false; return; }
			Out.Edges.Add(FString::Printf(TEXT("%d.%s>%d.%s"),
				From->GetId().NodeIdx(), *From->GetArgumentName().ToString(),
				To->GetId().NodeIdx(), *To->GetArgumentName().ToString()));
		});
		Out.Edges.Sort();
		return bValid;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85TextureGraphCommonWorkspaceVerticalTest,
	"UAI.Root.T85.TextureGraph.CommonWorkspaceVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85TextureGraphCommonWorkspaceVerticalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;

	const FString PackageName =
		TEXT("/Game/UAI/Transient/T85TextureGraphCommon/TG_Common_")
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

	UPackage* Package = CreatePackage(*PackageName);
	UTextureGraph* Graph = Package
		? NewObject<UTextureGraph>(
			Package, *AssetName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!TestNotNull(
		TEXT("a persistent TextureGraph fixture is created"), Graph))
	{
		return false;
	}
	Graph->Construct(AssetName);
	FAssetRegistryModule::AssetCreated(Graph);
	UTG_Graph* NativeGraph = Graph->Graph();
	UTG_Node* ScalarNode = NativeGraph
		? NativeGraph->CreateExpressionNode(
			UTG_Expression_Scalar::StaticClass())
		: nullptr;
	UTG_Expression_Scalar* Scalar = ScalarNode
		? Cast<UTG_Expression_Scalar>(ScalarNode->GetExpression()) : nullptr;
	const UTG_Expression_Output* Output = nullptr;
	if (NativeGraph)
	{
		NativeGraph->ForEachNodes(
			[&Output](const UTG_Node* Node, const uint32)
			{
				if (!Output && Node)
				{
					Output = Cast<UTG_Expression_Output>(Node->GetExpression());
				}
			});
	}
	if (!TestNotNull(TEXT("the native scalar fixture is created"), Scalar)
		|| !TestNotNull(TEXT("the native output fixture exists"), Output))
	{
		return false;
	}
	Scalar->SetTitleName(InitialParameterAlias);
	const FString OutputAlias = Output->GetTitleName().ToString();
	if (!TestFalse(TEXT("the output has a readable alias"), OutputAlias.IsEmpty())
		|| !TestTrue(
			TEXT("the initial TextureGraph fixture is saved"),
			SaveGraph(*Package, *Graph, Filename)))
	{
		return false;
	}

	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(
			TEXT("the formal process active composition freezes"),
			TryComposeProcessActiveCommandComposition(
				Registry, Dispatch, Error)))
	{
		AddError(Error);
		return false;
	}

	const FString Workspace = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir()
		/ (TEXT("Automation/UAI/T85/TextureGraphExport_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	const FString ReadbackWorkspace = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir()
		/ (TEXT("Automation/UAI/T85/TextureGraphReadback_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
		IFileManager::Get().DeleteDirectory(*ReadbackWorkspace, false, true);
	};

	FString Params;
	if (!TestTrue(
			TEXT("the TextureGraph export request canonicalizes"),
			TryBuildExportParams(ObjectPath, Workspace, Params, Error)))
	{
		AddError(Error);
		return false;
	}
	const CommandEdge::FUeAgentCommandResult Export = SubmitPublicCommand(
		Registry, Dispatch, TEXT("content_graph.export_workspace"),
		TEXT("req-t85-tg-export"), Params);
	if (!TestTrue(
			TEXT("the public TextureGraph export publishes a workspace"),
			Export.IsOk()))
	{
		AddError(Export.GetErrorJson());
		return false;
	}
	if (!TestTrue(
			TEXT("the exported workspace exposes readable TextureGraph semantics"),
			TryEditReadableTextureGraph(Workspace, OutputAlias, Error)))
	{
		AddError(Error);
		return false;
	}

	if (!TestTrue(
			TEXT("the TextureGraph directory import request canonicalizes"),
			TryBuildImportParams(ObjectPath, Workspace, Params, Error)))
	{
		AddError(Error);
		return false;
	}
	const CommandEdge::FUeAgentCommandResult Import = SubmitPublicCommand(
		Registry, Dispatch, TEXT("content_graph.import_workspace"),
		TEXT("req-t85-tg-import"), Params);
	if (!TestTrue(
			TEXT("the public import uses the common persistence lifecycle"),
			Import.IsOk()))
	{
		AddError(Import.GetErrorJson());
		return false;
	}
	TSharedPtr<FJsonObject> ImportPayload;
	FString PayloadError;
	TestTrue(
		TEXT("the common import returns compact success"),
		CommandContract::Json::TryParseObject(
			Import.GetPayloadJson(), ImportPayload, PayloadError)
		&& ImportPayload.IsValid()
		&& ImportPayload->Values.Num() == 1
		&& ImportPayload->GetStringField(TEXT("status")) == TEXT("succeeded"));

	Graph = nullptr;
	NativeGraph = nullptr;
	ScalarNode = nullptr;
	Scalar = nullptr;
	Output = nullptr;
	Package = nullptr;
	if (!TestTrue(
			TEXT("the imported TextureGraph package unloads for durable readback"),
			TryUnloadPackage(PackageName, Error)))
	{
		AddError(Error);
		return false;
	}
	UTextureGraph* Reloaded = LoadObject<UTextureGraph>(nullptr, *ObjectPath);
	if (!TestNotNull(
			TEXT("the imported TextureGraph independently reloads"), Reloaded))
	{
		return false;
	}

	if (!TestTrue(
			TEXT("the independent TextureGraph re-export request canonicalizes"),
			TryBuildExportParams(
				ObjectPath, ReadbackWorkspace, Params, Error)))
	{
		AddError(Error);
		return false;
	}
	const CommandEdge::FUeAgentCommandResult ReExport = SubmitPublicCommand(
		Registry, Dispatch, TEXT("content_graph.export_workspace"),
		TEXT("req-t85-tg-readback"), Params);
	if (!TestTrue(
			TEXT("an independent public re-export reads the persisted graph"),
			ReExport.IsOk())
		|| !TestTrue(
			TEXT("re-export preserves readable parameter and edge semantics"),
			TryReadbackReadableTextureGraph(
				ReadbackWorkspace, OutputAlias, Error)))
	{
		if (!ReExport.IsOk()) AddError(ReExport.GetErrorJson());
		if (!Error.IsEmpty()) AddError(Error);
		return false;
	}

	UTG_Graph* ReloadedNativeGraph = Reloaded->Graph();
	UTG_Pin* ReloadedParameter = ReloadedNativeGraph
		? ReloadedNativeGraph->FindParamPin(FName(AuthoredParameterAlias))
		: nullptr;
	float ReloadedValue = 0.0f;
	TestNotNull(
		TEXT("the readable parameter alias survives native reload"),
		ReloadedParameter);
	TestTrue(
		TEXT("the authored scalar value survives native reload"),
		ReloadedParameter
			&& ReloadedParameter->GetValue(ReloadedValue)
			&& FMath::IsNearlyEqual(
				ReloadedValue, AuthoredParameterValue));
	bool bNativeEdgeFound = false;
	if (ReloadedNativeGraph && ReloadedParameter)
	{
		const UTG_Node* ParameterNode = ReloadedParameter->GetNodePtr();
		ReloadedNativeGraph->ForEachEdges(
			[&bNativeEdgeFound, &OutputAlias, ParameterNode](
				const UTG_Pin* From, const UTG_Pin* To)
			{
				const UTG_Expression_Output* TargetOutput = To && To->GetNodePtr()
					? Cast<UTG_Expression_Output>(
						To->GetNodePtr()->GetExpression())
					: nullptr;
				bNativeEdgeFound = bNativeEdgeFound
					|| (From && To
						&& From->GetNodePtr() == ParameterNode
						&& From->GetArgumentName() == FName(TEXT("ValueOut"))
						&& TargetOutput
						&& TargetOutput->GetTitleName().ToString() == OutputAlias
						&& To->GetArgumentName() == FName(TEXT("Source")));
			});
	}
	TestTrue(
		TEXT("the readable edge survives as a native TextureGraph connection"),
		bNativeEdgeFound);

	DeleteAssetIfPresent(ObjectPath);
	UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
		*this, PackageName);
	TestFalse(
		TEXT("the task-owned TextureGraph fixture leaves no asset file"),
		IFileManager::Get().FileExists(*Filename));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85TextureGraphAtomicLifecycleTest,
	"UAI.Root.T85.TextureGraph.AtomicLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85TextureGraphAtomicLifecycleTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	using namespace HjsonWorkspace;
	using namespace DomainAdapters::ContentGraphRead;
	const FString PackageName = TEXT("/Game/UAI/Transient/T85TextureGraphAtomic/TG_Atomic_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Name = FPackageName::GetLongPackageAssetName(PackageName);
	const FString Path = PackageName + TEXT(".") + Name;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()
		/ TEXT("Automation/UAI/T85/TextureGraphAtomic") / Name);
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(Path);
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
	};
	UPackage* Package = CreatePackage(*PackageName);
	UTextureGraph* Asset = NewObject<UTextureGraph>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("owned TextureGraph exists"), Asset)) return false;
	Asset->Construct(Name);
	FAssetRegistryModule::AssetCreated(Asset);
	UTG_Graph* Native = Asset->Graph();
	UTG_Node* First = Native->CreateExpressionNode(UTG_Expression_Scalar::StaticClass());
	UTG_Node* Neighbour = Native->CreateExpressionNode(UTG_Expression_Scalar::StaticClass());
	UTG_Node* Output = nullptr;
	Native->ForEachNodes([&](const UTG_Node* Node, const uint32)
	{
		if (Node && Cast<UTG_Expression_Output>(Node->GetExpression())) Output = const_cast<UTG_Node*>(Node);
	});
	if (!First || !Neighbour || !Output) { AddError(TEXT("native fixture nodes missing")); return false; }
	First->GetExpression()->SetTitleName(InitialParameterAlias);
	Neighbour->GetExpression()->SetTitleName(TEXT("NeighbourGain"));
	FTG_Name ScalarPin = GET_MEMBER_NAME_CHECKED(UTG_Expression_Scalar, Scalar);
	FTG_Name FromPin = TEXT("ValueOut");
	FTG_Name ToPin = TEXT("Source");
	if (!TestTrue(TEXT("disk neighbour value is authored"), Neighbour->GetInputPin(ScalarPin)->SetValue(2.0f))
		|| !TestTrue(TEXT("disk graph is saved"), SaveGraph(*Package, *Asset, Filename))) return false;
	TArray<uint8> DiskBefore;
	if (!TestTrue(TEXT("original disk bytes read independently"), FFileHelper::LoadFileToArray(DiskBefore, *Filename))) return false;
	UTG_Node* Removed = Native->CreateExpressionNode(UTG_Expression_Scalar::StaticClass());
	if (!TestNotNull(TEXT("dirty-only structural member exists"), Removed)) return false;
	Removed->GetExpression()->SetTitleName(TEXT("RemovedGain"));
	if (!TestTrue(TEXT("dirty first value set"), First->GetInputPin(ScalarPin)->SetValue(0.25f))
		|| !TestTrue(TEXT("dirty neighbour value set"), Neighbour->GetInputPin(ScalarPin)->SetValue(0.875f))
		|| !TestTrue(TEXT("dirty removed value set"), Removed->GetInputPin(ScalarPin)->SetValue(0.625f))
		|| !TestTrue(TEXT("dirty before connection set"), Native->Connect(*Removed, FromPin, *Output, ToPin))) return false;
	Asset->MarkPackageDirty();
	FNativeGraphValues Before;
	if (!TestTrue(TEXT("complete dirty native graph is captured independently"), CaptureNativeValues(Asset, Before))
		|| !TestEqual(TEXT("three native input parameters captured"), Before.Parameters.Num(), 3)
		|| !TestEqual(TEXT("four dirty native nodes captured"), Before.Nodes.Num(), 4)) return false;
	const int32 FirstId = First->GetId().NodeIdx();
	const int32 RemovedId = Removed->GetId().NodeIdx();
	const int32 OutputId = Output->GetId().NodeIdx();
	const FString OutputAlias = Output->GetExpression()->GetTitleName().ToString();
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error, Params;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)
		|| !TryBuildExportParams(Path, Workspace, Params, Error)) { AddError(Error); return false; }
	const auto Export = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"),
		TEXT("req-t85-texture-graph-atomic-export"), Params);
	if (!Export.IsOk()) { AddError(Export.GetErrorJson()); return false; }
	if (!TryEditReadableTextureGraph(Workspace, OutputAlias, Error, true)) { AddError(Error); return false; }
	// Remove the dirty-only member through the same ordinary keyed graph document.
	{
		FString File;
		TSharedPtr<FJsonObject> Document, GraphDocument;
		if (!TryLoadTextureGraphDocument(Workspace, File, Document, GraphDocument, Error)) { AddError(Error); return false; }
		const auto Nodes = GraphDocument->GetObjectField(TEXT("nodes"));
		FString RemoveKey;
		for (const auto& Pair : Nodes->Values)
		{
			FString VisibleName = Pair.Key;
			if (Pair.Value->Type == EJson::Object) Pair.Value->AsObject()->TryGetStringField(TEXT("name"), VisibleName);
			if (VisibleName == TEXT("RemovedGain")) RemoveKey = Pair.Key;
		}
		if (!TestFalse(TEXT("ordinary removed member identified"), RemoveKey.IsEmpty())) return false;
		Nodes->RemoveField(RemoveKey);
		FString Text;
		if (!CommandContract::Json::TryCanonicalize(Document, Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{ AddError(Error); return false; }
	}

	auto Resources = MakeSemanticWorkspaceResourceTrackerOwner();
	FUeAgentContentGraphCapture Capture;
	FUeAgentContentGraphReadError CaptureError;
	StateSemantic::FUeAgentCanonicalSnapshotPtr Snapshot;
	FAuthoringModelPtr Model;
	FWorkspaceError WorkspaceError;
	if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(*Asset, Capture, CaptureError)
		|| !TryBuildContentGraphAuthoringSnapshot(Capture, Resources, Snapshot, CaptureError)
		|| !Snapshot.IsValid())
	{ AddError(CaptureError.Code + TEXT(":") + CaptureError.Detail); return false; }
	if (!TryComposeSnapshotAuthoringModel(FSnapshotCompositionRequest(TEXT("ue.texture_graph.asset"),
		TEXT("llm_authoring"), Snapshot.ToSharedRef()), *Resources, Model, WorkspaceError))
	{ AddError(WorkspaceError.GetCode()); return false; }
	Composition::HjsonImport::FPreparedAuthoringImportPtr Prepared;
	Composition::HjsonImport::FSemanticWorkspaceImportError PrepareError;
	const FString RequestId = TEXT("t85-texture-graph-late-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const Contract::FUeAgentCallerContext Caller(TEXT("t85-texture-graph"),
		{TEXT("uai:query"), TEXT("uai:operate"), TEXT("authoring.write"), TEXT("authoring.structure")}, RequestId);
	if (!Composition::HjsonImport::TryPrepareSemanticWorkspaceImport(
		FWorkspaceSource::Directory(Workspace), ESemanticWorkspaceMode::State, Model.ToSharedRef(), Snapshot.ToSharedRef(),
		Contract::FUeAgentDescriptorIdentity(TEXT("texture_graph.semantic_workspace"), 1, Model->GetSemanticFingerprint()),
		RequestId, RequestId, Caller, false, {}, {}, *Resources, Prepared, PrepareError)
		|| !Prepared.IsValid() || !Prepared->GetPreflightResult().GetPlan().IsValid())
	{ AddError(PrepareError.Code + TEXT(":") + PrepareError.Detail); return false; }
	Composition::GraphCompound::FTextureGraphSemanticCompositeRequest Composite;
	if (!DomainAdapters::TextureGraph::TryBuildSemanticComposite(Prepared->GetWorkspace()->GetModel(),
		Capture, *Asset, Path, Composite, WorkspaceError))
	{ AddError(WorkspaceError.GetCode()); return false; }
	{
		UTextureGraph* Copy = DuplicateObject<UTextureGraph>(Asset, GetTransientPackage());
		ON_SCOPE_EXIT
		{
			if (Copy) { Copy->ClearFlags(RF_Public | RF_Standalone); Copy->MarkAsGarbage(); }
		};
		FNativeGraphValues Copied;
		const bool bCopyEqual = CaptureNativeValues(Copy, Copied) && Copied == Before;
		TestTrue(TEXT("native scratch duplicate preserves complete nodes parameters edges"), bCopyEqual);
		TSet<FString> Deletes;
		bool bBindingsValid = true;
		for (const auto& Link : Composite.Links)
		{
			const bool bDelete = Link.GetIntent() == DomainAdapters::AudioMediaGraph::EAudioMediaGraphMutationIntent::Delete;
			auto Observe = [&](UTextureGraph* Graph, FString& FromAlias, FString& ToAlias, int32& FromCount, int32& ToCount)
			{
				FromCount = ToCount = 0;
				UTG_Node* From = nullptr;
				UTG_Node* To = nullptr;
				if (!Graph || !Graph->Graph()) return false;
				Graph->Graph()->ForEachNodes([&](const UTG_Node* Node, const uint32)
				{
					if (!Node || !Node->GetExpression()) return;
					auto Matches = [&](const FString& Id)
					{
						const int32* Index = Composite.NodeBindings.NodeIndices.Find(Id);
						return Index && Node->GetId().NodeIdx() == *Index;
					};
					if (Matches(Link.GetFrom().GetElementId()))
					{ ++FromCount; From = const_cast<UTG_Node*>(Node); FromAlias = Node->GetExpression()->GetTitleName().ToString(); }
					if (Matches(Link.GetTo().GetElementId()))
					{ ++ToCount; To = const_cast<UTG_Node*>(Node); ToAlias = Node->GetExpression()->GetTitleName().ToString(); }
				});
				FTG_Name FromName(*Link.GetFrom().GetPinName()), ToName(*Link.GetTo().GetPinName());
				const UTG_Pin* FromPin = From ? From->GetOutputPin(FromName) : nullptr;
				const UTG_Pin* ToPin = To ? To->GetInputPin(ToName) : nullptr;
				return FromPin && ToPin && FromPin->GetEdges().Contains(ToPin->GetId());
			};
			FString FromAlias, ToAlias, CopyFromAlias, CopyToAlias;
			int32 FromCount, ToCount, CopyFromCount, CopyToCount;
			const bool bExists = Observe(Asset, FromAlias, ToAlias, FromCount, ToCount);
			const bool bCopyExists = Observe(Copy, CopyFromAlias, CopyToAlias, CopyFromCount, CopyToCount);
			const FString Pair = Link.GetFrom().GetElementId() + TEXT(".") + Link.GetFrom().GetPinName()
				+ TEXT(">") + Link.GetTo().GetElementId() + TEXT(".") + Link.GetTo().GetPinName();
			const bool bDuplicateDelete = bDelete && Deletes.Contains(Pair);
			if (bDelete) Deletes.Add(Pair);
			AddInfo(FString::Printf(TEXT("TextureGraph link %s %s.%s>%s.%s matches=%d/%d copy_matches=%d/%d exists=%d copy_exists=%d duplicate_delete=%d"),
				bDelete ? TEXT("delete") : TEXT("create"), *FromAlias, *Link.GetFrom().GetPinName(), *ToAlias, *Link.GetTo().GetPinName(),
				FromCount, ToCount, CopyFromCount, CopyToCount, bExists, bCopyExists, bDuplicateDelete));
			bBindingsValid &= FromCount == 1 && ToCount == 1 && CopyFromCount == 1 && CopyToCount == 1
				&& !bDuplicateDelete && (!bDelete || (bExists && bCopyExists));
		}
		if (!TestTrue(TEXT("composite endpoints uniquely bind and each delete exists once"), bBindingsValid) || !bCopyEqual) return false;
	}

	bool bLateObserved = false;
	UeAgentAuthoringOperationApply::FBackendOptions Options;
	Options.FinalReadback = [&](UeAgentAuthoringOperationApply::FOperationFailure& Failure)
	{
		UTextureGraph* Current = LoadObject<UTextureGraph>(nullptr, *Path);
		FNativeGraphValues Actual;
		TArray<uint8> Published;
		auto ExpectedNodes = Before.Nodes;
		ExpectedNodes.Remove(RemovedId);
		ExpectedNodes.Add(FirstId, UTG_Expression_Scalar::StaticClass()->GetPathName()
			+ TEXT("|") + AuthoredParameterAlias);
		const FString ExpectedEdge = FString::Printf(TEXT("%d.ValueOut>%d.Source"), FirstId, OutputId);
		bLateObserved = Current && !Current->GetPackage()->IsDirty() && CaptureNativeValues(Current, Actual)
			&& Actual.Nodes.OrderIndependentCompareEqual(ExpectedNodes)
			&& Actual.Parameters.Num() == 2
			&& Actual.Parameters.FindRef(AuthoredParameterAlias) == AuthoredParameterValue
			&& Actual.Parameters.FindRef(TEXT("NeighbourGain")) == 0.875f
			&& Actual.Edges == TArray<FString>{ExpectedEdge}
			&& FFileHelper::LoadFileToArray(Published, *Filename) && Published != DiskBefore;
		Failure.Diagnostic.Code = bLateObserved ? TEXT("test_texture_graph_late_rejection") : TEXT("test_texture_graph_late_observation_failed");
		Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
		return false;
	};
	UeAgentAuthoringOperationApply::FApplyWorkspaceResult Result;
	Asset = nullptr; Native = nullptr; First = nullptr; Neighbour = nullptr; Removed = nullptr; Output = nullptr; Package = nullptr;
	const bool bApplied = OperationOwners::TextureGraph::TryApplySemanticTextureGraphComposite(
		Composite, Prepared->GetPreflightResult().GetPlan().ToSharedRef(), MakeShared<FNeverCancelled>(), Result, Error, Options);
	AddInfo(FString::Printf(TEXT("TextureGraph late applied=%d observed=%d phase=%s cause=%s rollback=%d/%d"),
		bApplied, bLateObserved, *Result.Execution.Phase, *Result.Execution.Diagnostic.CauseCode,
		Result.Execution.bRollbackAttempted, Result.Execution.bRollbackSucceeded));
	if (!TestFalse(TEXT("late readback rejection fails apply"), bApplied)
		|| !TestTrue(TEXT("clean published desired native graph observed before rejection"), bLateObserved)
		|| !TestEqual(TEXT("late rejection phase"), Result.Execution.Phase, FString(TEXT("final_readback")))
		|| !TestEqual(TEXT("late rejection transaction cause"), Result.Execution.Diagnostic.CauseCode, FString(TEXT("verification_failed")))
		|| !TestEqual(TEXT("late rejection diagnostic retained"), Result.Execution.Diagnostic.Code, FString(TEXT("test_texture_graph_late_rejection"))))
	{ AddError(Error); return false; }
	TestTrue(TEXT("public coordinator confirms restoration"),
		Result.Execution.bRollbackAttempted && Result.Execution.bRollbackSucceeded);
	UTextureGraph* Restored = LoadObject<UTextureGraph>(nullptr, *Path);
	FNativeGraphValues RestoredValues;
	TArray<uint8> RestoredDisk;
	TestTrue(TEXT("all dirty native nodes parameters and edges restore"),
		CaptureNativeValues(Restored, RestoredValues) && RestoredValues == Before);
	TestTrue(TEXT("original dirty status restores"), Restored && Restored->GetPackage()->IsDirty());
	TestTrue(TEXT("independent original disk bytes restore"),
		FFileHelper::LoadFileToArray(RestoredDisk, *Filename) && RestoredDisk == DiskBefore);
	return !HasAnyErrors();
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208TextureGraphCreationTest,
	"UeAgentInterface.T208.TextureGraph.CreationLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208TextureGraphCreationTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T208TextureGraphCreate_") + Token + TEXT("/TG_Owner");
	const FString Path = PackageName + TEXT(".TG_Owner");
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-texturegraph-create/native") / Token);
	ON_SCOPE_EXIT { if (auto* Asset = FindObject<UTextureGraph>(nullptr, *Path)) { Asset->GetPackage()->SetDirtyFlag(false); ObjectTools::DeleteObjects({Asset}, false); } };
	auto Absent = [&]() {
		return !FindObject<UTextureGraph>(nullptr, *Path) && !IFileManager::Get().FileExists(*Filename)
			&& !FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetByObjectPath(FSoftObjectPath(Path)).IsValid();
	};
	auto Observe = [&](float Value) {
		auto* Asset = FindObject<UTextureGraph>(nullptr, *Path);
		FNativeGraphValues Values;
		const bool Captured = CaptureNativeValues(Asset, Values);
		const bool Ok = Asset && !Asset->GetPackage()->IsDirty() && Captured
			&& Values.Nodes.Num() == 2 && Values.Parameters.FindRef(TEXT("Gain")) == Value && IFileManager::Get().FileExists(*Filename);
		AddInfo(FString::Printf(TEXT("TextureGraph create observe: loaded=%d clean=%d native=%d nodes=%d gain=%f disk=%d"),
			Asset != nullptr, Asset && !Asset->GetPackage()->IsDirty(), Captured, Values.Nodes.Num(), Values.Parameters.FindRef(TEXT("Gain")), IFileManager::Get().FileExists(*Filename)));
		return Ok;
	};
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch; FString Error;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Candidate = MakeShared<FJsonObject>(); Candidate->SetStringField(TEXT("name"), TEXT("Texture Graph"));
	auto Expression = MakeShared<FJsonObject>(); Expression->SetStringField(TEXT("name"), TEXT("Scalar"));
	auto Scalar = MakeShared<FJsonObject>(); Scalar->SetNumberField(TEXT("value"), 0.25); Scalar->SetStringField(TEXT("alias"), TEXT("Gain"));
	auto Inputs = MakeShared<FJsonObject>(); Inputs->SetObjectField(TEXT("Scalar"), Scalar);
	auto Node = MakeShared<FJsonObject>(); Node->SetObjectField(TEXT("candidate"), Expression); Node->SetObjectField(TEXT("inputs"), Inputs);
	auto Nodes = MakeShared<FJsonObject>(); Nodes->SetObjectField(TEXT("Gain"), Node);
	auto Graph = MakeShared<FJsonObject>(); Graph->SetObjectField(TEXT("nodes"), Nodes); Graph->SetArrayField(TEXT("connections"), {});
	auto Root = MakeShared<FJsonObject>(); Root->SetObjectField(TEXT("candidate"), Candidate); Root->SetObjectField(TEXT("graph"), Graph);
	auto Document = MakeShared<FJsonObject>(); Document->SetStringField(TEXT("schema"), HjsonWorkspace::GetSemanticWorkspaceSchemaId()); Document->SetObjectField(TEXT("root"), Root);
	auto Import = [&]() {
		FString Text, ParamsText; CommandContract::Json::TryCanonicalize(Document, Text, Error);
		auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("inline")); Source->SetStringField(TEXT("document"), Text);
		auto Params = MakeShared<FJsonObject>(); Params->SetStringField(TEXT("target"), Path); Params->SetObjectField(TEXT("source"), Source);
		CommandContract::Json::TryCanonicalize(Params, ParamsText, Error);
		return SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), FGuid::NewGuid().ToString(EGuidFormats::Digits), ParamsText);
	};
	int32 Writes = 0; TWeakObjectPtr<UTextureGraph> Created;
	const auto Handle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* Object) {
		if (Object && Object->GetOutermost()->GetName() == PackageName) { ++Writes; if (auto* Asset = Cast<UTextureGraph>(Object)) Created = Asset; }
	});
	ON_SCOPE_EXIT { FCoreUObjectDelegates::OnObjectModified.Remove(Handle); };
	for (int32 Probe = 0; Probe != 5; ++Probe)
	{
		if (Probe == 0) Candidate->SetStringField(TEXT("name"), TEXT("Definitely Not A Factory"));
		if (Probe == 1) Expression->SetStringField(TEXT("name"), TEXT("Definitely Not An Expression"));
		if (Probe == 2) Root->SetBoolField(TEXT("unknown_field"), true);
		if (Probe == 3) Node->SetBoolField(TEXT("unknown_field"), true);
		if (Probe == 4) Scalar->SetBoolField(TEXT("unknown_field"), true);
		const auto Rejected = Import();
		const bool ZeroWrite = !Rejected.IsOk() && Writes == 0 && Absent();
		AddInfo(FString::Printf(TEXT("TextureGraph create reject probe=%d zero_write_absent=%d error=%s"), Probe, ZeroWrite, *Rejected.GetErrorJson()));
		if (!TestTrue(TEXT("invalid candidate or field rejects before target creation"), ZeroWrite)) return false;
		Candidate->SetStringField(TEXT("name"), TEXT("Texture Graph")); Expression->SetStringField(TEXT("name"), TEXT("Scalar")); Root->RemoveField(TEXT("unknown_field"));
		Node->RemoveField(TEXT("unknown_field")); Scalar->RemoveField(TEXT("unknown_field"));
	}
	DomainAdapters::AssetDataSimpleEffect::FUeAgentAssetDataSimpleEffectFactoryBinding Binding;
	ExecutionMutation::FExecutionMutationError FactoryError;
	if (!Composition::SimpleAuthoring::TryResolveGenericAssetFactory(TEXT("Texture Graph"), {}, Binding, FactoryError)) { AddError(FactoryError.GetCode() + FactoryError.GetDetail()); return false; }
	bool Witness = false;
	UeAgentAuthoringOperationApply::FBackendOptions Options;
	Options.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		Witness = Observe(0.25f) && !Created.IsValid();
		AddInfo(FString::Printf(TEXT("TextureGraph create late witness: replaced_saved_content=%d original_released=%d"), Witness, !Created.IsValid()));
		Failure.Diagnostic.Code = TEXT("test_texture_graph_create_late_rejection"); return false;
	};
	UeAgentAuthoringOperationApply::FApplyWorkspaceResult LateResult;
	const bool Applied = DomainAdapters::TextureGraph::ApplyInitialGraph(Path, Graph, Binding, Token, Options, LateResult, Error);
	const bool Restored = !Applied && Witness && LateResult.Execution.bRollbackAttempted && LateResult.Execution.bRollbackSucceeded && Absent();
	AddInfo(FString::Printf(TEXT("TextureGraph create late recovery: object_registry_disk_absent=%d detail=%s"), Restored, *Error));
	if (!TestTrue(TEXT("late persisted creation restores absence"), Restored)) return false;
	Created.Reset(); const auto Success = Import();
	if (!Success.IsOk()) { AddError(Success.GetErrorJson()); return false; }
	bool Verified = Observe(0.25f) && !Created.IsValid();
	FString Params; TryBuildExportParams(Path, Workspace / TEXT("readback"), Params, Error);
	const auto Export = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("tg-create-export"), Params);
	FString File; TSharedPtr<FJsonObject> Readback, ReadGraph;
	Verified &= Export.IsOk() && TryLoadTextureGraphDocument(Workspace / TEXT("readback"), File, Readback, ReadGraph, Error);
	AddInfo(FString::Printf(TEXT("TextureGraph create public: replaced_saved_scalar_independent_export=%d"), Verified));
	if (!TestTrue(TEXT("public initial root content persists and exports"), Verified)) { AddError(Error); return false; }
	for (const auto& Pair : ReadGraph->GetObjectField(TEXT("nodes"))->Values)
	{
		auto ReadNode = Pair.Value->AsObject();
		const TSharedPtr<FJsonObject>* ReadInputs = nullptr; const TSharedPtr<FJsonObject>* ScalarInput = nullptr;
		if (ReadNode->TryGetObjectField(TEXT("inputs"), ReadInputs) && (*ReadInputs)->TryGetObjectField(TEXT("Scalar"), ScalarInput))
			(*ScalarInput)->SetNumberField(TEXT("value"), 0.625);
	}
	FString Text; CommandContract::Json::TryCanonicalize(Readback, Text, Error);
	if (!FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	TryBuildImportParams(Path, Workspace / TEXT("readback"), Params, Error);
	const auto Edited = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), TEXT("tg-create-followup"), Params);
	const bool EditedValue = Edited.IsOk() && Observe(0.625f);
	AddInfo(FString::Printf(TEXT("TextureGraph create subsequent public edit: saved_value=%d"), EditedValue));
	if (!EditedValue) AddError(Edited.GetErrorJson());
	return TestTrue(TEXT("created root supports existing public edit"), EditedValue);
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208TextureGraphSettingsTest,
 "UeAgentInterface.T208.TextureGraph.SettingsLifecycle",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208TextureGraphSettingsTest::RunTest(const FString&)
{
 using namespace UE::UAI; using namespace HjsonWorkspace; using namespace DomainAdapters::ContentGraphRead;
 const FString Name = TEXT("TG_Settings_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
 const FString PackageName = TEXT("/Game/UAI/Transient/T208TextureGraphSettings/") + Name;
 const FString Path = PackageName + TEXT(".") + Name;
 const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
 const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-texturegraph-settings/native") / Name);
 ON_SCOPE_EXIT { if (UObject* Existing = LoadObject<UObject>(nullptr, *Path)) ObjectTools::DeleteObjects({Existing}, false); };
 UPackage* Package = CreatePackage(*PackageName);
 UTextureGraph* Asset = NewObject<UTextureGraph>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
 Asset->Construct(Name); FAssetRegistryModule::AssetCreated(Asset);
 UTG_Node* Choice = Asset->Graph()->CreateExpressionNode(UTG_Expression_IfThenElse::StaticClass());
 UTG_Node* Scalar = Asset->Graph()->CreateExpressionNode(UTG_Expression_Scalar::StaticClass());
 if (!Choice || !Scalar) return false;
 Choice->GetExpression()->SetTitleName(TEXT("Choice")); Scalar->GetExpression()->SetTitleName(TEXT("Neighbour"));
 FTG_Name From = TEXT("ValueOut"), To = TEXT("LHS");
 if (!TestTrue(TEXT("fixture connects native scalar to comparison input"), Asset->Graph()->Connect(*Scalar, From, *Choice, To))) return false;
 const int32 ChoiceIndex = Choice->GetId().NodeIdx();
 auto* Expression = CastChecked<UTG_Expression_IfThenElse>(Choice->GetExpression());
 if (!SaveGraph(*Package, *Asset, Filename)) return false;
 TArray<uint8> DiskBefore; FFileHelper::LoadFileToArray(DiskBefore, *Filename);
 Expression->Operator = EIfThenElseOperator::GTE;
 FPropertyChangedEvent Changed(FindFProperty<FProperty>(Expression->GetClass(), TEXT("Operator")));
 static_cast<UObject*>(Expression)->PostEditChangeProperty(Changed); Asset->MarkPackageDirty();
 FNativeGraphValues Before; if (!CaptureNativeValues(Asset, Before)) return false;
 auto Pins = [](UTextureGraph* Current) {
  TArray<FString> Facts;
  if (Current && Current->Graph()) Current->Graph()->ForEachPins([&](const UTG_Pin* Pin, uint32, uint32) {
   if (Pin) Facts.Add(FString::Printf(TEXT("%d:%d:%s:%s:%s:%d"), Pin->GetId().NodeIdx(), Pin->GetId().PinIdx(),
    *Pin->GetArgumentName().ToString(), *Pin->GetArgumentCPPTypeName().ToString(), *Pin->GetAliasName().ToString(), Pin->IsInput()));
  }); Facts.Sort(); return Facts;
 };
 const auto BeforePins = Pins(Asset);

 TWeakObjectPtr<UTextureGraph> Original = Asset;
 TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
 TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch; FString Error, Params;
 if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
 auto Export = [&](const FString& Directory) {
  if (!TryBuildExportParams(Path, Directory, Params, Error)) return false;
  auto Result = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("tg-settings-export"), Params);
  if (!Result.IsOk()) Error = Result.GetErrorJson(); return Result.IsOk();
 };
 auto Import = [&]() {
  TryBuildImportParams(Path, Workspace, Params, Error);
  return SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), TEXT("tg-settings-import"), Params);
 };
 auto Property = [&](const FString& Directory, const FString* NewValue, FString& Value) {
  TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.hjson"), true, false);
  for (const FString& File : Files) {
   if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
   FString Text; TSharedPtr<FJsonObject> Document;
   if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) continue;
   const TSharedPtr<FJsonObject>* Categories = nullptr; if (!Document->TryGetObjectField(TEXT("categories"), Categories)) continue;
   TSharedPtr<FJsonObject> Found;
   TFunction<void(const TSharedPtr<FJsonObject>&)> Visit = [&](const TSharedPtr<FJsonObject>& Object) {
    for (const auto& Field : Object->Values) {
     if (Field.Value->Type != EJson::Object) continue;
     auto Row = Field.Value->AsObject();
     if (Field.Key == TEXT("Operator") && Row->HasField(TEXT("type")) && Row->HasField(TEXT("value"))) Found = Row;
     else Visit(Row);
    }
   }; Visit(*Categories);
   if (!Found.IsValid()) continue;
   if (!Found->TryGetStringField(TEXT("value"), Value)) { Error = TEXT("setting_value_not_readable_enum"); return false; }
   if (NewValue) { Found->SetStringField(TEXT("value"), *NewValue); if (!CommandContract::Json::TryCanonicalize(Document, Text, Error)
     || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false; }
   return true;
  }
  Error = TEXT("public_setting_category_not_found"); return false;
 };
 if (!Export(Workspace)) { AddError(Error); return false; }
 auto EnumDefinition = [&](const FString& Directory) {
  TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("definitions.hjson"), true, false);
  for (const FString& File : Files) {
   FString Text; TSharedPtr<FJsonObject> Document;
   if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) continue;
   const TSharedPtr<FJsonObject>* Enums = nullptr;
   if (!Document->TryGetObjectField(TEXT("enums"), Enums) || !Enums || (*Enums)->Values.IsEmpty()) continue;
   for (const auto& Pair : (*Enums)->Values) {
    FString Canonical; const auto Wrapped = MakeShared<FJsonObject>(); Wrapped->SetField(TEXT("enum"), Pair.Value);
    if (CommandContract::Json::TryCanonicalize(Wrapped, Canonical, Error)
     && Canonical.Contains(TEXT("\"GT\"")) && Canonical.Contains(TEXT("\"LT\"")) && Canonical.Contains(TEXT("\"NEQ\""))) return true;
   }
  }
  return false;
 };
 if (!TestTrue(TEXT("public definitions contain shared enum legal names"), EnumDefinition(Workspace))) return false;

 FString Visible, Invalid = TEXT("DefinitelyInvalidOperator"), Desired = TEXT("LT");
 if (!Property(Workspace, &Invalid, Visible)) { AddError(Error); return false; }
 int32 Writes = 0; const auto Watch = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* Object) {
  if (Object && Object->GetPackage()->GetName() == PackageName) ++Writes;
 }); ON_SCOPE_EXIT { FCoreUObjectDelegates::OnObjectModified.Remove(Watch); };
 AddExpectedError(TEXT("UAI workspace operation: unknown_enum_value"), EAutomationExpectedErrorFlags::Contains, 1);
 auto Rejected = Import(); TArray<uint8> AfterInvalid; FFileHelper::LoadFileToArray(AfterInvalid, *Filename);
 const bool ZeroWrite = !Rejected.IsOk() && Writes == 0 && AfterInvalid == DiskBefore && Expression->Operator == EIfThenElseOperator::GTE;
 AddInfo(FString::Printf(TEXT("TG setting invalid zero_write=%d error=%s"), ZeroWrite, *Rejected.GetErrorJson()));
 if (!TestTrue(TEXT("invalid enum rejected before source write"), ZeroWrite)) return false;
 if (!Property(Workspace, &Desired, Visible)) { AddError(Error); return false; }
 auto Resources = MakeSemanticWorkspaceResourceTrackerOwner(); FUeAgentContentGraphCapture Capture; FUeAgentContentGraphReadError CaptureError;
 StateSemantic::FUeAgentCanonicalSnapshotPtr Snapshot; FAuthoringModelPtr Model; FWorkspaceError WorkspaceError;
 if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(*Asset, Capture, CaptureError)
  || !TryBuildContentGraphAuthoringSnapshot(Capture, Resources, Snapshot, CaptureError)
  || !TryComposeSnapshotAuthoringModel(FSnapshotCompositionRequest(TEXT("ue.texture_graph.asset"), TEXT("llm_authoring"), Snapshot.ToSharedRef()), *Resources, Model, WorkspaceError))
 { AddError(CaptureError.Code + WorkspaceError.GetCode()); return false; }
 Composition::HjsonImport::FPreparedAuthoringImportPtr Prepared; Composition::HjsonImport::FSemanticWorkspaceImportError PrepareError;
 const FString RequestId = TEXT("tg-settings-late");
 const Contract::FUeAgentCallerContext Caller(TEXT("tg-settings"), {TEXT("uai:query"), TEXT("uai:operate"), TEXT("authoring.write"), TEXT("authoring.structure")}, RequestId);
 if (!Composition::HjsonImport::TryPrepareSemanticWorkspaceImport(FWorkspaceSource::Directory(Workspace), ESemanticWorkspaceMode::State,
  Model.ToSharedRef(), Snapshot.ToSharedRef(), Contract::FUeAgentDescriptorIdentity(TEXT("texture_graph.semantic_workspace"), 1, Model->GetSemanticFingerprint()),
  RequestId, RequestId, Caller, false, {}, {}, *Resources, Prepared, PrepareError)) { AddError(PrepareError.Code + PrepareError.Detail); return false; }
 Composition::HjsonImport::FSemanticWorkspaceChangeProjection Projection; auto Resolved = Prepared->GetWorkspace();
 if (!Composition::HjsonImport::TryPrepareSemanticWorkspaceChanges(Resolved, Model.ToSharedRef(), Snapshot.ToSharedRef(),
  Contract::FUeAgentDescriptorIdentity(TEXT("texture_graph.semantic_workspace"), 1, Model->GetSemanticFingerprint()), RequestId, RequestId, Caller,
  Path, *Resources, Projection, Prepared, PrepareError)) { AddError(PrepareError.Code + PrepareError.Detail); return false; }
 Composition::GraphCompound::FTextureGraphSemanticCompositeRequest Composite;
 if (!DomainAdapters::TextureGraph::TryBuildSemanticComposite(Prepared->GetWorkspace()->GetModel(), Capture, *Asset, Path, Composite, WorkspaceError)
  || !DomainAdapters::TextureGraph::TryBindSettingChanges(*Prepared, Projection, Composite, Error)) { AddError(WorkspaceError.GetCode() + Error); return false; }
 auto Observe = [&](EIfThenElseOperator Expected, bool Dirty) {
  auto* Current = LoadObject<UTextureGraph>(nullptr, *Path); UTG_Expression_IfThenElse* CurrentExpression = nullptr;
  if (Current && Current->Graph()) Current->Graph()->ForEachNodes([&](const UTG_Node* Node, uint32) {
   if (Node && Node->GetId().NodeIdx() == ChoiceIndex) CurrentExpression = Cast<UTG_Expression_IfThenElse>(Node->GetExpression()); });
  FNativeGraphValues Actual;
  return Current && Current->GetPackage()->IsDirty() == Dirty && CurrentExpression && CurrentExpression->Operator == Expected
   && CurrentExpression->ComparisonType == EIfThenElseType::IndividualComponent && CaptureNativeValues(Current, Actual) && Actual == Before
   && Pins(Current) == BeforePins
   && CurrentExpression->GetParentNode()->GetPin(TEXT("Operator"))->GetEvaluatedVarValue()
    == (Expected == EIfThenElseOperator::LT ? TEXT("LT") : TEXT("GTE"));
 };
 bool Late = false; UeAgentAuthoringOperationApply::FBackendOptions Options;
 Options.FinalReadback = [&](UeAgentAuthoringOperationApply::FOperationFailure& Failure) {
  TArray<uint8> Published; Late = !Original.IsValid() && Observe(EIfThenElseOperator::LT, false)
   && FFileHelper::LoadFileToArray(Published, *Filename) && Published != DiskBefore;
  AddInfo(FString::Printf(TEXT("TG setting late persisted_replaced=%d"), Late));
  Failure.Diagnostic.Code = Late ? TEXT("test_tg_setting_late_rejection") : TEXT("test_tg_setting_late_observation_failed"); return false;
 };
 Asset = nullptr; Expression = nullptr; Package = nullptr; Choice = nullptr; Scalar = nullptr;
 UeAgentAuthoringOperationApply::FApplyWorkspaceResult ApplyResult;
 bool Applied = OperationOwners::TextureGraph::TryApplySemanticTextureGraphComposite(Composite,
  Prepared->GetPreflightResult().GetPlan().ToSharedRef(), MakeShared<FNeverCancelled>(), ApplyResult, Error, Options);
 TArray<uint8> Restored; FFileHelper::LoadFileToArray(Restored, *Filename);
 bool Recovered = !Applied && Late && ApplyResult.Execution.bRollbackSucceeded && Restored == DiskBefore && Observe(EIfThenElseOperator::GTE, true);
 AddInfo(FString::Printf(TEXT("TG setting late recovered_dirty_disk_graph=%d error=%s"), Recovered, *Error));
 if (!TestTrue(TEXT("late restore returns original dirty expression and disk graph"), Recovered)) return false;
 auto Success = Import(); if (!Success.IsOk()) { AddError(Success.GetErrorJson()); return false; }
 if (!TestTrue(TEXT("published enum and non-target graph preserved"), Observe(EIfThenElseOperator::LT, false))) return false;
 if (!Export(Workspace + TEXT("-readback")) || !Property(Workspace + TEXT("-readback"), nullptr, Visible)) { AddError(Error); return false; }
 TestTrue(TEXT("independent export preserves enum definitions"), EnumDefinition(Workspace + TEXT("-readback")));
 AddInfo(FString::Printf(TEXT("TG setting public independent_export=%s"), *Visible));
 TestEqual(TEXT("independent export returns desired enum name"), Visible, Desired);
 return !HasAnyErrors();
}
