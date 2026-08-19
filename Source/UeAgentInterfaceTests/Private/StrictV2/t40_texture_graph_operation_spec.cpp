#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandJson.h"
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

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

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
			TEXT("texture_graph.graph.link.mutate"),
			TEXT("texture_graph.graph.node.mutate"),
			TEXT("texture_graph.graph.parameter.mutate"),
			TEXT("texture_graph.graph.validate_evaluate_settle"),
			TEXT("texture_graph.runtime.probe"),
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

	FString Sha256Text(const FString& Value)
	{
		FTCHARToUTF8 Utf8(*Value);
		uint8 Digest[SHA256_DIGEST_LENGTH] = {};
		SHA256(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), Digest);
		return TEXT("sha256:")
			+ BytesToHex(Digest, SHA256_DIGEST_LENGTH).ToLower();
	}

	bool WriteText(const FString& Path, const FString& Text)
	{
		return IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true)
			&& FFileHelper::SaveStringToFile(
				Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	FString ImportDocument(const FString& Target)
	{
		return FString::Printf(TEXT(R"hjson({
			"schema":"uai.texture_graph.edit/1",
			"root":{
				"type":"/Script/TextureGraph.TextureGraph",
				"operation":{
					"command_id":"texture_graph.graph.node.mutate",
					"params":{
						"target_ref":"%s",
						"nodes":[{
							"element_id":"hjson_scalar",
							"expression_class_ref":"/Script/TextureGraph.TG_Expression_Scalar",
							"intent":"create"
						}]
					}
				}
			}
		})hjson"), *Target);
	}

	FString CanonicalParams(const TSharedRef<FJsonObject>& Params)
	{
		FString Result;
		FString Error;
		return Json::TryCanonicalize(Params, Result, Error) ? Result : TEXT("{}");
	}

	FString ImportParams(
		const FString& Target,
		const FString& Revision,
		const TSharedRef<FJsonObject>& Source)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetObjectField(TEXT("source"), Source);
		Params->SetStringField(TEXT("expected_revision"), Revision);
		Params->SetStringField(
			TEXT("authorization_id"), TEXT("auth:t40:hjson-product"));
		return CanonicalParams(Params);
	}

	bool WriteDirectoryWorkspace(
		const FString& Root,
		const FString& Target,
		const FString& Revision,
		const FString& Document)
	{
		const FString Entry = TEXT("asset.hjson");
		const TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
		Manifest->SetStringField(
			TEXT("workspace_schema"), TEXT("uai.content_graph.workspace/1"));
		Manifest->SetNumberField(TEXT("format_version"), 1);
		Manifest->SetStringField(TEXT("target_identity"), Target);
		Manifest->SetStringField(TEXT("base_revision"), Revision);
		Manifest->SetStringField(TEXT("entry_file"), Entry);
		const TSharedRef<FJsonObject> File = MakeShared<FJsonObject>();
		File->SetStringField(TEXT("path"), Entry);
		File->SetStringField(TEXT("content_hash"), Sha256Text(Document));
		Manifest->SetArrayField(
			TEXT("files"), {MakeShared<FJsonValueObject>(File)});
		Manifest->SetArrayField(
			TEXT("references"), TArray<TSharedPtr<FJsonValue>>{});
		return WriteText(FPaths::Combine(Root, Entry), Document)
			&& WriteText(
				FPaths::Combine(Root, TEXT(".uai/workspace.hjson")),
				CanonicalParams(Manifest));
	}

	bool CleanupPackage(const FString& PackageName)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName, FPackageName::GetAssetPackageExtension());
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			Package->SetDirtyFlag(false);
			if (IFileManager::Get().FileExists(*Filename))
			{
				IFileManager::Get().Delete(*Filename, false, true);
			}
			FText Error;
			UPackageTools::UnloadPackages({Package}, Error, true);
			CollectGarbage(RF_NoFlags, true);
		}
		if (IFileManager::Get().FileExists(*Filename))
		{
			IFileManager::Get().Delete(*Filename, false, true);
		}
		return !IFileManager::Get().FileExists(*Filename)
			&& !FindPackage(nullptr, *PackageName);
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
	TestEqual(TEXT("T40 owns exactly 7 current commands"),
		ExpectedCommands().Num(), 7);
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
	TestEqual(TEXT("TextureGraph contribution owns seven commands"),
		Declaration.Command.Contributions.Num(), 1);
	TestEqual(TEXT("TextureGraph contribution owns one HJSON provider"),
		Declaration.Hjson.Providers.Num(), 1);
	TestEqual(TEXT("TextureGraph contribution owns one target composition"),
		Declaration.Composition.Contracts.Num(), 1);
	TestTrue(TEXT("TextureGraph composition owns semantic routes"),
		Declaration.Composition.Contracts.Num() == 1
			&& !Declaration.Composition.Contracts[0].SemanticRoutes.IsEmpty());
	const TArray<FString> OperationIds = {
		TEXT("texture_graph.artifact.bake"),
		TEXT("texture_graph.graph.link.mutate"),
		TEXT("texture_graph.graph.node.mutate"),
		TEXT("texture_graph.graph.parameter.mutate"),
		TEXT("texture_graph.graph.validate_evaluate_settle"),
	};
	TestEqual(TEXT("TextureGraph declares every write through OperationService"),
		Declaration.Operation.Operations.Num(), OperationIds.Num());
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
	TestTrue(
		TEXT("the exact TextureGraph root resolves one provider"),
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
	TestTrue(TEXT("the TextureGraph resolution is valid"), Resolution.IsValid());
	if (Resolution.IsValid())
	{
		TestEqual(
			TEXT("the TextureGraph provider identity is unique"),
			Resolution.GetProvider()->GetProviderId(),
			FString(TEXT("texture-graph-domain-provider")));
		TestEqual(
			TEXT("the TextureGraph root contract is provider-owned"),
			Resolution.GetInitializationContractId(),
			FString(TEXT("uai.init.texture_graph.root/1")));
	}

	FTargetCompositionContract Composition;
	TestTrue(
		TEXT("ue.texture_graph.asset has one current target composition"),
		FTargetCompositionRegistry::Get().FindContract(
			TEXT("ue.texture_graph.asset"), Composition));
	TestEqual(
		TEXT("TextureGraph composition owns four semantic surfaces"),
		Composition.Surfaces.Num(), 4);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T40TextureGraphHjsonProductTest,
	"UAI.StrictV2.T40.TEXTURE.GRAPH.OPERATION.HjsonCarrierProductRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T40TextureGraphHjsonProductTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Tests::StrictV2::T40;
	(void)Parameters;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the process public command surface composes"),
		Module::CommandRegistration::Active::
			TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)))
	{
		AddError(Error);
		return false;
	}
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Root = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("Automation/StrictV2/T40/Hjson_") + Suffix);
	const TArray<FString> Kinds = {TEXT("inline"), TEXT("single_file"), TEXT("directory")};
	TArray<FString> Packages;
	for (const FString& Kind : Kinds)
	{
		const FString PackageName = TEXT("/Game/UAI/Tests/T40/Hjson_")
			+ Kind + TEXT("_") + Suffix;
		Packages.Add(PackageName);
		CleanupPackage(PackageName);
		const TSharedRef<FJsonObject> Create = MakeShared<FJsonObject>();
		Create->SetStringField(TEXT("asset_kind"), TEXT("texture_graph"));
		Create->SetStringField(
			TEXT("asset_name"), FPackageName::GetLongPackageAssetName(PackageName));
		Create->SetStringField(
			TEXT("package_path"), FPackageName::GetLongPackagePath(PackageName));
		const FUeAgentCommandResult Created = SubmitPublic(
			Registry, Dispatch, TEXT("audio_media.asset.create"),
			CanonicalParams(Create));
		TestTrue(*FString::Printf(TEXT("%s fixture is created publicly"), *Kind),
			Created.IsOk());
		UTextureGraph* Graph = LoadObject<UTextureGraph>(
			nullptr, *ObjectPath(PackageName));
		if (!TestNotNull(*FString::Printf(TEXT("%s TextureGraph loads"), *Kind), Graph))
		{
			continue;
		}
		DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture Before;
		DomainAdapters::ContentGraphRead::FUeAgentContentGraphReadError ReadError;
		if (!TestTrue(TEXT("pre-import revision is captured"),
			DomainAdapters::ContentGraphRead::
				FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
					*Graph, Before, ReadError)))
		{
			AddError(ReadError.Code);
			continue;
		}
		const FString Document = ImportDocument(PackageName);
		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), Kind);
		if (Kind == TEXT("inline"))
		{
			Source->SetStringField(TEXT("document"), Document);
		}
		else if (Kind == TEXT("single_file"))
		{
			const FString Path = FPaths::Combine(Root, Kind, TEXT("asset.hjson"));
			TestTrue(TEXT("single-file fixture persists"), WriteText(Path, Document));
			Source->SetStringField(TEXT("path"), Path);
		}
		else
		{
			const FString Path = FPaths::Combine(Root, Kind);
			TestTrue(TEXT("directory fixture and manifest persist"),
				WriteDirectoryWorkspace(Path, PackageName, Before.Revision, Document));
			Source->SetStringField(TEXT("path"), Path);
		}
		const FUeAgentCommandResult Imported = SubmitPublic(
			Registry, Dispatch, TEXT("content_graph.import_workspace"),
			ImportParams(PackageName, Before.Revision, Source));
		TestTrue(*FString::Printf(
			TEXT("%s imports through the one public HJSON entry"), *Kind),
			Imported.IsOk());
		if (!Imported.IsOk())
		{
			AddError(Imported.GetErrorJson());
			continue;
		}
		Graph = LoadObject<UTextureGraph>(nullptr, *ObjectPath(PackageName));
		bool bFound = false;
		if (Graph && Graph->Graph())
		{
			Graph->Graph()->ForEachNodes(
				[&bFound](const UTG_Node* Node, const uint32)
				{
					bFound = bFound || (Node && Node->GetExpression()
						&& Node->GetExpression()->GetName() == TEXT("hjson_scalar"));
				});
		}
		TestTrue(*FString::Printf(
			TEXT("%s independently reloads the authored node"), *Kind), bFound);

		const FString ExportRoot = FPaths::Combine(Root, TEXT("export_") + Kind);
		const TSharedRef<FJsonObject> Export = MakeShared<FJsonObject>();
		Export->SetStringField(TEXT("target"), PackageName);
		Export->SetStringField(TEXT("workspace_path"), ExportRoot);
		Export->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const FUeAgentCommandResult Exported = SubmitPublic(
			Registry, Dispatch, TEXT("content_graph.export_workspace"),
			CanonicalParams(Export));
		TestTrue(*FString::Printf(
			TEXT("%s result exports through the one public workspace entry"), *Kind),
			Exported.IsOk());
		if (!Exported.IsOk())
		{
			AddError(TEXT("export error diagnostic: ")
				+ Exported.GetErrorJson());
		}
		TSharedPtr<FJsonObject> ExportPayload;
		FString ExportJsonError;
		FString PublishedWorkspace;
		const bool bExportPayloadValid = Exported.IsOk()
				&& Json::TryParseObject(
					Exported.GetPayloadJson(), ExportPayload, ExportJsonError)
				&& ExportPayload.IsValid()
				&& ExportPayload->TryGetStringField(
					TEXT("workspace_path"), PublishedWorkspace)
				&& !PublishedWorkspace.IsEmpty();
		TestTrue(TEXT("export returns its atomic published workspace"),
			bExportPayloadValid);
		if (!bExportPayloadValid)
		{
			AddError(TEXT("export payload diagnostic: ")
				+ Exported.GetPayloadJson() + TEXT(" parse_error:")
				+ ExportJsonError);
		}
		TestTrue(TEXT("export publishes a machine manifest"),
			!PublishedWorkspace.IsEmpty()
				&& IFileManager::Get().FileExists(*FPaths::Combine(
					PublishedWorkspace, TEXT(".uai/workspace.hjson"))));
	}
	for (const FString& PackageName : Packages)
	{
		TestTrue(TEXT("HJSON fixture cleanup succeeds"), CleanupPackage(PackageName));
	}
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return !HasAnyErrors();
}
