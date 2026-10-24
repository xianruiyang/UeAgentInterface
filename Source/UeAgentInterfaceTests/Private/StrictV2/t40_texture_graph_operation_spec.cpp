#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/Contract/Command/UeAgentCommandLifecyclePolicy.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/TextureGraph/UeAgentTextureGraphArtifactService.h"
#include "OperationOwners/TextureGraph/UeAgentTextureGraphDomainService.h"
#include "OperationOwners/TextureGraph/UeAgentTextureGraphOperationOwner.h"
#include "PackageTools.h"
#include "TextureGraph.h"
#include "Expressions/TG_Expression.h"
#include "TG_Graph.h"
#include "TG_Node.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Expressions/Input/TG_Expression_Scalar.h"
#include "Expressions/Output/TG_Expression_Output.h"
#include "TG_Pin.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "ObjectTools.h"
#include "Misc/ScopeExit.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"

namespace UE::UAI::Tests::StrictV2::T40
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;

	class FNeverCancelled final : public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("texture_graph.artifact.bake"),
			TEXT("texture_graph.artifact.capture"),
			TEXT("texture_graph.graph.validate_evaluate_settle"),
			TEXT("texture_graph.runtime.probe"),
		};
		return Commands;
	}

	const TArray<FString>& DormantCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("texture_graph.graph.link.mutate"),
			TEXT("texture_graph.graph.node.mutate"),
			TEXT("texture_graph.graph.parameter.mutate"),
		};
		return Commands;
	}

	FString ObjectPath(const FString& PackageName)
	{
		return PackageName + TEXT(".")
			+ FPackageName::GetLongPackageAssetName(PackageName);
	}

	FUeAgentCommandResult SubmitPublic(
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const FString& ParamsJson)
	{
		const FUeAgentFrozenCommandContract* Contract = Registry.IsValid()
			? Registry->FindExact(CommandId) : nullptr;
		if (!Contract || !Dispatch.IsValid())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t40-hjson"), TEXT("req-t40-hjson"), CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("t40_public_contract_missing"), CommandId);
		}
		const FString RequestId = TEXT("t40-hjson-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t40-hjson\",")
			TEXT("\"request_id\":\"%s\",\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*RequestId, *CommandId, *Contract->GetContractHash(), *ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		TOptional<FUeAgentCommandRequest> Request;
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		if (!FUeAgentCommandProtocolCodec().DecodeRequest(
				Bytes, Request, Diagnostic) || !Request.IsSet())
		{
			return FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t40-hjson"), RequestId, CommandId,
				Contract->GetContractHash(), Diagnostic.Code, Diagnostic.Message);
		}
		return Dispatch->Submit(
			Request.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("strict-v2-t40-hjson"),
				{TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate"),
				 TEXT("uai:authoring:hjson:import"),
				 TEXT("audio_media_graph.read"),
				 TEXT("audio_media_graph.write")},
				TEXT("corr-t40-hjson")),
			MakeShared<FNeverCancelled>());
	}

	FString CanonicalParams(const TSharedRef<FJsonObject>& Params)
	{
		FString Result;
		FString Error;
		return Json::TryCanonicalize(Params, Result, Error) ? Result : TEXT("{}");
	}

	FString ImportParams(
		const FString& Target,
		const TSharedRef<FJsonObject>& Source)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetObjectField(TEXT("source"), Source);
		return CanonicalParams(Params);
	}


	bool ReadOrdinaryGraph(const FString& Workspace, FString& File,
		TSharedPtr<FJsonObject>& Document, FString& Error)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
		Files.Sort();
		for (const FString& Candidate : Files)
		{
			FString Normalized = Candidate;
			FPaths::NormalizeFilename(Normalized);
			if (Normalized.Contains(TEXT("/.uai/")) || Candidate.EndsWith(TEXT(".interfaces.hjson"))) continue;
			FString Text;
			TSharedPtr<FJsonObject> Parsed;
			if (FFileHelper::LoadFileToString(Text, *Candidate)
				&& FUeAgentStructuredHjsonParser::ParseObject(Text, Candidate, Parsed, Error)
				&& Parsed.IsValid() && Parsed->HasTypedField<EJson::Object>(TEXT("nodes"))
				&& Parsed->HasTypedField<EJson::Array>(TEXT("connections")))
			{ File = Candidate; Document = Parsed; return true; }
		}
		Error = TEXT("texture_graph_ordinary_document_missing");
		return false;
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T40TextureGraphInventoryTest,
	"UAI.StrictV2.T40.TEXTURE.GRAPH.OPERATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T40TextureGraphInventoryTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T40;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the process active composition freezes"),
		TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has one T40 owner"), *CommandId),
				Contract->GetPublicOwner(),
				FString(TEXT("texture-graph-operation-owner")));
			TestEqual(
				*FString::Printf(TEXT("%s uses the stable SDK namespace"), *CommandId),
				Contract->GetPythonNamespace(),
				FString(TEXT("texture_graph")));
			FString ExpectedMethod = CommandId;
			ExpectedMethod.RemoveFromStart(TEXT("texture_graph."));
			ExpectedMethod.ReplaceInline(TEXT("."), TEXT("_"));
			TestEqual(
				*FString::Printf(TEXT("%s uses the stable SDK method"), *CommandId),
				Contract->GetPythonFunction(),
				ExpectedMethod);
			if (CommandId == TEXT("texture_graph.artifact.bake"))
			{
				const auto& Fields = Contract->GetModelResultFields();
				TestEqual(TEXT("bake exposes one compact creation result"),
					Fields.Num(), 1);
				if (Fields.Num() == 1)
				{
					TestEqual(TEXT("bake returns verified created assets"),
						Fields[0].Name, FString(TEXT("asset_refs")));
				}
			}
		}
	}
	for (const FString& CommandId : DormantCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s remains registered"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(
				*FString::Printf(TEXT("%s is centrally dormant"), *CommandId),
				Contract->GetAvailability(),
				CommandContract::EUeAgentCommandAvailability::Dormant);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T40TextureGraphHjsonProviderTest,
	"UAI.StrictV2.T40.TEXTURE.GRAPH.OPERATION.HjsonProviderAndComposition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T40TextureGraphHjsonProviderTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::DomainCapability;
	(void)Parameters;
	TSharedPtr<const FFrozenDomainCapabilitySet> FrozenCapabilities;
	FString CapabilityError;
	TestTrue(
		TEXT("the TextureGraph domain capability freezes"),
		TryFreezeProcessDomainCapabilities(
			FrozenCapabilities,
			CapabilityError));
	if (!FrozenCapabilities.IsValid())
	{
		AddError(CapabilityError);
		return false;
	}
	const TSharedRef<const FDomainCapabilityContribution>* TextureGraph =
		FrozenCapabilities->GetContributions().FindByPredicate(
			[](const TSharedRef<const FDomainCapabilityContribution>& Item)
			{
				return Item->GetDomainId() == TEXT("texture-graph");
			});
	TestNotNull(
		TEXT("TextureGraph owns one typed domain contribution"),
		TextureGraph);
	if (!TextureGraph)
	{
		return false;
	}
	const FDomainCapabilityDeclaration& Declaration =
		(*TextureGraph)->GetDeclaration();
	TestFalse(TEXT("TextureGraph contribution owns command declarations"),
		Declaration.Command.Contributions.IsEmpty());
	TestTrue(TEXT("TextureGraph contribution owns its HJSON provider"),
		Declaration.Hjson.Providers.ContainsByPredicate(
			[](const TSharedRef<IImportInitializationDomainProvider>& Provider)
			{
				return Provider->GetProviderId()
					== TEXT("texture-graph-domain-provider");
			}));
	const TArray<FString> OperationIds = {
		TEXT("texture_graph.artifact.bake"),
		TEXT("texture_graph.graph.link.mutate"),
		TEXT("texture_graph.graph.node.mutate"),
		TEXT("texture_graph.graph.parameter.mutate"),
		TEXT("texture_graph.graph.validate_evaluate_settle"),
	};
	for (const FString& OperationId : OperationIds)
	{
		const TSharedPtr<const IDomainOperationInvocationFactory> Factory =
			FrozenCapabilities->FindOperationFactory(OperationId);
		const TSharedPtr<const IDomainOperationInvocationFactory> PublicFactory =
			UE::UAI::OperationOwners::TextureGraph::
				FindTextureGraphOperationInvocationFactory(OperationId);
		TestTrue(*FString::Printf(TEXT("%s owns one request factory"), *OperationId),
			Factory.IsValid());
		TestTrue(*FString::Printf(
			TEXT("%s public handler and contribution share one factory"),
			*OperationId),
			Factory.IsValid() && PublicFactory.IsValid()
				&& Factory.Get() == PublicFactory.Get());
	}
	TestFalse(TEXT("semantic composite is not a named operation factory"),
		UE::UAI::OperationOwners::TextureGraph::
			FindTextureGraphOperationInvocationFactory(
				TEXT("texture_graph.semantic_workspace.composite")).IsValid());
	TestFalse(TEXT("semantic composite is absent from capability operations"),
		Declaration.Operation.Operations.ContainsByPredicate(
			[](const FDomainOperationEntry& Entry)
			{
				return Entry.OperationId
					== TEXT("texture_graph.semantic_workspace.composite");
			}));
	FDomainCapabilityAuthorities Authorities;
	TestTrue(TEXT("TextureGraph contribution activates process authorities"),
		FrozenCapabilities->TryBuildAuthorities(
			Authorities,
			CapabilityError));
	FWorkspaceError Error;
	TSharedPtr<const FImportInitializationDomainProviderRegistry> Registry;
	if (!TestTrue(
			TEXT("the process provider registry freezes with T40"),
			TryFreezeProcessImportInitializationDomainProviders(Registry, Error))
		|| !Registry.IsValid())
	{
		AddError(Error.GetCode());
		return false;
	}
	FImportInitializationDomainResolution Resolution;
	TestFalse(
		TEXT("the retired operation wrapper schema is rejected"),
		Registry->TryResolve(
			FImportInitializationDomainRequest(
				TEXT("content_graph.import_workspace"),
				TEXT("uai.texture_graph.edit/1"),
				TEXT("/Game/UAI/StrictV2/T40/Fixture.Fixture"),
				TEXT("/Script/TextureGraph.TextureGraph"),
				EInitializationNodeRole::RootTarget,
				TEXT("{}")),
			Resolution,
			Error));
	TestFalse(TEXT("old schema leaves no resolution"), Resolution.IsValid());
	FImportInitializationDomainResolution SemanticResolution;
	TestTrue(
		TEXT("the shared semantic workspace schema resolves the TextureGraph provider"),
		Registry->TryResolve(
			FImportInitializationDomainRequest(
				TEXT("content_graph.import_workspace"),
				GetSemanticWorkspaceSchemaId(),
				TEXT("/Game/UAI/StrictV2/T40/Fixture.Fixture"),
				TEXT("/Script/TextureGraph.TextureGraph"),
				EInitializationNodeRole::RootTarget,
				TEXT("{}")),
			SemanticResolution,
			Error));
	TestTrue(TEXT("the semantic TextureGraph resolution is valid"),
		SemanticResolution.IsValid());

	FTargetCompositionContract Composition;
	TestTrue(
		TEXT("ue.texture_graph.asset has one current target composition"),
		FTargetCompositionRegistry::Get().FindContract(
			TEXT("ue.texture_graph.asset"), Composition));
	TestEqual(
		TEXT("TextureGraph composition owns four semantic surfaces"),
		Composition.Surfaces.Num(), 4);
	TestFalse(TEXT("registered TextureGraph composition owns semantic routes"),
		Composition.SemanticRoutes.IsEmpty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T40TextureGraphHjsonProductTest,
	"UAI.StrictV2.T40.TEXTURE.GRAPH.OPERATION.HjsonCarrierProductRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T40TextureGraphHjsonProductTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Tests::StrictV2::T40;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
	{ AddError(Error); return false; }
	const FString PackageName = TEXT("/Game/UAI/Tests/T40/TG_Workspace_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Path = ObjectPath(PackageName);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/StrictV2/T40") / FPackageName::GetLongPackageAssetName(PackageName));
	const FString Reexport = Workspace + TEXT("_readback");
	ON_SCOPE_EXIT
	{
		if (UObject* Owned = LoadObject<UObject>(nullptr, *Path)) ObjectTools::DeleteObjectsUnchecked({Owned});
		TestFalse(TEXT("owned TextureGraph disk removed by UE"), IFileManager::Get().FileExists(*Filename));
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
		IFileManager::Get().DeleteDirectory(*Reexport, false, true);
	};
	UPackage* Package = CreatePackage(*PackageName);
	UTextureGraph* Graph = NewObject<UTextureGraph>(Package, *FPackageName::GetLongPackageAssetName(PackageName), RF_Public | RF_Standalone | RF_Transactional);
	Graph->Construct(FPackageName::GetLongPackageAssetName(PackageName));
	FAssetRegistryModule::AssetCreated(Graph);
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, Graph, *Filename, SaveArgs)) return false;
	Package->SetDirtyFlag(false);
	TArray<uint8> OriginalDisk;
	if (!FFileHelper::LoadFileToArray(OriginalDisk, *Filename)) return false;
	auto Export = [&](const FString& Directory)
	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Path);
		Params->SetStringField(TEXT("workspace_path"), Directory);
		Params->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		auto Result = SubmitPublic(Registry, Dispatch, TEXT("content_graph.export_workspace"), CanonicalParams(Params));
		if (!Result.IsOk()) AddError(Result.GetErrorJson());
		return Result.IsOk();
	};
	if (!Export(Workspace)) return false;
	FString File;
	TSharedPtr<FJsonObject> Document;
	if (!ReadOrdinaryGraph(Workspace, File, Document, Error)) { AddError(Error); return false; }
	auto Nodes = Document->GetObjectField(TEXT("nodes"));
	FString OutputKey;
	for (const auto& Pair : Nodes->Values)
		if (Pair.Value->AsObject()->GetStringField(TEXT("node_type")) == UTG_Expression_Output::StaticClass()->GetName())
			OutputKey = Pair.Key;
	if (!TestFalse(TEXT("public output node has a readable key"), OutputKey.IsEmpty())) return false;
	const FString AddedKey = TEXT("AddedGain");
	auto Added = MakeShared<FJsonObject>();
	Added->SetStringField(TEXT("name"), TEXT("No such TextureGraph expression for T40"));
	auto Value = MakeShared<FJsonObject>();
	Value->SetStringField(TEXT("alias"), AddedKey);
	Value->SetNumberField(TEXT("value"), 0.625);
	auto Inputs = MakeShared<FJsonObject>();
	Inputs->SetObjectField(TEXT("Scalar"), Value);
	Added->SetObjectField(TEXT("inputs"), Inputs);
	Nodes->SetObjectField(AddedKey, Added);
	auto Connection = MakeShared<FJsonObject>();
	Connection->SetStringField(TEXT("from"), AddedKey + TEXT(".ValueOut"));
	Connection->SetStringField(TEXT("to"), OutputKey + TEXT(".Source"));
	Document->SetArrayField(TEXT("connections"), {MakeShared<FJsonValueObject>(Connection)});
	auto Write = [&]()
	{
		FString Text;
		return FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
			&& FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	};
	auto Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("kind"), TEXT("directory"));
	Source->SetStringField(TEXT("path"), Workspace);
	if (!Write()) { AddError(Error); return false; }
	auto Rejected = SubmitPublic(Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportParams(Path, Source));
	TArray<uint8> RejectedDisk;
	int32 BeforeNodes = 0;
	Graph->Graph()->ForEachNodes([&](const UTG_Node* Node, const uint32){ if(Node) ++BeforeNodes; });
	if (!TestFalse(TEXT("unknown readable candidate rejected"), Rejected.IsOk())
		|| !TestTrue(TEXT("unknown candidate cause"), Rejected.GetErrorJson().Contains(TEXT("texture_graph_candidate_unknown")))
		|| !TestTrue(TEXT("unknown name leaves original graph and disk untouched"),
			BeforeNodes == 1 && !Package->IsDirty() && FFileHelper::LoadFileToArray(RejectedDisk, *Filename) && RejectedDisk == OriginalDisk))
	{ AddError(Rejected.GetErrorJson()); return false; }
	const FString ReadableName = GetDefault<UTG_Expression_Scalar>()->GetDefaultName().ToString();
	Added->SetStringField(TEXT("name"), ReadableName);
	if (!Write()) { AddError(Error); return false; }
	Graph = nullptr; Package = nullptr;
	auto Imported = SubmitPublic(Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportParams(Path, Source));
	if (!TestTrue(TEXT("ordinary node plus parameter and connection import"), Imported.IsOk()))
	{ AddError(Imported.GetErrorJson()); return false; }
	// Independently discard the in-process asset and reload the published package.
	if (UPackage* Loaded = FindPackage(nullptr, *PackageName))
	{
		FText UnloadError;
		if (!UPackageTools::UnloadPackages({Loaded}, UnloadError, true))
		{ AddError(UnloadError.ToString()); return false; }
	}
	Graph = LoadObject<UTextureGraph>(nullptr, *Path);
	UTG_Node* ScalarNode = nullptr;
	UTG_Node* OutputNode = nullptr;
	int32 Count = 0;
	if (Graph && Graph->Graph()) Graph->Graph()->ForEachNodes([&](const UTG_Node* Node, const uint32)
	{
		if (!Node) return;
		++Count;
		if (Cast<UTG_Expression_Scalar>(Node->GetExpression())) ScalarNode = const_cast<UTG_Node*>(Node);
		if (Cast<UTG_Expression_Output>(Node->GetExpression())) OutputNode = const_cast<UTG_Node*>(Node);
	});
	FTG_Name ScalarPinName(TEXT("Scalar")), FromPinName(TEXT("ValueOut")), ToPinName(TEXT("Source"));
	const UTG_Pin* ScalarPin = ScalarNode ? ScalarNode->GetInputPin(ScalarPinName) : nullptr;
	const UTG_Pin* From = ScalarNode ? ScalarNode->GetOutputPin(FromPinName) : nullptr;
	const UTG_Pin* To = OutputNode ? OutputNode->GetInputPin(ToPinName) : nullptr;
	float NativeValue = 0;
	TestTrue(TEXT("native reloaded new scalar parameter and bound connection"),
		Graph && !Graph->GetPackage()->IsDirty() && Count == 2 && ScalarPin
		&& ScalarPin->GetAliasName() == FName(*AddedKey) && ScalarPin->GetValue(NativeValue) && NativeValue == 0.625f
		&& From && To && From->GetEdges().Contains(To->GetId()));
	if (!Export(Reexport) || !ReadOrdinaryGraph(Reexport, File, Document, Error))
	{ AddError(Error); return false; }
	Nodes = Document->GetObjectField(TEXT("nodes"));
	FString ReadbackScalarKey, ReadbackOutputKey;
	for (const auto& Pair : Nodes->Values)
	{
		auto Node = Pair.Value->AsObject();
		const FString Type = Node->GetStringField(TEXT("node_type"));
		if (Type == UTG_Expression_Output::StaticClass()->GetName()) ReadbackOutputKey = Pair.Key;
		if (Type != UTG_Expression_Scalar::StaticClass()->GetName()) continue;
		auto Input = Node->GetObjectField(TEXT("inputs"))->GetObjectField(TEXT("Scalar"));
		if (Input->GetStringField(TEXT("alias")) == AddedKey && Input->GetNumberField(TEXT("value")) == 0.625)
			ReadbackScalarKey = Pair.Key;
	}
	bool bEdge = false;
	for (const auto& Item : Document->GetArrayField(TEXT("connections")))
	{
		auto Edge = Item->AsObject();
		bEdge |= Edge->GetStringField(TEXT("from")) == ReadbackScalarKey + TEXT(".ValueOut")
			&& Edge->GetStringField(TEXT("to")) == ReadbackOutputKey + TEXT(".Source");
	}
	TestTrue(TEXT("ordinary reexport preserves new scalar value alias and connection"),
		Nodes->Values.Num() == 2 && !ReadbackScalarKey.IsEmpty() && !ReadbackOutputKey.IsEmpty() && bEdge);
	return !HasAnyErrors();
}
