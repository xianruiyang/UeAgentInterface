#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphMetaSoundNativeHook.h"

#include "Editor.h"
#include "MetasoundBuilderBase.h"
#include "MetasoundDocumentBuilderRegistry.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendSearchEngine.h"
#include "MetasoundSource.h"

namespace UE::UAI::DomainAdapters::AudioMediaGraph
{
	namespace
	{
		FString MakeEvidenceId(const TCHAR* Stage)
		{
			return FString::Printf(TEXT("metasound:%s:%s"), Stage,
				*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		}

		class FMetaSoundBuilderLease final
		{
		public:
			explicit FMetaSoundBuilderLease(UMetaSoundBuilderBase& InBuilder)
				: Builder(&InBuilder)
				, ClassName(InBuilder.GetBuilder().GetConstDocumentChecked()
					.RootGraph.Metadata.GetClassName())
				, AssetPath(InBuilder.GetBuilder().GetHintPath())
			{
			}

			~FMetaSoundBuilderLease()
			{
				Finish();
			}

			bool Finish()
			{
				if (Builder == nullptr)
				{
					return true;
				}
				const bool bFinished =
					Metasound::Engine::FDocumentBuilderRegistry::GetChecked()
						.FinishBuilding(ClassName, AssetPath);
				Builder = nullptr;
				return bFinished;
			}

		private:
			UMetaSoundBuilderBase* Builder = nullptr;
			FMetasoundFrontendClassName ClassName;
			FTopLevelAssetPath AssetPath;
		};

		UMetaSoundBuilderBase* TryOpenBuilder(
			UMetaSoundSource& Source,
			FAudioMediaGraphError& OutError)
		{
			UMetaSoundEditorSubsystem* const Subsystem = GEditor
				? GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>()
				: nullptr;
			if (Subsystem == nullptr
				|| Cast<IMetaSoundDocumentInterface>(&Source) == nullptr)
			{
				OutError.Set(TEXT("audio_media_graph_metasound_builder_unavailable"));
				return nullptr;
			}
			return &Metasound::Engine::FDocumentBuilderRegistry::GetChecked()
				.FindOrBeginBuilding(Source, true);
		}

		bool TryFinalizeBuilder(
			UMetaSoundSource& Source,
			UMetaSoundBuilderBase& Builder,
			FAudioMediaGraphError& OutError)
		{
			UMetaSoundEditorSubsystem* const Subsystem = GEditor
				? GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>()
				: nullptr;
			if (Subsystem == nullptr)
			{
				OutError.Set(TEXT("audio_media_graph_metasound_builder_unavailable"));
				return false;
			}
			Builder.InitNodeLocations();
			Builder.ConformObjectToDocument();
			Subsystem->RegisterGraphWithFrontend(Source, true);
			Source.PostEditChange();
			return true;
		}

		const FMetasoundFrontendClass* FindNodeClass(
			const FMetaSoundFrontendDocumentBuilder& Builder,
			const FGuid& NodeId)
		{
			const FMetasoundFrontendNode* const Node = Builder.FindNode(NodeId);
			if (Node == nullptr)
			{
				return nullptr;
			}
			return Builder.GetConstDocumentChecked().Dependencies.FindByPredicate(
				[Node](const FMetasoundFrontendClass& Item)
				{
					return Item.ID == Node->ClassID;
				});
		}
	}

