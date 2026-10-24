#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphMetaSoundNativeHook.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MetasoundFrontendLiteral.h"
#include "UObject/Class.h"

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
		FAudioMediaGraphError MetaSoundFact(const UMetaSoundSource& Source, const FString& Code,
			const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_graph"), Source.GetPathName());
			Error.SourceKind = TEXT("native_result");
			if (Source.IsAsset()) Error.TargetPath = Source.GetPathName();
			Error.PropertyPath = Path;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		TSharedPtr<FJsonValue> MetaSoundFlags(const TCHAR* First, bool bFirst, const TCHAR* Second, bool bSecond)
		{
			auto Facts = MakeShared<FJsonObject>();
			Facts->SetBoolField(First, bFirst);
			Facts->SetBoolField(Second, bSecond);
			return MakeShared<FJsonValueObject>(Facts);
		}

		TSharedPtr<FJsonValue> MetaSoundBuilderValue(EMetaSoundBuilderResult Result)
		{
			auto Facts = MakeShared<FJsonObject>();
			Facts->SetStringField(TEXT("enum_type"), TEXT("EMetaSoundBuilderResult"));
			Facts->SetStringField(TEXT("value"), Result == EMetaSoundBuilderResult::Succeeded ? TEXT("Succeeded") : TEXT("Failed"));
			return MakeShared<FJsonValueObject>(Facts);
		}

		TSharedPtr<FJsonValue> MetaSoundClassValue(const FString& Name, int32 Major, int32 Minor)
		{
			auto Facts = MakeShared<FJsonObject>();
			Facts->SetStringField(TEXT("class_name"), Name);
			Facts->SetNumberField(TEXT("major"), Major);
			Facts->SetNumberField(TEXT("minor"), Minor);
			return MakeShared<FJsonValueObject>(Facts);
		}

		template<typename TValue, typename TEncode>
		void AppendMetaSoundLiteralArray(FJsonObject& Facts, const FMetasoundFrontendLiteral& Literal, TEncode Encode)
		{
			TArray<TValue> Values;
			if (!Literal.TryGet(Values)) return;
			TArray<TSharedPtr<FJsonValue>> Encoded;
			for (const TValue& Value : Values) Encoded.Add(Encode(Value));
			Facts.SetArrayField(TEXT("value"), MoveTemp(Encoded));
		}

		TSharedPtr<FJsonValue> MetaSoundLiteralValue(const FMetasoundFrontendLiteral& Literal)
		{
			auto Facts = MakeShared<FJsonObject>();
			const auto Type = Literal.GetType();
			Facts->SetStringField(TEXT("literal_type"), StaticEnum<EMetasoundFrontendLiteralType>()->GetNameStringByValue(static_cast<int64>(Type)));
			if (Literal.IsArray())
			{
				Facts->SetNumberField(TEXT("element_count"), Literal.GetArrayNum());
				switch (Type)
				{
				case EMetasoundFrontendLiteralType::BooleanArray:
					AppendMetaSoundLiteralArray<bool>(*Facts, Literal, [](bool Value) { return MakeShared<FJsonValueBoolean>(Value); }); break;
				case EMetasoundFrontendLiteralType::IntegerArray:
					AppendMetaSoundLiteralArray<int32>(*Facts, Literal, [](int32 Value) { return MakeShared<FJsonValueNumber>(Value); }); break;
				case EMetasoundFrontendLiteralType::FloatArray:
					AppendMetaSoundLiteralArray<float>(*Facts, Literal, [](float Value) { return MakeShared<FJsonValueNumber>(Value); }); break;
				case EMetasoundFrontendLiteralType::StringArray:
					AppendMetaSoundLiteralArray<FString>(*Facts, Literal, [](const FString& Value) { return MakeShared<FJsonValueString>(Value); }); break;
				case EMetasoundFrontendLiteralType::UObjectArray:
					AppendMetaSoundLiteralArray<UObject*>(*Facts, Literal, [](UObject* Value) -> TSharedPtr<FJsonValue>
					{
						auto Object = MakeShared<FJsonObject>();
						Object->SetBoolField(TEXT("object_present"), Value != nullptr);
						if (Value && Value->IsAsset()) Object->SetStringField(TEXT("asset"), Value->GetPathName());
						return MakeShared<FJsonValueObject>(Object);
					}); break;
				default: break; // NoneArray represents only a default-construction count.
				}
			}
			else if (Type == EMetasoundFrontendLiteralType::Boolean)
			{
				bool Value = false;
				if (Literal.TryGet(Value)) Facts->SetBoolField(TEXT("value"), Value);
			}
			else if (Type == EMetasoundFrontendLiteralType::Integer)
			{
				int32 Value = 0;
				if (Literal.TryGet(Value)) Facts->SetNumberField(TEXT("value"), Value);
			}
			else if (Type == EMetasoundFrontendLiteralType::Float)
			{
				float Value = 0.f;
				if (Literal.TryGet(Value)) Facts->SetNumberField(TEXT("value"), Value);
			}
			else if (Type == EMetasoundFrontendLiteralType::String)
			{
				FString Value;
				if (Literal.TryGet(Value)) Facts->SetStringField(TEXT("value"), Value);
			}
			else if (Type == EMetasoundFrontendLiteralType::UObject)
			{
				UObject* Value = nullptr;
				if (Literal.TryGet(Value))
				{
					Facts->SetBoolField(TEXT("object_present"), Value != nullptr);
					if (Value && Value->IsAsset()) Facts->SetStringField(TEXT("asset"), Value->GetPathName());
				}
			}
			return MakeShared<FJsonValueObject>(Facts);
		}

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
			if (Subsystem == nullptr)
			{
				OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_builder_unavailable"),
					TEXT("The MetaSound editor subsystem is unavailable."), TEXT("/builder/editor_subsystem"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return nullptr;
			}
			if (Cast<IMetaSoundDocumentInterface>(&Source) == nullptr)
			{
				OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_builder_unavailable"),
					TEXT("The source does not expose a MetaSound document interface."), TEXT("/builder/document_interface"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
				OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_builder_unavailable"),
					TEXT("The MetaSound editor subsystem is unavailable during finalization."), TEXT("/builder/editor_subsystem"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
		for (int32 MutationIndex = 0; MutationIndex < Mutations.Num(); ++MutationIndex)
		{
			const auto& Mutation = Mutations[MutationIndex];
			ON_SCOPE_EXIT
			{
				if (OutError.IsValid() && !OutError.OperationIndex.IsSet())
				{
					OutError.OperationIndex = MutationIndex;
				}
			};
			const bool bInput = Mutation.GetInterfaceKind() == TEXT("input");
			if (!bInput && Mutation.GetInterfaceKind() != TEXT("output"))
			{
				OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_interface_invalid"),
					TEXT("The MetaSound interface kind must be input or output."), TEXT("/interface_kind"), MakeShared<FJsonValueString>(Mutation.GetInterfaceKind()), MakeShared<FJsonValueString>(TEXT("input or output")));
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
					OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_interface_missing"),
						TEXT("The MetaSound interface selected for deletion was not found."), TEXT("/interface"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.CandidateName = Mutation.GetName().ToString();
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
					OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_interface_apply_failed"),
						TEXT("The MetaSound builder rejected interface deletion."), TEXT("/interface"), MetaSoundBuilderValue(RemoveResult), MetaSoundBuilderValue(EMetaSoundBuilderResult::Succeeded));
					OutError.CandidateName = Mutation.GetName().ToString();
					return false;
				}
				continue;
			}
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Update)
			{
				if (!bExists || !bInput)
				{
					OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_interface_mismatch"),
						TEXT("Updating an interface default requires an existing MetaSound graph input."), TEXT("/interface"), MetaSoundFlags(TEXT("interface_present"), bExists, TEXT("is_input"), bInput), MetaSoundFlags(TEXT("interface_present"), true, TEXT("is_input"), true));
					OutError.CandidateName = Mutation.GetName().ToString();
					return false;
				}
				EMetaSoundBuilderResult SetResult = EMetaSoundBuilderResult::Failed;
				Builder->SetGraphInputDefault(
					Mutation.GetName(), Mutation.GetDefaultValue(), SetResult);
				if (SetResult != EMetaSoundBuilderResult::Succeeded)
				{
					OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_default_failed"),
						TEXT("The MetaSound builder rejected the graph input default."), TEXT("/interface"), MetaSoundBuilderValue(SetResult), MetaSoundBuilderValue(EMetaSoundBuilderResult::Succeeded));
					OutError.CandidateName = Mutation.GetName().ToString();
					OutError.ProposedValue = MetaSoundLiteralValue(Mutation.GetDefaultValue());
					return false;
				}
				continue;
			}
			if (bExists)
			{
				if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Create)
				{
					OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_interface_exists"),
						TEXT("The MetaSound interface already exists."), TEXT("/interface"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
					OutError.CandidateName = Mutation.GetName().ToString();
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
					OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_interface_apply_failed"),
						TEXT("The MetaSound builder rejected removal of the interface being replaced."), TEXT("/interface"), MetaSoundBuilderValue(RemoveResult), MetaSoundBuilderValue(EMetaSoundBuilderResult::Succeeded));
					OutError.CandidateName = Mutation.GetName().ToString();
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
				OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_interface_apply_failed"),
					TEXT("The MetaSound builder rejected interface creation."), TEXT("/interface"), MetaSoundBuilderValue(AddResult), MetaSoundBuilderValue(EMetaSoundBuilderResult::Succeeded));
				OutError.CandidateName = Mutation.GetName().ToString();
				OutError.ProposedValue = MetaSoundLiteralValue(Mutation.GetDefaultValue());
				return false;
			}
		}
		if (!TryFinalizeBuilder(Source, *Builder, OutError)) return false;
		if (!Lease.Finish())
		{
			OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_builder_release_failed"),
				TEXT("The MetaSound document builder registry did not finish building the source."),
				TEXT("/builder/finish"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
		for (int32 MutationIndex = 0; MutationIndex < Mutations.Num(); ++MutationIndex)
		{
			const auto& Mutation = Mutations[MutationIndex];
			ON_SCOPE_EXIT
			{
				if (OutError.IsValid() && !OutError.OperationIndex.IsSet())
				{
					OutError.OperationIndex = MutationIndex;
				}
			};
			if (!Mutation.GetNodeId().IsValid())
			{
				OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_node_spec_invalid"),
					TEXT("The MetaSound node identifier is not a valid GUID."), TEXT("/node_id"), MakeShared<FJsonValueString>(Mutation.GetNodeId().ToString(EGuidFormats::DigitsWithHyphensLower)), MakeShared<FJsonValueString>(TEXT("valid node GUID")));
				return false;
			}
			const bool bExists = Builder->GetBuilder().FindNode(
				Mutation.GetNodeId()) != nullptr;
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Delete)
			{
				if (!bExists)
				{
					OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_node_missing"),
						TEXT("The selected MetaSound node was not found."), TEXT("/node_id"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.CandidateName = Mutation.GetNodeId().ToString(EGuidFormats::DigitsWithHyphensLower);
					return false;
				}
				EMetaSoundBuilderResult RemoveResult = EMetaSoundBuilderResult::Failed;
				Builder->RemoveNode(
					FMetaSoundNodeHandle(Mutation.GetNodeId()), RemoveResult);
				if (RemoveResult != EMetaSoundBuilderResult::Succeeded)
				{
					OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_node_apply_failed"),
						TEXT("The MetaSound builder rejected node deletion."), TEXT("/node_id"), MetaSoundBuilderValue(RemoveResult), MetaSoundBuilderValue(EMetaSoundBuilderResult::Succeeded));
					OutError.CandidateName = Mutation.GetNodeId().ToString(EGuidFormats::DigitsWithHyphensLower);
					return false;
				}
				continue;
			}

			FMetasoundFrontendClassName ClassName;
			if (!FMetasoundFrontendClassName::Parse(
					Mutation.GetClassName(), ClassName))
			{
				OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_node_spec_invalid"),
					TEXT("The requested MetaSound class name could not be parsed."), TEXT("/class_name"), MakeShared<FJsonValueString>(Mutation.GetClassName()), MakeShared<FJsonValueString>(TEXT("valid MetaSound frontend class name")));
				return false;
			}
			FMetasoundFrontendClass RegisteredClass;
			if (!Metasound::Frontend::ISearchEngine::Get()
					.FindClassWithHighestMinorVersion(
						ClassName, Mutation.GetMajorVersion(), RegisteredClass))
			{
				OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_node_class_missing"),
					TEXT("No registered MetaSound class was found for the requested name and major version."), TEXT("/class"), MakeShared<FJsonValueNull>(), MetaSoundClassValue(Mutation.GetClassName(), Mutation.GetMajorVersion(), Mutation.GetMinorVersion()));
				return false;
			}
			if (RegisteredClass.Metadata.GetVersion().Minor
				!= Mutation.GetMinorVersion())
			{
				OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_node_version_mismatch"),
					TEXT("The registered MetaSound class minor version differs from the requested version."), TEXT("/class"), MetaSoundClassValue(RegisteredClass.Metadata.GetClassName().ToString(), RegisteredClass.Metadata.GetVersion().Major, RegisteredClass.Metadata.GetVersion().Minor), MetaSoundClassValue(Mutation.GetClassName(), Mutation.GetMajorVersion(), Mutation.GetMinorVersion()));
				return false;
			}
			if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Update)
			{
				if (!bExists)
				{
					OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_node_missing"),
						TEXT("The selected MetaSound node was not found."), TEXT("/node_id"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.CandidateName = Mutation.GetNodeId().ToString(EGuidFormats::DigitsWithHyphensLower);
					return false;
				}
			}
			else
			{
				if (bExists)
				{
					if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Create)
					{
						OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_node_exists"),
							TEXT("The MetaSound node already exists."), TEXT("/node_id"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
						OutError.CandidateName = Mutation.GetNodeId().ToString(EGuidFormats::DigitsWithHyphensLower);
						return false;
					}
					EMetaSoundBuilderResult RemoveResult = EMetaSoundBuilderResult::Failed;
					Builder->RemoveNode(
						FMetaSoundNodeHandle(Mutation.GetNodeId()), RemoveResult);
					if (RemoveResult != EMetaSoundBuilderResult::Succeeded)
					{
						OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_node_apply_failed"),
							TEXT("The MetaSound builder rejected removal of the node being replaced."), TEXT("/node_id"), MetaSoundBuilderValue(RemoveResult), MetaSoundBuilderValue(EMetaSoundBuilderResult::Succeeded));
						OutError.CandidateName = Mutation.GetNodeId().ToString(EGuidFormats::DigitsWithHyphensLower);
						return false;
					}
				}
				if (Builder->GetBuilder().AddNodeByClassName(
						ClassName, Mutation.GetMajorVersion(),
						Mutation.GetNodeId()) == nullptr)
				{
					OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_node_class_missing"),
						TEXT("The MetaSound document builder did not create a node of the requested class."), TEXT("/class"), MakeShared<FJsonValueNull>(), MetaSoundClassValue(Mutation.GetClassName(), Mutation.GetMajorVersion(), Mutation.GetMinorVersion()));
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
				OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_node_version_mismatch"),
					TEXT("The resulting MetaSound node class or version differs from the requested class."), TEXT("/class"), NativeClass ? MetaSoundClassValue(NativeClass->Metadata.GetClassName().ToString(), NativeClass->Metadata.GetVersion().Major, NativeClass->Metadata.GetVersion().Minor) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()), MetaSoundClassValue(Mutation.GetClassName(), Mutation.GetMajorVersion(), Mutation.GetMinorVersion()));
				OutError.CandidateName = Mutation.GetNodeId().ToString(EGuidFormats::DigitsWithHyphensLower);
				return false;
			}
			if (Mutation.GetPosition().IsSet())
			{
				EMetaSoundBuilderResult LocationResult =
					EMetaSoundBuilderResult::Failed;
				Builder->SetNodeLocation(
					FMetaSoundNodeHandle(Mutation.GetNodeId()),
					Mutation.GetPosition().GetValue(),
					LocationResult);
				if (LocationResult != EMetaSoundBuilderResult::Succeeded)
				{
					OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_node_location_apply_failed"),
						TEXT("The MetaSound builder rejected the requested node location."), TEXT("/position"), MetaSoundBuilderValue(LocationResult), MetaSoundBuilderValue(EMetaSoundBuilderResult::Succeeded));
					OutError.CandidateName = Mutation.GetNodeId().ToString(EGuidFormats::DigitsWithHyphensLower);
					auto Position = MakeShared<FJsonObject>();
					Position->SetNumberField(TEXT("x"), Mutation.GetPosition().GetValue().X);
					Position->SetNumberField(TEXT("y"), Mutation.GetPosition().GetValue().Y);
					OutError.ProposedValue = MakeShared<FJsonValueObject>(Position);
					return false;
				}
			}
		}
		if (!TryFinalizeBuilder(Source, *Builder, OutError)) return false;
		if (!Lease.Finish())
		{
			OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_builder_release_failed"),
				TEXT("The MetaSound document builder registry did not finish building the source."),
				TEXT("/builder/finish"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
		for (int32 MutationIndex = 0; MutationIndex < Mutations.Num(); ++MutationIndex)
		{
			const auto& Mutation = Mutations[MutationIndex];
			ON_SCOPE_EXIT
			{
				if (OutError.IsValid() && !OutError.OperationIndex.IsSet())
				{
					OutError.OperationIndex = MutationIndex;
				}
			};
			const FMetaSoundBuilderNodeOutputHandle Output(
				Mutation.GetFromNodeId(), Mutation.GetFromVertexId());
			const FMetaSoundBuilderNodeInputHandle Input(
				Mutation.GetToNodeId(), Mutation.GetToVertexId());
			if (!Builder->ContainsNodeOutput(Output))
			{
				OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_link_endpoint_missing"),
					TEXT("The selected MetaSound output vertex was not found."), TEXT("/from"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = Mutation.GetFromNodeId().ToString() + TEXT(":") + Mutation.GetFromVertexId().ToString();
				return false;
			}
			if (!Builder->ContainsNodeInput(Input))
			{
				OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_link_endpoint_missing"),
					TEXT("The selected MetaSound input vertex was not found."), TEXT("/to"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = Mutation.GetToNodeId().ToString() + TEXT(":") + Mutation.GetToVertexId().ToString();
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
				OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_link_apply_failed"),
					TEXT("The MetaSound builder rejected the requested link mutation."), TEXT("/link"), MetaSoundBuilderValue(Result), MetaSoundBuilderValue(EMetaSoundBuilderResult::Succeeded));
				OutError.CandidateName = Mutation.GetFromNodeId().ToString() + TEXT(":") + Mutation.GetFromVertexId().ToString() + TEXT(" -> ") + Mutation.GetToNodeId().ToString() + TEXT(":") + Mutation.GetToVertexId().ToString();
				return false;
			}
		}
		if (!TryFinalizeBuilder(Source, *Builder, OutError)) return false;
		if (!Lease.Finish())
		{
			OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_builder_release_failed"),
				TEXT("The MetaSound document builder registry did not finish building the source."),
				TEXT("/builder/finish"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
			OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_default_endpoint_missing"),
				TEXT("The MetaSound input vertex selected for a default value was not found."), TEXT("/input"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.CandidateName = Mutation.GetNodeId().ToString() + TEXT(":") + Mutation.GetVertexId().ToString();
			return false;
		}
		EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
		Builder->SetNodeInputDefault(Input, Mutation.GetValue(), Result);
		if (Result != EMetaSoundBuilderResult::Succeeded)
		{
			OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_default_failed"),
				TEXT("The MetaSound builder rejected the node input default."), TEXT("/default"), MetaSoundBuilderValue(Result), MetaSoundBuilderValue(EMetaSoundBuilderResult::Succeeded));
			OutError.CandidateName = Mutation.GetNodeId().ToString() + TEXT(":") + Mutation.GetVertexId().ToString();
			OutError.ProposedValue = MetaSoundLiteralValue(Mutation.GetValue());
			return false;
		}
		if (!TryFinalizeBuilder(Source, *Builder, OutError)) return false;
		if (!Lease.Finish())
		{
			OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_builder_release_failed"),
				TEXT("The MetaSound document builder registry did not finish building the source."),
				TEXT("/builder/finish"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
		if (!TryFinalizeBuilder(Source, *Builder, OutError)) return false;
		if (!Lease.Finish())
		{
			OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_builder_release_failed"),
				TEXT("The MetaSound document builder registry did not finish building the source."),
				TEXT("/builder/finish"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
			OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_builder_unavailable"),
				TEXT("The MetaSound editor subsystem or document interface is unavailable."), TEXT("/builder"), MetaSoundFlags(TEXT("editor_subsystem_present"), Subsystem != nullptr, TEXT("document_interface_present"), DocumentInterface != nullptr), MetaSoundFlags(TEXT("editor_subsystem_present"), true, TEXT("document_interface_present"), true));
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
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetField(TEXT("builder_result"), MetaSoundBuilderValue(AddResult));
			Actual->SetBoolField(TEXT("node_id_valid"), OutputHandle.NodeID.IsValid());
			Actual->SetBoolField(TEXT("vertex_id_valid"), OutputHandle.VertexID.IsValid());
			OutError = MetaSoundFact(Source, TEXT("audio_media_graph_metasound_apply_failed"),
				TEXT("Creating the MetaSound graph input failed or returned invalid node/vertex identifiers."), TEXT("/interface"),
				MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("builder Succeeded with valid node and vertex identifiers")));
			OutError.CandidateName = TEXT("GC010_Gain");
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
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetField(TEXT("builder_result"), MetaSoundBuilderValue(CaptureResult));
			Actual->SetBoolField(TEXT("node_present"), CapturedNode.IsSet());
			Actual->SetBoolField(TEXT("node_id_valid"), CapturedOutput.NodeID.IsValid());
			Actual->SetBoolField(TEXT("vertex_id_valid"), CapturedOutput.VertexID.IsValid());
			Actual->SetBoolField(TEXT("input_name_present"), CapturedNames.Contains(TEXT("GC010_Gain")));
			OutError = MetaSoundFact(Source, TEXT("audio_media_graph_capture_failed"),
				TEXT("MetaSound readback did not return the created graph input with a valid node and output vertex."), TEXT("/capture"),
				MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("builder Succeeded with the named input, a node and valid node/vertex identifiers")));
			OutError.CandidateName = TEXT("GC010_Gain");
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
