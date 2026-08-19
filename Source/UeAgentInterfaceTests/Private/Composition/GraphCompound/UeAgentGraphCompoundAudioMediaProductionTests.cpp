#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaBindings.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "HAL/FileManager.h"
#include "MetasoundBuilderBase.h"
#include "MetasoundDocumentBuilderRegistry.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocumentBuilder.h"
#include "MetasoundSource.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNodeMixer.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "TextureGraph.h"
#include "TG_Graph.h"
#include "TG_Node.h"
#include "TG_Pin.h"
#include "Expressions/Input/TG_Expression_Scalar.h"
#include "Expressions/Output/TG_Expression_Output.h"

namespace
{
	using namespace UE::UAI::Composition::GraphCompound;
	using namespace UE::UAI::Contract;

	class FActiveCancellation final : public IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	class FAudioMediaAssetSet final
	{
	public:
		FAudioMediaAssetSet()
			: Suffix(FGuid::NewGuid().ToString(EGuidFormats::Digits))
		{
		}

		FString Add(const FString& Stem)
		{
			const FString PackageName = TEXT("/Game/UAI/Tests/GraphCompound/")
				+ Stem + TEXT("_") + Suffix;
			PackageNames.Add(PackageName);
			return PackageName;
		}

		static FString ObjectPath(const FString& PackageName)
		{
			return PackageName + TEXT(".")
				+ FPackageName::GetLongPackageAssetName(PackageName);
		}

		bool Unload(const FString& PackageName) const
		{
			UPackage* const Package = FindPackage(nullptr, *PackageName);
			if (!Package)
			{
				return true;
			}
			Package->SetDirtyFlag(false);
			FText Error;
			const bool bUnloaded = UPackageTools::UnloadPackages(
				{Package}, Error, true);
			CollectGarbage(RF_NoFlags, true);
			return bUnloaded && !FindPackage(nullptr, *PackageName);
		}

		bool Cleanup()
		{
			bool bClean = true;
			for (const FString& PackageName : PackageNames)
			{
				if (UPackage* const Package = FindPackage(nullptr, *PackageName))
				{
					if (UObject* const Asset = FindObject<UObject>(
						Package,
						*FPackageName::GetLongPackageAssetName(PackageName)))
					{
						FAssetRegistryModule::AssetDeleted(Asset);
					}
				}
				bClean = Unload(PackageName) && bClean;
				const FString Filename =
					FPackageName::LongPackageNameToFilename(
						PackageName, FPackageName::GetAssetPackageExtension());
				if (IFileManager::Get().FileExists(*Filename))
				{
					bClean = IFileManager::Get().Delete(
						*Filename, false, true) && bClean;
				}
				bClean = !IFileManager::Get().FileExists(*Filename)
					&& !FindPackage(nullptr, *PackageName) && bClean;
			}
			return bClean;
		}

		~FAudioMediaAssetSet() { Cleanup(); }

	private:
		FString Suffix;
		TArray<FString> PackageNames;
	};

	FUeAgentCallerContext Caller(const FString& CapabilityId)
	{
		return FUeAgentCallerContext(
			TEXT("test.agent"),
			{TEXT("audio_media_graph.read"), TEXT("audio_media_graph.write")},
			TEXT("correlation:w05:") + CapabilityId);
	}

	bool TryOperationError(
		const FUeAgentOperationResult& Result,
		FString& OutCode,
		FString& OutDetail)
	{
		OutCode.Reset();
		OutDetail.Reset();
		if (!Result.IsValid())
		{
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Result.GetValue().GetJsonValue());
		const TSharedPtr<FJsonObject>* Error = nullptr;
		return FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid()
			&& Root->TryGetObjectField(TEXT("operation_error"), Error)
			&& Error && Error->IsValid()
			&& (*Error)->TryGetStringField(TEXT("code"), OutCode)
			&& ((*Error)->TryGetStringField(TEXT("detail"), OutDetail) || true);
	}