	bool FAudioMediaGraphMetaSoundNativeHook::TryApplyInterfaceMutations(
		UMetaSoundSource& Source,
		const TArray<FAudioMediaGraphMetaSoundInterfaceMutation>& Mutations,
		FAudioMediaGraphError& OutError) const
	{
		OutError.Reset();
		UMetaSoundBuilderBase* const Builder = TryOpenBuilder(Source, OutError);
		if (Builder == nullptr)
		{
			return false;
		}
		FMetaSoundBuilderLease Lease(*Builder);
		for (const FAudioMediaGraphMetaSoundInterfaceMutation& Mutation
			: Mutations)
		{
			const bool bInput = Mutation.GetInterfaceKind() == TEXT("input");
			if (!bInput && Mutation.GetInterfaceKind() != TEXT("output"))
			{
				OutError.Set(TEXT("audio_media_graph_metasound_interface_invalid"));
				return false;
			}
			EMetaSoundBuilderResult FindResult = EMetaSoundBuilderResult::Failed;
			const FMetaSoundNodeHandle Existing = bInput
				? Builder->FindGraphInputNode(Mutation.GetName(), FindResult)
				: Builder->FindGraphOutputNode(Mutation.GetName(), FindResult);
			const bool bExists = FindResult == EMetaSoundBuilderResult::Succeeded
				&& Existing.IsSet();
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Delete)
			{
				if (!bExists)
				{
					OutError.Set(TEXT("audio_media_graph_metasound_interface_missing"));
					return false;
				}
				EMetaSoundBuilderResult RemoveResult = EMetaSoundBuilderResult::Failed;
				if (bInput)
				{
					Builder->RemoveGraphInput(Mutation.GetName(), RemoveResult);
				}
				else
				{
					Builder->RemoveGraphOutput(Mutation.GetName(), RemoveResult);
				}
				if (RemoveResult != EMetaSoundBuilderResult::Succeeded)
				{
					OutError.Set(TEXT("audio_media_graph_metasound_interface_apply_failed"));
					return false;
				}
				continue;
			}
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Update)
			{
				if (!bExists || !bInput)
				{
					OutError.Set(TEXT("audio_media_graph_metasound_interface_mismatch"));
					return false;
				}
				EMetaSoundBuilderResult SetResult = EMetaSoundBuilderResult::Failed;
				Builder->SetGraphInputDefault(
					Mutation.GetName(), Mutation.GetDefaultValue(), SetResult);
				if (SetResult != EMetaSoundBuilderResult::Succeeded)
				{
					OutError.Set(TEXT("audio_media_graph_metasound_default_failed"));
					return false;
				}
				continue;
			}
			if (bExists)
			{
				if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Create)
				{
					OutError.Set(TEXT("audio_media_graph_metasound_interface_exists"));
					return false;
				}
				EMetaSoundBuilderResult RemoveResult = EMetaSoundBuilderResult::Failed;
				if (bInput)
				{
					Builder->RemoveGraphInput(Mutation.GetName(), RemoveResult);
				}
				else
				{
					Builder->RemoveGraphOutput(Mutation.GetName(), RemoveResult);
				}
				if (RemoveResult != EMetaSoundBuilderResult::Succeeded)
				{
					OutError.Set(TEXT("audio_media_graph_metasound_interface_apply_failed"));
					return false;
				}
			}
			EMetaSoundBuilderResult AddResult = EMetaSoundBuilderResult::Failed;
			if (bInput)
			{
				Builder->AddGraphInputNode(
					Mutation.GetName(), Mutation.GetDataType(),
					Mutation.GetDefaultValue(), AddResult,
					Mutation.IsConstructor());
			}
			else
			{
				Builder->AddGraphOutputNode(
					Mutation.GetName(), Mutation.GetDataType(),
					Mutation.GetDefaultValue(), AddResult,
					Mutation.IsConstructor());
			}
			if (AddResult != EMetaSoundBuilderResult::Succeeded)
			{
				OutError.Set(TEXT("audio_media_graph_metasound_interface_apply_failed"));
				return false;
			}
		}
		if (!TryFinalizeBuilder(Source, *Builder, OutError)
			|| !Lease.Finish())
		{
			if (OutError.GetCode().IsEmpty())
			{
				OutError.Set(TEXT("audio_media_graph_metasound_builder_release_failed"));
			}
			return false;
		}
		return true;
	}

	bool FAudioMediaGraphMetaSoundNativeHook::TryApplyNodeMutations(
		UMetaSoundSource& Source,
		const TArray<FAudioMediaGraphMetaSoundNodeMutation>& Mutations,
		FAudioMediaGraphError& OutError) const
	{
		OutError.Reset();
		UMetaSoundBuilderBase* const Builder = TryOpenBuilder(Source, OutError);
		if (Builder == nullptr)
		{
			return false;
		}
		FMetaSoundBuilderLease Lease(*Builder);
		for (const FAudioMediaGraphMetaSoundNodeMutation& Mutation : Mutations)
		{
			FMetasoundFrontendClassName ClassName;
			if (!Mutation.GetNodeId().IsValid()
				|| !FMetasoundFrontendClassName::Parse(
					Mutation.GetClassName(), ClassName))
			{
				OutError.Set(TEXT("audio_media_graph_metasound_node_spec_invalid"));
				return false;
			}
			if (Mutation.GetIntent() != EAudioMediaGraphMutationIntent::Delete)
			{
				FMetasoundFrontendClass RegisteredClass;
				if (!Metasound::Frontend::ISearchEngine::Get()
						.FindClassWithHighestMinorVersion(
							ClassName, Mutation.GetMajorVersion(), RegisteredClass))
				{
					OutError.Set(TEXT("audio_media_graph_metasound_node_class_missing"),
						Mutation.GetClassName());
					return false;
				}
				if (RegisteredClass.Metadata.GetVersion().Minor
					!= Mutation.GetMinorVersion())
				{
					OutError.Set(TEXT("audio_media_graph_metasound_node_version_mismatch"),
						Mutation.GetClassName());
					return false;
				}
			}
			const bool bExists = Builder->GetBuilder().FindNode(
				Mutation.GetNodeId()) != nullptr;
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Delete)
			{
				if (!bExists)
				{
					OutError.Set(TEXT("audio_media_graph_metasound_node_missing"));
					return false;
				}
				EMetaSoundBuilderResult RemoveResult = EMetaSoundBuilderResult::Failed;
				Builder->RemoveNode(
					FMetaSoundNodeHandle(Mutation.GetNodeId()), RemoveResult);
				if (RemoveResult != EMetaSoundBuilderResult::Succeeded)
				{
					OutError.Set(TEXT("audio_media_graph_metasound_node_apply_failed"));
					return false;
				}
				continue;
			}
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Update)
			{
				if (!bExists)
				{
					OutError.Set(TEXT("audio_media_graph_metasound_node_missing"));
					return false;
				}
			}
			else
			{
				if (bExists)
				{
					if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Create)
					{
						OutError.Set(TEXT("audio_media_graph_metasound_node_exists"));
						return false;
					}
					EMetaSoundBuilderResult RemoveResult = EMetaSoundBuilderResult::Failed;
					Builder->RemoveNode(
						FMetaSoundNodeHandle(Mutation.GetNodeId()), RemoveResult);
					if (RemoveResult != EMetaSoundBuilderResult::Succeeded)
					{
						OutError.Set(TEXT("audio_media_graph_metasound_node_apply_failed"));
						return false;
					}
				}
				if (Builder->GetBuilder().AddNodeByClassName(
						ClassName, Mutation.GetMajorVersion(),
						Mutation.GetNodeId()) == nullptr)
				{
					OutError.Set(TEXT("audio_media_graph_metasound_node_class_missing"),
						Mutation.GetClassName());
					return false;
				}
			}
			const FMetasoundFrontendClass* const NativeClass =
				FindNodeClass(Builder->GetBuilder(), Mutation.GetNodeId());
			if (NativeClass == nullptr
				|| NativeClass->Metadata.GetClassName() != ClassName
				|| NativeClass->Metadata.GetVersion().Major
					!= Mutation.GetMajorVersion()
				|| NativeClass->Metadata.GetVersion().Minor
					!= Mutation.GetMinorVersion())
			{
				OutError.Set(TEXT("audio_media_graph_metasound_node_version_mismatch"),
					Mutation.GetClassName());
				return false;
			}
		}
		if (!TryFinalizeBuilder(Source, *Builder, OutError)
			|| !Lease.Finish())
		{
			if (OutError.GetCode().IsEmpty())
			{
				OutError.Set(TEXT("audio_media_graph_metasound_builder_release_failed"));
			}
			return false;
		}
		return true;
	}

	bool FAudioMediaGraphMetaSoundNativeHook::TryApplyLinkMutations(
		UMetaSoundSource& Source,
		const TArray<FAudioMediaGraphMetaSoundLinkMutation>& Mutations,
		FAudioMediaGraphError& OutError) const
	{
		OutError.Reset();
		UMetaSoundBuilderBase* const Builder = TryOpenBuilder(Source, OutError);
		if (Builder == nullptr)
		{
			return false;
		}
		FMetaSoundBuilderLease Lease(*Builder);
		for (const FAudioMediaGraphMetaSoundLinkMutation& Mutation : Mutations)
		{
			const FMetaSoundBuilderNodeOutputHandle Output(
				Mutation.GetFromNodeId(), Mutation.GetFromVertexId());
			const FMetaSoundBuilderNodeInputHandle Input(
				Mutation.GetToNodeId(), Mutation.GetToVertexId());
			if (!Builder->ContainsNodeOutput(Output)
				|| !Builder->ContainsNodeInput(Input))
			{
				OutError.Set(TEXT("audio_media_graph_metasound_link_endpoint_missing"));
				return false;
			}
			EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Delete)
			{
				Builder->DisconnectNodes(Output, Input, Result);
			}
			else
			{
				Builder->ConnectNodes(Output, Input, Result);
			}
			if (Result != EMetaSoundBuilderResult::Succeeded)
			{
				OutError.Set(TEXT("audio_media_graph_metasound_link_apply_failed"));
				return false;
			}
		}
		if (!TryFinalizeBuilder(Source, *Builder, OutError)
			|| !Lease.Finish())
		{
			if (OutError.GetCode().IsEmpty())
			{
				OutError.Set(TEXT("audio_media_graph_metasound_builder_release_failed"));
			}
			return false;
		}
		return true;
	}

	bool FAudioMediaGraphMetaSoundNativeHook::TryApplyDefaultMutation(
		UMetaSoundSource& Source,
		const FAudioMediaGraphMetaSoundDefaultMutation& Mutation,
		FAudioMediaGraphError& OutError) const
	{
		OutError.Reset();
		UMetaSoundBuilderBase* const Builder = TryOpenBuilder(Source, OutError);
		if (Builder == nullptr)
		{
			return false;
		}
		FMetaSoundBuilderLease Lease(*Builder);
		const FMetaSoundBuilderNodeInputHandle Input(
			Mutation.GetNodeId(), Mutation.GetVertexId());
		if (!Builder->ContainsNodeInput(Input))
		{
			OutError.Set(TEXT("audio_media_graph_metasound_default_endpoint_missing"));
			return false;
		}
		EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
		Builder->SetNodeInputDefault(Input, Mutation.GetValue(), Result);
		if (Result != EMetaSoundBuilderResult::Succeeded)
		{
			OutError.Set(TEXT("audio_media_graph_metasound_default_failed"));
			return false;
		}
		if (!TryFinalizeBuilder(Source, *Builder, OutError)
			|| !Lease.Finish())
		{
			if (OutError.GetCode().IsEmpty())
			{
				OutError.Set(TEXT("audio_media_graph_metasound_builder_release_failed"));
			}
			return false;
		}
		return true;
	}

	bool FAudioMediaGraphMetaSoundNativeHook::TryRegisterCompileSettle(
		UMetaSoundSource& Source,
		FAudioMediaGraphError& OutError) const
	{
		OutError.Reset();
		UMetaSoundBuilderBase* const Builder = TryOpenBuilder(Source, OutError);
		if (Builder == nullptr)
		{
			return false;
		}
		FMetaSoundBuilderLease Lease(*Builder);
		if (!TryFinalizeBuilder(Source, *Builder, OutError)
			|| !Lease.Finish())
		{
			if (OutError.GetCode().IsEmpty())
			{
				OutError.Set(TEXT("audio_media_graph_metasound_builder_release_failed"));
			}
			return false;
		}
		return true;
	}

	bool FAudioMediaGraphMetaSoundNativeHook::
	TryMutateCompileAndCaptureOwnedSource(
		UMetaSoundSource& Source,
		FAudioMediaGraphMetaSoundScenarioResult& OutResult,
		FAudioMediaGraphError& OutError) const
	{
		OutResult = FAudioMediaGraphMetaSoundScenarioResult();
		OutError.Reset();
		UMetaSoundEditorSubsystem* Subsystem = GEditor
			? GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>()
			: nullptr;
		IMetaSoundDocumentInterface* DocumentInterface =
			Cast<IMetaSoundDocumentInterface>(&Source);
		if (Subsystem == nullptr || DocumentInterface == nullptr)
		{
			OutError.Set(TEXT("audio_media_graph_metasound_builder_unavailable"));
			return false;
		}

		Subsystem->InitAsset(Source, nullptr, true);
		// The owned automation source is deliberately transient. The editor
		// subsystem's public wrapper requires IsAsset(), while the native
		// registry supports the same transaction-time transient lifecycle.
		UMetaSoundBuilderBase* Builder =
			&Metasound::Engine::FDocumentBuilderRegistry::GetChecked()
				.FindOrBeginBuilding(Source, true);

		EMetaSoundBuilderResult BeforeResult = EMetaSoundBuilderResult::Failed;
		const int32 BeforeCount = Builder->GetGraphInputNames(BeforeResult).Num();
		OutResult.BeforeRevision = FString::Printf(
			TEXT("metasound-inputs:%d"), BeforeCount);

		EMetaSoundBuilderResult AddResult = EMetaSoundBuilderResult::Failed;
		FMetasoundFrontendLiteral DefaultValue;
		DefaultValue.Set(1.0f);
		const FMetaSoundBuilderNodeOutputHandle OutputHandle =
			Builder->AddGraphInputNode(
				TEXT("GC010_Gain"), TEXT("Float"), DefaultValue, AddResult);
		if (AddResult != EMetaSoundBuilderResult::Succeeded
			|| !OutputHandle.NodeID.IsValid()
			|| !OutputHandle.VertexID.IsValid())
		{
			OutError.Set(TEXT("audio_media_graph_metasound_apply_failed"));
			return false;
		}
		OutResult.bAppliedMutation = true;
		OutResult.ApplyEvidenceId = MakeEvidenceId(TEXT("apply"));

		Subsystem->RegisterGraphWithFrontend(Source, true);
		Source.PostEditChange();
		OutResult.bCompileBarrier = true;
		OutResult.bSettled = true;
		OutResult.BarrierEvidenceId = MakeEvidenceId(TEXT("frontend-register"));

		EMetaSoundBuilderResult CaptureResult = EMetaSoundBuilderResult::Failed;
		FName CapturedType;
		FMetaSoundBuilderNodeOutputHandle CapturedOutput;
		const FMetaSoundNodeHandle CapturedNode = Builder->FindGraphInputNode(
			TEXT("GC010_Gain"), CapturedType, CapturedOutput, CaptureResult);
		const TArray<FName> CapturedNames =
			Builder->GetGraphInputNames(CaptureResult);
		if (CaptureResult != EMetaSoundBuilderResult::Succeeded
			|| !CapturedNode.IsSet()
			|| !CapturedOutput.NodeID.IsValid()
			|| !CapturedOutput.VertexID.IsValid()
			|| !CapturedNames.Contains(TEXT("GC010_Gain")))
		{
			OutError.Set(TEXT("audio_media_graph_capture_failed"));
			return false;
		}

		OutResult.bIndependentCapture = true;
		OutResult.NodeGuid = CapturedOutput.NodeID;
		OutResult.VertexGuid = CapturedOutput.VertexID;
		OutResult.PinDirection = TEXT("output");
		OutResult.PinType = CapturedType;
		OutResult.GraphInputNames = CapturedNames;
		OutResult.ReadbackRevision = FString::Printf(
			TEXT("metasound-inputs:%d"), CapturedNames.Num());
		OutResult.ReadbackEvidenceId = MakeEvidenceId(TEXT("frontend-readback"));
		return true;
	}
}
