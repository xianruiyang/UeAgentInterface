#include "Misc/AutomationTest.h"

// GC-011 production-header discovery is compiled into this test source.

#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphTextureGraphNativeHook.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "CommandHandlers/TextureGraph/UeAgentTextureGraphCommandHandlers.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/Contract/UeAgentContract.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Editor.h"
#include "Expressions/Input/TG_Expression_Scalar.h"
#include "Expressions/Output/TG_Expression_Output.h"
#include "Expressions/Procedural/TG_Expression_Gradient.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "OperationOwners/TextureGraph/UeAgentTextureGraphArtifactService.h"
#include "PackageTools.h"
#include "TG_Graph.h"
#include "TG_Node.h"
#include "TextureGraph.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	using namespace UE::UAI;

	class FNeverCancelled final : public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	bool DecodeArtifactRequest(
		const FString& CommandId,
		const FString& ParamsJson,
		TOptional<CommandEdge::FUeAgentCommandRequest>& OutRequest)
	{
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t40-artifact\",")
			TEXT("\"request_id\":\"req-t40-%s\",")
			TEXT("\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":")
			TEXT("\"0000000000000000000000000000000000000000000000000000000000000000\",")
			TEXT("\"params\":%s}"),
			*CommandId.Replace(TEXT("."), TEXT("-")),
			*CommandId,
			*ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		CommandEdge::FUeAgentCommandProtocolDiagnostic Diagnostic;
		return CommandEdge::FUeAgentCommandProtocolCodec().DecodeRequest(
			Bytes, OutRequest, Diagnostic);
	}

	bool CleanupAssetPackage(const FString& PackageName)
	{
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			if (UObject* Asset = FindObject<UObject>(
				Package, *FPackageName::GetLongPackageAssetName(PackageName)))
			{
				FAssetRegistryModule::AssetDeleted(Asset);
			}
			Package->SetDirtyFlag(false);
			FText Error;
			UPackageTools::UnloadPackages({Package}, Error, true);
			CollectGarbage(RF_NoFlags, true);
		}
		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName, FPackageName::GetAssetPackageExtension());
		if (IFileManager::Get().FileExists(*Filename))
		{
			IFileManager::Get().Delete(*Filename, false, true);
		}
		return !IFileManager::Get().FileExists(*Filename)
			&& !FindPackage(nullptr, *PackageName);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAudioMediaGraphTextureGraphNativeHookTest,
	"UeAgentInterface.DomainAdapters.AudioMediaGraph.E2.Native.TextureGraph."
		"OwnedTopologySettleArtifactStateAndIndependentReadback",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAudioMediaGraphTextureGraphNativeHookTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainAdapters::AudioMediaGraph;

	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/UAI/AudioMediaGraph/TextureGraph/%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!TestNotNull(TEXT("owned TextureGraph package exists"), Package))
	{
		return false;
	}
	Package->SetDirtyFlag(false);
	TStrongObjectPtr<UTextureGraph> Graph(NewObject<UTextureGraph>(
		Package, TEXT("TG_GC010"), RF_Transactional));
	if (!TestNotNull(TEXT("owned TextureGraph exists"), Graph.Get()))
	{
		Package->MarkAsGarbage();
		return false;
	}
	Graph->Construct(TEXT("TG_GC010"));

	FAudioMediaGraphTextureGraphNativeHook Hook;
	FAudioMediaGraphTextureGraphScenarioResult GraphResult;
	FAudioMediaGraphError Error;
	TestTrue(TEXT("TextureGraph mutates through native UTG API"),
		Hook.TryMutateEvaluateAndCaptureOwnedGraph(
			*Graph, GraphResult, Error));
	TestTrue(TEXT("TextureGraph apply evidence exists"),
		GraphResult.HasAppliedMutation());
	TestTrue(TEXT("TextureGraph validate/evaluate settles"),
		GraphResult.HasEvaluateBarrier() && GraphResult.HasSettled());
	TestTrue(TEXT("TextureGraph topology is independently captured"),
		GraphResult.HasIndependentCapture());
	TestTrue(TEXT("TextureGraph captures native nodes"),
		GraphResult.GetNodeIdentities().Num() >= 1);
	TestTrue(TEXT("TextureGraph captures native pins"),
		GraphResult.GetPinIdentities().Num() >= 1);
	TestTrue(TEXT("TextureGraph captures parameter/output state"),
		GraphResult.GetParameterNames().Num() >= 1);
	TestFalse(TEXT("TextureGraph node preserves expression UObject path"),
		GraphResult.GetNodeIdentities()[0].GetExpressionObjectPath().IsEmpty());
	TestTrue(TEXT("TextureGraph node preserves FTG node id"),
		GraphResult.GetNodeIdentities()[0].GetNodeIndex() >= 0);

	FAudioMediaGraphTextureGraphArtifactState ArtifactState;
	TestTrue(TEXT("artifact state is captured only after settle"),
		Hook.TryCaptureSettledArtifactState(
			*Graph, GraphResult, ArtifactState, Error));
	TestTrue(TEXT("artifact state is independently captured"),
		ArtifactState.HasIndependentCapture());
	TestTrue(TEXT("artifact state has a native output"),
		ArtifactState.GetOutputs().Num() >= 1);
	TestFalse(TEXT("artifact output identity is explicit"),
		ArtifactState.GetOutputs()[0].GetOutputIdentity().IsEmpty());
	TestFalse(TEXT("artifact output settings are explicit"),
		ArtifactState.GetOutputs()[0].GetSettingsFingerprint().IsEmpty());
	TestFalse(TEXT("artifact value type is explicit"),
		ArtifactState.GetOutputs()[0].GetEvaluatedValueType().IsEmpty());
	TestFalse(TEXT("artifact evidence differs from topology readback"),
		ArtifactState.GetEvidenceId() == GraphResult.GetReadbackEvidenceId());
	TestFalse(TEXT("Adapter does not claim artifact publication"),
		ArtifactState.WasPublished());

	FAudioMediaGraphTextureGraphScenarioResult UnsettledResult;
	FAudioMediaGraphTextureGraphArtifactState RejectedArtifact;
	TestFalse(TEXT("artifact capture before settle fails closed"),
		Hook.TryCaptureSettledArtifactState(
			*Graph, UnsettledResult, RejectedArtifact, Error));
	TestEqual(TEXT("pre-settle artifact error is exact"), Error.GetCode(),
		FString(TEXT("audio_media_graph_artifact_before_settle")));

	Package->SetDirtyFlag(false);
	Package->MarkAsGarbage();
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAudioMediaGraphTextureGraphRealRenderReadbackTest,
	"UeAgentInterface.DomainAdapters.AudioMediaGraph.E2.Native.TextureGraph."
		"RealRenderPixelReadback",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAudioMediaGraphTextureGraphRealRenderReadbackTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainAdapters::AudioMediaGraph;
	(void)Parameters;
	if (!FApp::CanEverRender())
	{
		AddError(TEXT("real TextureGraph render evidence requires a non-NullRHI run"));
		return false;
	}
	UWorld* World = GEditor
		? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!TestNotNull(TEXT("an Editor world is available"), World))
	{
		return false;
	}
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/UAI/AudioMediaGraph/TextureGraphRender/%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	TStrongObjectPtr<UTextureGraph> Graph(NewObject<UTextureGraph>(
		Package, TEXT("TG_RenderEvidence"), RF_Transactional));
	if (!TestNotNull(TEXT("render TextureGraph exists"), Graph.Get()))
	{
		return false;
	}
	Graph->Construct(TEXT("TG_RenderEvidence"));

	FAudioMediaGraphTextureGraphNativeHook Hook;
	FAudioMediaGraphTextureGraphScenarioResult Scenario;
	FAudioMediaGraphError Error;
	if (!TestTrue(
			TEXT("the render fixture uses native mutation and settle"),
			Hook.TryMutateEvaluateAndCaptureOwnedGraph(
				*Graph, Scenario, Error)))
	{
		AddError(Error.GetCode());
		return false;
	}
	UTG_Graph* NativeGraph = Graph->Graph();
	UTG_Node* ScalarNode = nullptr;
	UTG_Node* OutputNode = nullptr;
	NativeGraph->ForEachNodes(
		[&](const UTG_Node* Node, const uint32)
		{
			if (!Node || !Node->GetExpression()) return;
			if (Cast<UTG_Expression_Scalar>(Node->GetExpression()))
			{
				ScalarNode = const_cast<UTG_Node*>(Node);
			}
			if (Cast<UTG_Expression_Output>(Node->GetExpression()))
			{
				OutputNode = const_cast<UTG_Node*>(Node);
			}
		});
	if (!TestNotNull(TEXT("the fixture has its scalar source"), ScalarNode)
		|| !TestNotNull(TEXT("the fixture has its output node"), OutputNode))
	{
		return false;
	}
	FTG_Name ScalarOutput = TEXT("ValueOut");
	FTG_Name OutputInput = TEXT("Source");
	NativeGraph->RemoveEdge(
		*ScalarNode, ScalarOutput, *OutputNode, OutputInput);
	UTG_Node* GradientNode = NativeGraph->CreateExpressionNode(
		UTG_Expression_Gradient::StaticClass());
	FTG_Name GradientOutput = TEXT("Output");
	if (!TestNotNull(TEXT("a procedural texture source is created"), GradientNode)
		|| !TestTrue(
			TEXT("the procedural texture source connects to the output"),
			NativeGraph->Connect(
				*GradientNode, GradientOutput, *OutputNode, OutputInput)))
	{
		return false;
	}
	UTG_Expression_Output* PrimaryOutput =
		CastChecked<UTG_Expression_Output>(OutputNode->GetExpression());
	PrimaryOutput->SetShouldExport(true);
	PrimaryOutput->OutputSettings.FolderPath =
		FName(TEXT("/Game/UAI/Tests/T40"));
	PrimaryOutput->OutputSettings.BaseName = FName(TEXT("PrimaryOutput"));
	UTG_Node* SecondaryGradientNode = NativeGraph->CreateExpressionNode(
		UTG_Expression_Gradient::StaticClass());
	UTG_Node* SecondaryOutputNode = NativeGraph->CreateExpressionNode(
		UTG_Expression_Output::StaticClass());
	UTG_Expression_Output* SecondaryOutput = SecondaryOutputNode
		? Cast<UTG_Expression_Output>(SecondaryOutputNode->GetExpression())
		: nullptr;
	if (!TestNotNull(
			TEXT("a second procedural texture source is created"),
			SecondaryGradientNode)
		|| !TestNotNull(
			TEXT("a second output is created"), SecondaryOutputNode)
		|| !TestNotNull(
			TEXT("the second output expression is native"), SecondaryOutput))
	{
		return false;
	}
	SecondaryOutput->InitializeOutputSettings();
	SecondaryOutput->SetTitleName(TEXT("SecondaryOutput"));
	SecondaryOutput->SetShouldExport(true);
	SecondaryOutput->OutputSettings.FolderPath =
		FName(TEXT("/Game/UAI/Tests/T40"));
	SecondaryOutput->OutputSettings.BaseName = FName(TEXT("SecondaryOutput"));
	if (!TestTrue(
			TEXT("the second procedural source connects to the second output"),
			NativeGraph->Connect(
				*SecondaryGradientNode,
				GradientOutput,
				*SecondaryOutputNode,
				OutputInput)))
	{
		return false;
	}
	if (!TestTrue(
			TEXT("the textured fixture validates and evaluates"),
			Hook.TryValidateEvaluateSettle(*Graph, Error)))
	{
		AddError(Error.GetCode());
		return false;
	}
	TArray<FAudioMediaGraphTextureGraphRenderedOutput> Outputs;
	if (!TestTrue(
			TEXT("RenderTextureGraph returns real render targets and readback"),
			Hook.TryRenderAndReadOutputs(*World, *Graph, Outputs, Error)))
	{
		AddError(Error.GetCode());
		return false;
	}
	TestTrue(TEXT("two independently selectable rendered outputs are returned"),
		Outputs.Num() >= 2);
	for (const FAudioMediaGraphTextureGraphRenderedOutput& Output : Outputs)
	{
		const FAudioMediaGraphTextureGraphPixelStats& Stats = Output.GetStats();
		TestNotNull(TEXT("the output retains its real render target"),
			Output.GetRenderTarget());
		TestTrue(TEXT("the output has positive dimensions"),
			Stats.GetWidth() > 0 && Stats.GetHeight() > 0);
		TestEqual(TEXT("all rendered pixels are read back"),
			Stats.GetSampledPixelCount(),
			Stats.GetWidth() * Stats.GetHeight());
		TestTrue(TEXT("pixel statistics include at least one unique color"),
			Stats.GetUniqueColorEstimate() > 0);
	}

	TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>> PublicHandlers;
	FString HandlerError;
	if (!TestTrue(
			TEXT("the T40 public handlers compose"),
			CommandHandlers::TextureGraph::TryCreateHandlers(
				OperationOwners::TextureGraph::
					GetTextureGraphBakeInvocationFactory(),
				PublicHandlers,
				HandlerError)))
	{
		AddError(HandlerError);
		return false;
	}
	TMap<FString, TSharedRef<CommandEdge::IUeAgentExactCommandHandler>> Handlers;
	for (const TSharedRef<CommandEdge::IUeAgentExactCommandHandler>& Handler
		: PublicHandlers)
	{
		Handlers.Add(Handler->GetCommandId(), Handler);
	}
	DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture Capture;
	DomainAdapters::ContentGraphRead::FUeAgentContentGraphReadError ReadError;
	if (!TestTrue(
			TEXT("the source revision is read from current ContentGraphRead"),
			DomainAdapters::ContentGraphRead::
				FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
					*Graph, Capture, ReadError)))
	{
		AddError(ReadError.Code);
		return false;
	}
	const Contract::FUeAgentCallerContext Caller(
		TEXT("t40-artifact-test"),
		{TEXT("uai:query"), TEXT("uai:operate")},
		TEXT("corr-t40-artifact"));
	const TSharedRef<Contract::IUeAgentCancellationSignal> Cancellation =
		MakeShared<FNeverCancelled>();

	TOptional<CommandEdge::FUeAgentCommandRequest> ProbeRequest;
	const FString ProbeParams = FString::Printf(
		TEXT("{\"target\":\"%s\",\"render_output_index\":0,")
		TEXT("\"expected_stats\":{\"min_width\":1,\"min_height\":1,")
		TEXT("\"min_unique_color_estimate\":1}}"),
		*Graph->GetPathName());
	if (!TestTrue(
			TEXT("the probe request decodes"),
			DecodeArtifactRequest(
				TEXT("texture_graph.runtime.probe"),
				ProbeParams,
				ProbeRequest))
		|| !TestTrue(TEXT("the probe request exists"), ProbeRequest.IsSet()))
	{
		return false;
	}
	const CommandEdge::FUeAgentCommandResult ProbeResult =
		Handlers.FindChecked(TEXT("texture_graph.runtime.probe"))->Submit(
			ProbeRequest.GetValue(), Caller, Cancellation);
	TestTrue(TEXT("runtime probe succeeds through its current handler"),
		ProbeResult.IsOk());
	TSharedPtr<FJsonObject> ProbePayload;
	FString JsonError;
	TestTrue(
		TEXT("runtime probe returns canonical structured evidence"),
		CommandContract::Json::TryParseObject(
			ProbeResult.GetPayloadJson(), ProbePayload, JsonError));
	bool bProbePassed = false;
	TestTrue(
		TEXT("runtime probe returns the expected-stats verdict"),
		ProbePayload.IsValid()
			&& ProbePayload->TryGetBoolField(
				TEXT("probe_passed"), bProbePassed));
	TestTrue(TEXT("the attainable expected-stats contract passes"), bProbePassed);
	TestTrue(
		TEXT("runtime probe returns pixel statistics"),
		ProbePayload.IsValid()
			&& ProbePayload->HasTypedField<EJson::Object>(TEXT("pixel_stats")));

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString BakePackage =
		TEXT("/Game/UAI/Tests/T40/Baked_") + Suffix;
	TOptional<CommandEdge::FUeAgentCommandRequest> BakeRequest;
	const FString BakeParams = FString::Printf(
		TEXT("{\"target\":\"%s\",\"expected_revision\":\"%s\",")
		TEXT("\"outputs\":[{\"render_output_index\":0,")
		TEXT("\"destination\":\"%s\"}]}"),
		*Graph->GetPathName(), *Capture.Revision, *BakePackage);
	if (!TestTrue(
			TEXT("the bake request decodes"),
			DecodeArtifactRequest(
				TEXT("texture_graph.artifact.bake"), BakeParams, BakeRequest))
		|| !TestTrue(TEXT("the bake request exists"), BakeRequest.IsSet()))
	{
		return false;
	}
	const CommandEdge::FUeAgentCommandResult BakeResult =
		Handlers.FindChecked(TEXT("texture_graph.artifact.bake"))->Submit(
			BakeRequest.GetValue(), Caller, Cancellation);
	TestTrue(
		TEXT("bake completes through OperationLifecycle"), BakeResult.IsOk());
	if (!BakeResult.IsOk())
	{
		AddError(TEXT("bake failure payload: ") + BakeResult.GetErrorJson());
	}
	const FString BakeObjectPath = BakePackage + TEXT(".")
		+ FPackageName::GetLongPackageAssetName(BakePackage);
	UTexture2D* ReloadedBake = LoadObject<UTexture2D>(
		nullptr, *BakeObjectPath);
	TestNotNull(
		TEXT("bake persists and independently reloads a Texture2D"),
		ReloadedBake);
	if (ReloadedBake)
	{
		TestTrue(TEXT("the reloaded Texture2D has real dimensions"),
			ReloadedBake->GetSizeX() > 0 && ReloadedBake->GetSizeY() > 0);
	}

	const FString RollbackFirst =
		TEXT("/Game/UAI/Tests/T40/RollbackFirst_") + Suffix;
	const FString RollbackBlocked =
		TEXT("/Game/UAI/Tests/T40/RollbackBlocked_") + Suffix;
	const FString RollbackFirstFilename =
		FPackageName::LongPackageNameToFilename(
			RollbackFirst, FPackageName::GetAssetPackageExtension());
	const FString RollbackBlockedFilename =
		FPackageName::LongPackageNameToFilename(
			RollbackBlocked, FPackageName::GetAssetPackageExtension());
	TestTrue(
		TEXT("the rollback fixture creates an exact persistence collision"),
		IFileManager::Get().MakeDirectory(*RollbackBlockedFilename, true));
	TOptional<CommandEdge::FUeAgentCommandRequest> RollbackRequest;
	const FString RollbackParams = FString::Printf(
		TEXT("{\"target\":\"%s\",\"expected_revision\":\"%s\",")
		TEXT("\"outputs\":[")
		TEXT("{\"render_output_index\":0,\"destination\":\"%s\"},")
		TEXT("{\"render_output_index\":1,\"destination\":\"%s\"}]}"),
		*Graph->GetPathName(),
		*Capture.Revision,
		*RollbackFirst,
		*RollbackBlocked);
	TestTrue(
		TEXT("the batch rollback request decodes"),
		DecodeArtifactRequest(
			TEXT("texture_graph.artifact.bake"),
			RollbackParams,
			RollbackRequest));
	if (RollbackRequest.IsSet())
	{
		const CommandEdge::FUeAgentCommandResult RollbackResult =
			Handlers.FindChecked(TEXT("texture_graph.artifact.bake"))->Submit(
				RollbackRequest.GetValue(), Caller, Cancellation);
		TestFalse(
			TEXT("a batch persistence failure never reports success"),
			RollbackResult.IsOk());
		TestFalse(
			TEXT("the first batch output is durably rolled back"),
			IFileManager::Get().FileExists(*RollbackFirstFilename));
		TestFalse(
			TEXT("the blocked batch output is not persisted"),
			IFileManager::Get().FileExists(*RollbackBlockedFilename));
		TestNull(
			TEXT("the first batch package is unloaded after rollback"),
			FindPackage(nullptr, *RollbackFirst));
		TestNull(
			TEXT("the blocked batch package is unloaded after rollback"),
			FindPackage(nullptr, *RollbackBlocked));
	}
	IFileManager::Get().DeleteDirectory(
		*RollbackBlockedFilename, false, true);
	TestTrue(TEXT("the successful bake fixture is cleaned up"),
		CleanupAssetPackage(BakePackage));
	CleanupAssetPackage(RollbackFirst);
	CleanupAssetPackage(RollbackBlocked);
	Package->SetDirtyFlag(false);
	Package->MarkAsGarbage();
	return !HasAnyErrors();
}