	bool TryQueryError(
		const FUeAgentQueryResult& Result,
		FString& OutCode,
		FString& OutDetail)
	{
		OutCode.Reset();
		OutDetail.Reset();
		if (!Result.IsValid())
		{
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Result.GetValue().GetJsonValue());
		const TSharedPtr<FJsonObject>* Error = nullptr;
		return FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid()
			&& Root->TryGetObjectField(TEXT("query_error"), Error)
			&& Error && Error->IsValid()
			&& (*Error)->TryGetStringField(TEXT("code"), OutCode)
			&& ((*Error)->TryGetStringField(TEXT("detail"), OutDetail) || true);
	}

	bool Submit(
		FAutomationTestBase& Test,
		FGraphCompoundRuntime& Runtime,
		const FString& CapabilityId,
		const FString& Payload,
		const FString& Suffix,
		FString& OutRevision)
	{
		OutRevision.Reset();
		const FAudioMediaGraphRouteProfile* const Profile =
			FindAudioMediaGraphRouteProfile(CapabilityId);
		if (!Test.TestNotNull(TEXT("operation profile exists"), Profile))
		{
			return false;
		}
		const FUeAgentOperationResult Result = Runtime.GetOperationPort()->Submit(
			FUeAgentOperationRequest(
				Profile->GetExecutionContract().GetDescriptorIdentity(),
				TEXT("request:w05:") + Suffix,
				TEXT("idempotency:w05:") + Suffix,
				FUeAgentContractValue(Payload)),
			Caller(CapabilityId), MakeShared<FActiveCancellation>());
		FString ErrorCode;
		FString ErrorDetail;
		if (TryOperationError(Result, ErrorCode, ErrorDetail))
		{
			Test.AddError(CapabilityId + TEXT(": ") + ErrorCode
				+ TEXT(":") + ErrorDetail);
			return false;
		}
		if (!Test.TestTrue(TEXT("operation result envelope is valid"),
			Result.IsValid()))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Result.GetValue().GetJsonValue());
		const bool bExact = Result.IsValid()
			&& FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid()
			&& Root->TryGetStringField(TEXT("revision"), OutRevision)
			&& !OutRevision.IsEmpty();
		return Test.TestTrue(*FString::Printf(TEXT("%s returns real revision"),
			*CapabilityId), bExact);
	}

	bool SubmitExpectError(
		FAutomationTestBase& Test,
		FGraphCompoundRuntime& Runtime,
		const FString& CapabilityId,
		const FString& Payload,
		const FString& Suffix)
	{
		const FAudioMediaGraphRouteProfile* const Profile =
			FindAudioMediaGraphRouteProfile(CapabilityId);
		if (!Test.TestNotNull(TEXT("failure profile exists"), Profile))
		{
			return false;
		}
		const FUeAgentOperationResult Result = Runtime.GetOperationPort()->Submit(
			FUeAgentOperationRequest(
				Profile->GetExecutionContract().GetDescriptorIdentity(),
				TEXT("request:w05:negative:") + Suffix,
				TEXT("idempotency:w05:negative:") + Suffix,
				FUeAgentContractValue(Payload)),
			Caller(CapabilityId), MakeShared<FActiveCancellation>());
		FString ErrorCode;
		FString ErrorDetail;
		const bool bFailedClosed = TryOperationError(
			Result, ErrorCode, ErrorDetail) && !ErrorCode.IsEmpty();
		return Test.TestTrue(*FString::Printf(
			TEXT("%s fails closed without synthetic success"), *CapabilityId),
			bFailedClosed);
	}

	TSharedPtr<FGraphCompoundRuntime> MakeRuntime(FAutomationTestBase& Test)
	{
		FGraphCompoundRuntimeConfig RuntimeConfig;
		RuntimeConfig.MaxFrameBytes = 1024 * 1024;
		FGraphCompoundAudioMediaProductionConfig ProductionConfig;
		ProductionConfig.OperationTimeoutSeconds = 30.0;
		TSharedPtr<FGraphCompoundRuntime> Runtime;
		FGraphCompoundRuntimeError Error;
		if (!Test.TestTrue(TEXT("Audio/Media production runtime builds"),
				TryCreateAudioMediaGraphProductionRuntime(
					RuntimeConfig, ProductionConfig, Runtime, Error))
			|| !Test.TestNotNull(TEXT("Audio/Media runtime exists"), Runtime.Get()))
		{
			Test.AddError(Error.Code + TEXT(":") + Error.Detail);
			Runtime.Reset();
		}
		return Runtime;
	}

	FString CreatePayload(const FString& PackageName, const FString& Kind)
	{
		return FString::Printf(
			TEXT("{\"asset_kind\":\"%s\",\"asset_name\":\"%s\","
				"\"package_path\":\"%s\"}"),
			*Kind,
			*FPackageName::GetLongPackageAssetName(PackageName),
			*FPackageName::GetLongPackagePath(PackageName));
	}

	FString GuidText(const FGuid& Guid)
	{
		return Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundAudioMediaMetaSoundProductionTest,
	"UeAgentInterface.Composition.GraphCompound.AudioMedia.MetaSoundProduction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundAudioMediaMetaSoundProductionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FAudioMediaAssetSet Assets;
	TSharedPtr<FGraphCompoundRuntime> Runtime = MakeRuntime(*this);
	if (!Runtime.IsValid())
	{
		return false;
	}
	const FString PackageName = Assets.Add(TEXT("MS_W05"));
	const FString ObjectPath = FAudioMediaAssetSet::ObjectPath(PackageName);
	FString Revision;
	if (!Submit(*this, *Runtime, TEXT("audio_media.asset.create"),
			CreatePayload(PackageName, TEXT("metasound_source")),
			TEXT("ms-create"), Revision)
		|| !Submit(*this, *Runtime, TEXT("metasound.graph.interface.mutate"),
			FString::Printf(TEXT("{\"changes\":[{\"access_type\":\"reference\","
				"\"data_type_ref\":\"Float\",\"default_value\":220.0,"
				"\"intent\":\"create\",\"interface_kind\":\"input\","
				"\"name\":\"Frequency\"}],\"target_ref\":\"%s\"}"),
				*PackageName), TEXT("ms-interface"), Revision))
	{
		return false;
	}
	const FGuid RejectedNodeId = FGuid::NewGuid();
	if (!SubmitExpectError(*this, *Runtime, TEXT("metasound.graph.node.mutate"),
		FString::Printf(TEXT("{\"nodes\":[{\"class_name\":\"UE.Missing.Audio\"," 
			"\"intent\":\"create\",\"major_version\":1,\"minor_version\":0,"
			"\"node_guid\":\"%s\"}],\"target_ref\":\"%s\"}"),
			*GuidText(RejectedNodeId), *PackageName), TEXT("ms-invalid-class")))
	{
		return false;
	}
	UMetaSoundSource* Source = LoadObject<UMetaSoundSource>(nullptr, *ObjectPath);
	const IMetaSoundDocumentInterface* RejectedDocument =
		Cast<IMetaSoundDocumentInterface>(Source);
	const FMetasoundFrontendGraph* RejectedGraph = RejectedDocument
		? &RejectedDocument->GetConstDocument().RootGraph.GetConstDefaultGraph()
		: nullptr;
	TestTrue(TEXT("MetaSound failed node create leaves no native node"),
		RejectedGraph && !RejectedGraph->Nodes.ContainsByPredicate(
			[&RejectedNodeId](const FMetasoundFrontendNode& Node)
			{
				return Node.GetID() == RejectedNodeId;
			}));

	const FGuid OscillatorId = FGuid::NewGuid();
	if (!Submit(*this, *Runtime, TEXT("metasound.graph.node.mutate"),
		FString::Printf(TEXT("{\"nodes\":[{\"class_name\":\"UE.Sine.Audio\"," 
			"\"intent\":\"create\",\"major_version\":1,\"minor_version\":1,"
			"\"node_guid\":\"%s\"}],\"target_ref\":\"%s\"}"),
			*GuidText(OscillatorId), *PackageName), TEXT("ms-node"), Revision))
	{
		return false;
	}

	Source = LoadObject<UMetaSoundSource>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("MetaSound source loads after mutation"), Source))
	{
		return false;
	}
	UMetaSoundBuilderBase& Builder =
		Metasound::Engine::FDocumentBuilderRegistry::GetChecked()
			.FindOrBeginBuilding(*Source, true);
	EMetaSoundBuilderResult BuilderResult = EMetaSoundBuilderResult::Failed;
	FName InputType;
	FMetaSoundBuilderNodeOutputHandle FrequencyOutput;
	const FMetaSoundNodeHandle FrequencyNode = Builder.FindGraphInputNode(
		TEXT("Frequency"), InputType, FrequencyOutput, BuilderResult);
	const FMetasoundFrontendVertex* FrequencyInput =
		Builder.GetBuilder().FindNodeInput(OscillatorId, TEXT("Frequency"));
	if (!TestTrue(TEXT("MetaSound independent readback resolves native endpoints"),
		BuilderResult == EMetaSoundBuilderResult::Succeeded
			&& FrequencyNode.IsSet() && FrequencyOutput.IsSet()
			&& FrequencyInput != nullptr))
	{
		return false;
	}
	const FGuid InputVertexGuid = FrequencyInput->VertexID;
	const FString InputVertexId = GuidText(InputVertexGuid);
	const FMetasoundFrontendClassName ProbeClassName = Builder.GetBuilder()
		.GetConstDocumentChecked().RootGraph.Metadata.GetClassName();
	const FTopLevelAssetPath ProbeAssetPath = Builder.GetBuilder().GetHintPath();
	if (!TestTrue(TEXT("MetaSound readback probe releases its builder"),
		Metasound::Engine::FDocumentBuilderRegistry::GetChecked().FinishBuilding(
			ProbeClassName, ProbeAssetPath)))
	{
		return false;
	}
	if (!Submit(*this, *Runtime, TEXT("metasound.graph.default.mutate"),
			FString::Printf(TEXT("{\"data_type_ref\":\"Float\",\"intent\":\"update\","
				"\"node_guid\":\"%s\",\"target_ref\":\"%s\",\"value\":440.0,"
				"\"vertex_guid\":\"%s\"}"), *GuidText(OscillatorId),
				*PackageName, *InputVertexId), TEXT("ms-default"), Revision)
		|| !Submit(*this, *Runtime, TEXT("metasound.graph.link.mutate"),
			FString::Printf(TEXT("{\"links\":[{\"from\":{\"data_type_ref\":\"Float\","
				"\"direction\":\"output\",\"node_guid\":\"%s\",\"vertex_guid\":\"%s\"},"
				"\"intent\":\"create\",\"to\":{\"data_type_ref\":\"Float\","
				"\"direction\":\"input\",\"node_guid\":\"%s\",\"vertex_guid\":\"%s\"}}],"
				"\"target_ref\":\"%s\"}"), *GuidText(FrequencyOutput.NodeID),
				*GuidText(FrequencyOutput.VertexID), *GuidText(OscillatorId),
				*InputVertexId, *PackageName), TEXT("ms-link"), Revision)
		|| !Submit(*this, *Runtime, TEXT("metasound.graph.compile_settle"),
			FString::Printf(TEXT("{\"expected_document_revision\":\"%s\","
				"\"registration_policy\":\"register_and_compile\","
				"\"target_ref\":\"%s\"}"), *Revision, *PackageName),
			TEXT("ms-settle"), Revision))
	{
		return false;
	}
	Source = nullptr;
	if (!TestTrue(TEXT("MetaSound package unloads before persistence readback"),
		Assets.Unload(PackageName)))
	{
		return false;
	}
	Source = LoadObject<UMetaSoundSource>(nullptr, *ObjectPath);
	const IMetaSoundDocumentInterface* Document =
		Cast<IMetaSoundDocumentInterface>(Source);
	const FMetasoundFrontendGraph* Graph = Document
		? &Document->GetConstDocument().RootGraph.GetConstDefaultGraph() : nullptr;
	const bool bNodePersisted = Graph && Graph->Nodes.ContainsByPredicate(
		[&OscillatorId](const FMetasoundFrontendNode& Node)
		{
			return Node.GetID() == OscillatorId;
		});
	const bool bLinkPersisted = Graph && Graph->Edges.ContainsByPredicate(
		[&OscillatorId, &InputVertexId](const FMetasoundFrontendEdge& Edge)
		{
			return Edge.ToNodeID == OscillatorId
				&& GuidText(Edge.ToVertexID) == InputVertexId;
		});
	float PersistedFrequency = 0.0f;
	bool bDefaultPersisted = false;
	if (Source)
	{
		UMetaSoundBuilderBase& ReadbackBuilder =
			Metasound::Engine::FDocumentBuilderRegistry::GetChecked()
				.FindOrBeginBuilding(*Source, true);
		EMetaSoundBuilderResult DefaultResult = EMetaSoundBuilderResult::Failed;
		const FMetasoundFrontendLiteral DefaultLiteral =
			ReadbackBuilder.GetNodeInputDefault(
				FMetaSoundBuilderNodeInputHandle(
					OscillatorId, InputVertexGuid),
				DefaultResult);
		bDefaultPersisted = DefaultResult == EMetaSoundBuilderResult::Succeeded
			&& DefaultLiteral.TryGet(PersistedFrequency)
			&& FMath::IsNearlyEqual(PersistedFrequency, 440.0f);
		const FMetasoundFrontendClassName ReadbackClassName =
			ReadbackBuilder.GetBuilder().GetConstDocumentChecked()
				.RootGraph.Metadata.GetClassName();
		const FTopLevelAssetPath ReadbackAssetPath =
			ReadbackBuilder.GetBuilder().GetHintPath();
		TestTrue(TEXT("MetaSound persistence probe releases its builder"),
			Metasound::Engine::FDocumentBuilderRegistry::GetChecked()
				.FinishBuilding(ReadbackClassName, ReadbackAssetPath));
	}
	TestTrue(TEXT("MetaSound node persists in frontend document"), bNodePersisted);
	TestTrue(TEXT("MetaSound link persists in frontend document"), bLinkPersisted);
	TestTrue(TEXT("MetaSound default persists after unload and reload"),
		bDefaultPersisted);
	TestTrue(TEXT("MetaSound cleanup succeeds"), Assets.Cleanup());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundAudioMediaSoundCueProductionTest,
	"UeAgentInterface.Composition.GraphCompound.AudioMedia.SoundCueProduction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundAudioMediaSoundCueProductionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FAudioMediaAssetSet Assets;
	TSharedPtr<FGraphCompoundRuntime> Runtime = MakeRuntime(*this);
	if (!Runtime.IsValid())
	{
		return false;
	}
	const FString PackageName = Assets.Add(TEXT("SC_W05"));
	const FString AssetObjectPath = FAudioMediaAssetSet::ObjectPath(PackageName);
	const FString RootPath = AssetObjectPath + TEXT(":MixerRoot");
	const FString ChildPath = AssetObjectPath + TEXT(":WaveChild");
	FString Revision;
	if (!Submit(*this, *Runtime, TEXT("audio_media.asset.create"),
			CreatePayload(PackageName, TEXT("sound_cue")),
			TEXT("sc-create"), Revision))
	{
		return false;
	}
	const FString RejectedPath = AssetObjectPath + TEXT(":RejectedNode");
	if (!SubmitExpectError(*this, *Runtime, TEXT("sound_cue.graph.node.mutate"),
		FString::Printf(TEXT("{\"nodes\":[{\"intent\":\"create\"," 
			"\"native_class_ref\":\"/Script/Engine.SoundNodeMissing\"," 
			"\"object_path\":\"%s\"}],\"target_ref\":\"%s\"}"),
			*RejectedPath, *PackageName), TEXT("sc-invalid-class")))
	{
		return false;
	}
	USoundCue* Cue = LoadObject<USoundCue>(nullptr, *AssetObjectPath);
	TestNull(TEXT("SoundCue failed node create leaves no UObject"),
		Cue ? FindObject<UObject>(Cue, TEXT("RejectedNode")) : nullptr);
	if (!Submit(*this, *Runtime, TEXT("sound_cue.graph.node.mutate"),
			FString::Printf(TEXT("{\"nodes\":[{\"intent\":\"create\"," 
				"\"native_class_ref\":\"/Script/Engine.SoundNodeMixer\","
				"\"object_path\":\"%s\"},{\"intent\":\"create\","
				"\"native_class_ref\":\"/Script/Engine.SoundNodeWavePlayer\","
				"\"object_path\":\"%s\"}],\"target_ref\":\"%s\"}"),
				*RootPath, *ChildPath, *PackageName), TEXT("sc-nodes"), Revision)
		|| !Submit(*this, *Runtime, TEXT("sound_cue.graph.relation.mutate"),
			FString::Printf(TEXT("{\"relations\":[{\"child_object_path\":\"%s\","
				"\"intent\":\"create\",\"order_index\":0,"
				"\"parent_object_path\":\"%s\"}],\"target_ref\":\"%s\"}"),
				*ChildPath, *RootPath, *PackageName), TEXT("sc-relation"), Revision)
		|| !Submit(*this, *Runtime, TEXT("sound_cue.graph.root.mutate"),
			FString::Printf(TEXT("{\"intent\":\"update\","
				"\"root_node_object_path\":\"%s\",\"target_ref\":\"%s\"}"),
				*RootPath, *PackageName), TEXT("sc-root"), Revision)
		|| !Submit(*this, *Runtime, TEXT("sound_cue.graph.compile_settle"),
			FString::Printf(TEXT("{\"expected_graph_revision\":\"%s\","
				"\"sync_policy\":\"sync_and_compile\",\"target_ref\":\"%s\"}"),
				*Revision, *PackageName), TEXT("sc-settle"), Revision))
	{
		return false;
	}
	Cue = nullptr;
	if (!TestTrue(TEXT("SoundCue package unloads before persistence readback"),
		Assets.Unload(PackageName)))
	{
		return false;
	}
	Cue = LoadObject<USoundCue>(nullptr, *AssetObjectPath);
	USoundNodeMixer* Root = Cue ? Cast<USoundNodeMixer>(Cue->FirstNode) : nullptr;
	TestNotNull(TEXT("SoundCue root persists"), Root);
	TestTrue(TEXT("SoundCue ordered child persists"), Root
		&& Root->ChildNodes.Num() == 1
		&& Root->ChildNodes[0]
		&& Root->ChildNodes[0]->GetPathName() == ChildPath);
	TestTrue(TEXT("SoundCue graph compile barrier persists"),
		Cue && Cue->GetGraph() != nullptr);
	TestTrue(TEXT("SoundCue cleanup succeeds"), Assets.Cleanup());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundAudioMediaTextureGraphProductionTest,
	"UeAgentInterface.Composition.GraphCompound.AudioMedia.TextureGraphProduction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundAudioMediaTextureGraphProductionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FAudioMediaAssetSet Assets;
	TSharedPtr<FGraphCompoundRuntime> Runtime = MakeRuntime(*this);
	if (!Runtime.IsValid())
	{
		return false;
	}
	const FString PackageName = Assets.Add(TEXT("TG_W05"));
	const FString AssetObjectPath = FAudioMediaAssetSet::ObjectPath(PackageName);
	FString Revision;
	if (!Submit(*this, *Runtime, TEXT("audio_media.asset.create"),
			CreatePayload(PackageName, TEXT("texture_graph")),
			TEXT("tg-create"), Revision))
	{
		return false;
	}
	if (!SubmitExpectError(*this, *Runtime,
		TEXT("texture_graph.graph.node.mutate"),
		FString::Printf(TEXT("{\"nodes\":[{\"element_id\":\"rejected_expression\"," 
			"\"expression_class_ref\":\"/Script/TextureGraph.TG_Expression_Missing\"," 
			"\"intent\":\"create\"}],\"target_ref\":\"%s\"}"),
			*PackageName), TEXT("tg-invalid-class")))
	{
		return false;
	}
	UTextureGraph* Graph = LoadObject<UTextureGraph>(nullptr, *AssetObjectPath);
	bool bRejectedExpressionExists = false;
	if (Graph && Graph->Graph())
	{
		Graph->Graph()->ForEachNodes(
			[&bRejectedExpressionExists](const UTG_Node* Node, const uint32)
			{
				bRejectedExpressionExists = bRejectedExpressionExists
					|| (Node && Node->GetExpression()
						&& Node->GetExpression()->GetName()
							== TEXT("rejected_expression"));
			});
	}
	TestFalse(TEXT("TextureGraph failed node create leaves no expression"),
		bRejectedExpressionExists);
	if (!Submit(*this, *Runtime, TEXT("texture_graph.graph.node.mutate"),
			FString::Printf(TEXT("{\"nodes\":[{\"element_id\":\"scalar_gain\"," 
				"\"expression_class_ref\":\"/Script/TextureGraph.TG_Expression_Scalar\","
				"\"intent\":\"create\"}],\"target_ref\":\"%s\"}"),
				*PackageName), TEXT("tg-node"), Revision)
		|| !Submit(*this, *Runtime, TEXT("texture_graph.graph.parameter.mutate"),
			FString::Printf(TEXT("{\"parameters\":[{\"alias\":\"Scale\","
				"\"element_id\":\"scalar_gain\",\"intent\":\"update\","
				"\"pin_name\":\"Scalar\",\"value\":0.75}],"
				"\"target_ref\":\"%s\"}"), *PackageName),
			TEXT("tg-parameter"), Revision)
		|| !Submit(*this, *Runtime, TEXT("texture_graph.graph.link.mutate"),
			FString::Printf(TEXT("{\"links\":[{\"from\":{\"direction\":\"output\","
				"\"element_id\":\"scalar_gain\",\"pin_name\":\"ValueOut\"},"
				"\"intent\":\"create\",\"to\":{\"direction\":\"input\","
				"\"element_id\":\"builtin_output\",\"pin_name\":\"Source\"}}],"
				"\"target_ref\":\"%s\"}"), *PackageName),
			TEXT("tg-link"), Revision)
		|| !Submit(*this, *Runtime,
			TEXT("texture_graph.graph.validate_evaluate_settle"),
			FString::Printf(TEXT("{\"evaluation_policy\":\"validate_and_evaluate\","
				"\"expected_graph_revision\":\"%s\",\"target_ref\":\"%s\"}"),
				*Revision, *PackageName), TEXT("tg-settle"), Revision))
	{
		return false;
	}

	const FAudioMediaGraphRouteProfile* QueryProfile =
		FindAudioMediaGraphRouteProfile(TEXT("texture_graph.artifact.capture"));
	if (!TestNotNull(TEXT("TextureGraph query profile exists"), QueryProfile))
	{
		return false;
	}
	const FString QueryPayload = FString::Printf(
		TEXT("{\"expected_settled_revision\":\"%s\",\"outputs\":[{"
			"\"element_id\":\"builtin_output\",\"output_name\":\"Output\"}],"
			"\"target_ref\":\"%s\"}"), *Revision, *PackageName);
	const FUeAgentQueryResult QueryResult = Runtime->GetQueryPort()->Submit(
		FUeAgentQueryRequest(
			QueryProfile->GetExecutionContract().GetDescriptorIdentity(),
			TEXT("request:w05:tg-artifact"),
			FUeAgentContractValue(QueryPayload)),
		Caller(TEXT("texture_graph.artifact.capture")),
		MakeShared<FActiveCancellation>());
	if (!TestTrue(TEXT("TextureGraph query result envelope is valid"),
		QueryResult.IsValid()))
	{
		return false;
	}
	FString QueryErrorCode;
	FString QueryErrorDetail;
	if (TryQueryError(QueryResult, QueryErrorCode, QueryErrorDetail))
	{
		AddError(TEXT("texture_graph.artifact.capture: ") + QueryErrorCode
			+ TEXT(":") + QueryErrorDetail);
		return false;
	}
	TSharedPtr<FJsonObject> QueryRoot;
	const TSharedRef<TJsonReader<>> QueryReader =
		TJsonReaderFactory<>::Create(QueryResult.GetValue().GetJsonValue());
	bool bPublished = true;
	const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
	TSharedPtr<FJsonObject> OutputState;
	FString CapturedExpressionPath;
	FString CapturedOutputName;
	FString CapturedValueType;
	FString CapturedSettingsFingerprint;
	int32 CapturedNodeIndex = INDEX_NONE;
	int32 CapturedPinIndex = INDEX_NONE;
	bool bQueryExact = QueryResult.IsValid()
		&& FJsonSerializer::Deserialize(QueryReader, QueryRoot)
		&& QueryRoot.IsValid()
		&& QueryRoot->TryGetBoolField(TEXT("published"), bPublished)
		&& !bPublished
		&& QueryRoot->TryGetArrayField(TEXT("outputs"), Outputs)
		&& Outputs && Outputs->Num() == 1;
	if (bQueryExact && (*Outputs)[0].IsValid()
		&& (*Outputs)[0]->Type == EJson::Object)
	{
		OutputState = (*Outputs)[0]->AsObject();
		bQueryExact = OutputState.IsValid()
			&& OutputState->TryGetStringField(
				TEXT("expression_object_path"), CapturedExpressionPath)
			&& OutputState->TryGetStringField(
				TEXT("output_name"), CapturedOutputName)
			&& OutputState->TryGetStringField(
				TEXT("evaluated_value_type"), CapturedValueType)
			&& OutputState->TryGetStringField(
				TEXT("settings_fingerprint"), CapturedSettingsFingerprint)
			&& OutputState->TryGetNumberField(
				TEXT("node_index"), CapturedNodeIndex)
			&& OutputState->TryGetNumberField(
				TEXT("pin_index"), CapturedPinIndex)
			&& !CapturedExpressionPath.IsEmpty()
			&& CapturedOutputName == TEXT("Output")
			&& !CapturedValueType.IsEmpty()
			&& !CapturedSettingsFingerprint.IsEmpty();
	}
	TestTrue(TEXT("TextureGraph artifact capture is read-only native state"),
		bQueryExact);

	Graph = nullptr;
	if (!TestTrue(TEXT("TextureGraph package unloads before persistence readback"),
		Assets.Unload(PackageName)))
	{
		return false;
	}
	Graph = LoadObject<UTextureGraph>(nullptr, *AssetObjectPath);
	UTG_Node* ScalarNode = nullptr;
	UTG_Node* OutputNode = nullptr;
	if (Graph && Graph->Graph())
	{
		Graph->Graph()->ForEachNodes(
			[&](const UTG_Node* Node, const uint32)
			{
				if (!Node)
				{
					return;
				}
				if (Cast<UTG_Expression_Scalar>(Node->GetExpression()))
				{
					ScalarNode = const_cast<UTG_Node*>(Node);
				}
				if (Cast<UTG_Expression_Output>(Node->GetExpression()))
				{
					OutputNode = const_cast<UTG_Node*>(Node);
				}
			});
	}
	FTG_Name ScalarPinName = TEXT("Scalar");
	FTG_Name OutputPinName = TEXT("Source");
	UTG_Pin* ScalarPin = ScalarNode
		? ScalarNode->GetInputPin(ScalarPinName) : nullptr;
	UTG_Pin* OutputPin = OutputNode
		? OutputNode->GetInputPin(OutputPinName) : nullptr;
	FTG_Name ArtifactPinName = TEXT("Output");
	UTG_Pin* ArtifactPin = OutputNode
		? OutputNode->GetOutputPin(ArtifactPinName) : nullptr;
	float PersistedScalarValue = 0.0f;
	TestTrue(TEXT("TextureGraph semantic element persists"), ScalarNode
		&& ScalarNode->GetExpression()->GetName() == TEXT("scalar_gain"));
	TestTrue(TEXT("TextureGraph parameter alias/value persists"), ScalarPin
		&& ScalarPin->GetAliasName() == TEXT("Scale")
		&& ScalarPin->GetValue(PersistedScalarValue)
		&& FMath::IsNearlyEqual(PersistedScalarValue, 0.75f));
	TestTrue(TEXT("TextureGraph native edge persists"), OutputPin
		&& !OutputPin->GetEdges().IsEmpty());
	TestTrue(TEXT("TextureGraph capture reports independently read native identity"),
		OutputNode && ArtifactPin && OutputNode->GetExpression()
		&& CapturedExpressionPath == OutputNode->GetExpression()->GetPathName()
		&& CapturedNodeIndex == OutputNode->GetId().NodeIdx()
		&& CapturedPinIndex == ArtifactPin->GetId().PinIdx());
	TestTrue(TEXT("TextureGraph cleanup succeeds"), Assets.Cleanup());
	return !HasAnyErrors();
}
