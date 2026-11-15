#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaBindings.h"
#include "Misc/ScopeExit.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringPackageReadback.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaIdentity.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringDocuments.h"
#include "Domains/Audio/Authoring/UeAgentAudioGraphIntent.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/AssetDurable/UeAgentPackageAssetRevision.h"
#include "Core/AssetDurable/UeAgentPackageHeader.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemanticEvidence.h"
#include "Core/Verification/UeAgentIndependentReadback.h"
#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphMetaSoundNativeHook.h"
#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphSoundCueNativeHook.h"
#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphTextureGraphNativeHook.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Dom/JsonObject.h"
#include "Expressions/Output/TG_Expression_Output.h"
#include "HAL/FileManager.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundDocumentBuilderRegistry.h"
#include "MetasoundBuilderBase.h"
#include "MetasoundFactory.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundSource.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "TextureGraph.h"
#include "TG_Graph.h"
#include "TG_Node.h"
#include "TG_Pin.h"
#include "UObject/Package.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace AssetDurable;
	using namespace BindingGateway::Operation;
	using namespace ContextResource;
	using namespace Contract;
	using namespace DomainAdapters::AudioMediaGraph;
	using namespace PlanningPreflight;
	using namespace StateSemantic;
	using namespace Verification;

	namespace
	{
        void SetError(FGraphCompoundRuntimeError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic,
            FString SubjectId = FString(), const UE::UAI::Contract::FDiagnostic& Recovery = UE::UAI::Contract::FDiagnostic())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = MoveTemp(SubjectId);
            OutError.Append(Recovery);
        }

		const FString ReadbackOracleId =
			TEXT("uai.readback.graph_compound.audio_media");

		enum class EAudioMediaReadbackKind : uint8
		{
			AssetCreate,
			MetaSoundInterface,
			MetaSoundNodes,
			MetaSoundLinks,
			MetaSoundDefault,
			MetaSoundCompileSettle,
			SoundCueNodes,
			SoundCueRelations,
			SoundCueRoot,
			SoundCueCompileSettle,
			TextureGraphNodes,
			TextureGraphLinks,
			TextureGraphParameters,
			TextureGraphValidateEvaluateSettle,
			TextureGraphSemanticComposite,
		};

		struct FAudioMediaReadbackSpec final
		{
			EAudioMediaReadbackKind Kind = EAudioMediaReadbackKind::AssetCreate;
			FString AssetKind;
			FString NativeClassRef;
			FString ExpectedRevision;
			TArray<FAudioMediaGraphMetaSoundInterfaceMutation> MetaInterfaces;
			TArray<FAudioMediaGraphMetaSoundNodeMutation> MetaNodes;
			TArray<FAudioMediaGraphMetaSoundLinkMutation> MetaLinks;
			TOptional<FAudioMediaGraphMetaSoundDefaultMutation> MetaDefault;
			TSharedPtr<const FJsonObject> FrozenMetaDefault;
			TArray<FAudioMediaGraphSoundCueNodeMutation> SoundNodes;
			TArray<FAudioMediaGraphSoundCueRelationMutation> SoundRelations;
			FString SoundRoot;
			TArray<FAudioMediaGraphTextureGraphNodeMutation> TextureNodes;
			TArray<FAudioMediaGraphTextureGraphLinkMutation> TextureLinks;
			TArray<FAudioMediaGraphTextureGraphParameterMutation> TextureParameters;
		};

		

		FString TargetPackageName(const FString& TargetRef)
		{
			return FPackageName::ObjectPathToPackageName(TargetRef);
		}

		FString TargetAssetName(const FString& TargetRef)
		{
			return FPackageName::GetLongPackageAssetName(
				TargetPackageName(TargetRef));
		}

		FString TargetObjectPath(const FString& TargetRef)
		{
			const FString PackageName = TargetPackageName(TargetRef);
			return PackageName + TEXT(".")
				+ FPackageName::GetLongPackageAssetName(PackageName);
		}


		FAssetDurableSessionRef PersistedSession(
			const FAssetDurableSessionRef& Prepared,
			const FDurableReceipt& Receipt)
		{
			return MakeShared<const FAssetDurableSession>(
				Prepared->GetRequest(), Prepared->GetInventory(),
				Prepared->GetJournalId(), Prepared->GetJournalPath(),
				Prepared->GetJournalRoot(), Prepared->GetTransactionRoot(),
				Prepared->GetStagingRoot(), Receipt.GetPhaseHistory());
		}

		bool TryRollback(
			const FString& TargetRef,
			const bool bNotifyAssetRegistry,
			const FAssetDurableSessionRef& Session,
			FPackageAssetDurableBackend& Backend,
			const int64 FencingToken,
			FUeAgentResourceTracker& Resources,
			UE::UAI::Contract::FDiagnostic& OutDetail)
		{
			return UeAgentAuthoringOperationApply::TryRestorePackageDurability(
				TargetRef, bNotifyAssetRegistry, Session, Backend, FencingToken,
				Resources, OutDetail);
		}

		const FUeAgentCanonicalValue* FindField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object.GetObjectItems())
			{
				if (Item.Key == Name)
				{
					return &Item.Value.Get();
				}
			}
			return nullptr;
		}

		UE::UAI::Contract::FDiagnostic AudioMediaFailure(const TCHAR* Code, const FString& Reason,
			const FString& Target, const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("graph_compound"));
			Error.TargetPath = Target;
			Error.PropertyPath = Field;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			MarkGraphCompoundNativeDiagnostic(Error);
			return Error;
		}




		class FReadbackInput final
		{
		public:
			FReadbackInput(const FGraphCompoundAudioMediaOperationRequest& InRequest,
				const FGraphCompoundCanonicalEdit& InEdit, const TCHAR* InCode, FGraphCompoundRuntimeError& InError,
				FString InPrefix = FString())
				: Request(InRequest), Edit(InEdit), Code(InCode), Error(InError), Prefix(MoveTemp(InPrefix))
			{}

			FReadbackInput At(const FString& Child) const
			{
				return FReadbackInput(Request, Edit, Code, Error, Prefix + Child);
			}

			bool Fail(const FString& Reason, const FString& Field,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const bool bNative = false) const
			{
				Error = AudioMediaFailure(Code, Reason, Request.GetTargetRef(), Prefix + Field,
					MoveTemp(Actual), MoveTemp(Expected));
				Error.SubjectId = Edit.GetIdentity().GetSemanticId();
				Error.OperationIndex = static_cast<int32>(&Edit - Request.GetPayload().GetEdits().GetData());
				const FString* Pointer = Request.GetPayload().FindInputPointer(Edit.GetIdentity().GetSemanticId());
				if (!bNative && Pointer && !Pointer->IsEmpty())
				{
					Error.SourceKind.Reset();
					Error.Source.JsonPath = (Prefix + Field).IsEmpty() ? TEXT("/") : Prefix + Field;
					BindGraphCompoundInputValueDiagnostic(Error, *Pointer);
				}
				return false;
			}

			bool Forward(UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Field,
				const FString& ValueRoot = TEXT("/")) const
			{
				Error = MoveTemp(Diagnostic);
				if (Error.TargetPath.IsEmpty()) Error.TargetPath = Request.GetTargetRef();
				if (Error.SubjectId.IsEmpty()) Error.SubjectId = Edit.GetIdentity().GetSemanticId();
				if (!Error.OperationIndex.IsSet())
					Error.OperationIndex = static_cast<int32>(&Edit - Request.GetPayload().GetEdits().GetData());
				const FString* Pointer = Request.GetPayload().FindInputPointer(Edit.GetIdentity().GetSemanticId());
				if (!Pointer || Pointer->IsEmpty()) MarkGraphCompoundNativeDiagnostic(Error);
				else
				{
					const FString Root = (*Pointer == TEXT("/") ? FString() : *Pointer) + Prefix + Field;
					BindGraphCompoundInputValueDiagnostic(Error, Root.IsEmpty() ? TEXT("/") : Root, ValueRoot);
				}
				return false;
			}

			bool RequireValue(const FUeAgentCanonicalValuePtr& Value) const
			{
				return Value.IsValid() || Fail(TEXT("The canonical edit has no proposed value."),
					FString(), nullptr, MakeShared<FJsonValueString>(TEXT("proposed value")), true);
			}

			bool RequireEndpoint(const FUeAgentCanonicalValue* Value, const TCHAR* Name) const
			{
				return Value || Fail(TEXT("The graph link endpoint is missing."),
					FieldPointer(Name), nullptr, MakeShared<FJsonValueString>(TEXT("endpoint object")));
			}

			bool ReadString(const FUeAgentCanonicalValue& Object, const FString& Name, FString& OutValue,
				const bool bNative = false) const
			{
				if (Object.GetKind() != EUeAgentValueKind::Object)
					return Fail(TEXT("The edit value containing the required field must be an object."),
						FString(), CanonicalInputFact(&Object), MakeShared<FJsonValueString>(TEXT("object")), bNative);
				const FUeAgentCanonicalValue* Value = FindField(Object, Name);
				if (!Value || Value->GetKind() != EUeAgentValueKind::String)
					return Fail(Value ? TEXT("The field must be a string.") : TEXT("The required field is missing."),
						FieldPointer(Name), CanonicalInputFact(Value), MakeShared<FJsonValueString>(TEXT("non-empty string")), bNative);
				OutValue = Value->GetText();
				return !OutValue.IsEmpty() || Fail(TEXT("The string field must not be empty."),
					FieldPointer(Name), CanonicalInputFact(Value), MakeShared<FJsonValueString>(TEXT("non-empty string")), bNative);
			}

			bool ReadInteger(const FUeAgentCanonicalValue& Object, const FString& Name, int32& OutValue) const
			{
				const FUeAgentCanonicalValue* Value = FindField(Object, Name);
				const auto Expected = MakeShared<FJsonValueString>(
					FString::Printf(TEXT("integer in [%d, %d]"), MIN_int32, MAX_int32));
				if (!Value || (Value->GetKind() != EUeAgentValueKind::Integer && Value->GetKind() != EUeAgentValueKind::Decimal))
					return Fail(Value ? TEXT("The field must be numeric.") : TEXT("The required field is missing."),
						FieldPointer(Name), CanonicalInputFact(Value), Expected);
				int64 Number = 0;
				if (!TryParseInt64Number(Value->GetLexeme(), Number) || Number < MIN_int32 || Number > MAX_int32)
					return Fail(TEXT("The field is not an integer within the signed 32-bit range."),
						FieldPointer(Name), CanonicalInputFact(Value), Expected);
				OutValue = static_cast<int32>(Number);
				return true;
			}

			bool ReadNumber(const FUeAgentCanonicalValue* Value, const FString& Field, double& OutValue) const
			{
				if (!Value || (Value->GetKind() != EUeAgentValueKind::Integer && Value->GetKind() != EUeAgentValueKind::Decimal))
					return Fail(Value ? TEXT("The value must be numeric.") : TEXT("The required number is missing."),
						Field, CanonicalInputFact(Value), MakeShared<FJsonValueString>(TEXT("finite number")));
				return TryParseFiniteDecimal(Value->GetLexeme(), OutValue)
					|| Fail(TEXT("The number is not finite or cannot be represented."),
						Field, CanonicalInputFact(Value), MakeShared<FJsonValueString>(TEXT("finite number")));
			}

			bool ReadOptionalPosition(const FUeAgentCanonicalValue& Object, TOptional<FVector2D>& OutPosition) const
			{
				OutPosition.Reset();
				const FUeAgentCanonicalValue* Position = FindField(Object, TEXT("position"));
				if (!Position) return true;
				double X = 0.0;
				double Y = 0.0;
				if (Position->GetKind() != EUeAgentValueKind::Object)
					return Fail(TEXT("The optional position must be an object."), TEXT("/position"),
						CanonicalInputFact(Position), MakeShared<FJsonValueString>(TEXT("object with finite x and y")));
				if (!ReadNumber(FindField(*Position, TEXT("x")), TEXT("/position/x"), X)
					|| !ReadNumber(FindField(*Position, TEXT("y")), TEXT("/position/y"), Y)) return false;
				OutPosition = FVector2D(X, Y);
				return true;
			}

			bool ReadGuid(const FUeAgentCanonicalValue& Object, const FString& Name, FGuid& OutValue) const
			{
				FString Text;
				if (!ReadString(Object, Name, Text)) return false;
				return (FGuid::Parse(Text, OutValue) && OutValue.IsValid())
					|| Fail(TEXT("The field is not a valid non-zero GUID."), FieldPointer(Name),
						MakeShared<FJsonValueString>(Text), MakeShared<FJsonValueString>(TEXT("valid non-zero GUID")));
			}

			bool ReadIdentitySelector(const FString& Name, FString& OutValue) const
			{
				OutValue = Edit.GetIdentity().GetSelector(Name);
				return !OutValue.IsEmpty() || Fail(TEXT("The canonical edit identity is missing a required selector."),
					TEXT("/identity/selectors") + FieldPointer(Name), MakeShared<FJsonValueString>(OutValue),
					MakeShared<FJsonValueString>(TEXT("non-empty selector")), true);
			}

			bool ReadIdentityGuid(const FString& Name, FGuid& OutValue) const
			{
				FString Text;
				if (!ReadIdentitySelector(Name, Text)) return false;
				return FGuid::Parse(Text, OutValue)
					|| Fail(TEXT("The canonical edit identity selector cannot be parsed as a GUID."),
						TEXT("/identity/selectors") + FieldPointer(Name), MakeShared<FJsonValueString>(Text),
						MakeShared<FJsonValueString>(TEXT("parseable GUID")), true);
			}

			bool ReadMetaSoundLiteral(const FUeAgentCanonicalValue* Value, const FString& DataType,
				FMetasoundFrontendLiteral& OutLiteral, const FString& Field) const
			{
				FString LiteralType;
				TSharedPtr<FJsonValue> JsonValue;
				if (DataType == TEXT("Float") || DataType == TEXT("Int32"))
				{
					LiteralType = DataType == TEXT("Float") ? TEXT("float") : TEXT("int");
					JsonValue = MakeShared<FJsonValueNumber>(0);
				}
				else if (DataType == TEXT("Bool"))
				{
					LiteralType = TEXT("bool");
					JsonValue = MakeShared<FJsonValueBoolean>(false);
				}
				else if (DataType == TEXT("String"))
				{
					LiteralType = TEXT("string");
					JsonValue = MakeShared<FJsonValueString>(FString());
				}
				else
					return Fail(TEXT("The MetaSound literal data type is not supported by this input reader."),
						TEXT("/data_type_ref"), MakeShared<FJsonValueString>(DataType),
						MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
							MakeShared<FJsonValueString>(TEXT("Float")), MakeShared<FJsonValueString>(TEXT("Int32")),
							MakeShared<FJsonValueString>(TEXT("Bool")), MakeShared<FJsonValueString>(TEXT("String"))}));
				UE::UAI::Contract::FDiagnostic Detail;
				if (Value && !CanonicalJson::TryToJsonValue(*Value, JsonValue, Detail))
					return Forward(MoveTemp(Detail), Field);
				const TSharedRef<FJsonObject> Literal = MakeShared<FJsonObject>();
				Literal->SetStringField(TEXT("type"), LiteralType);
				Literal->SetField(TEXT("value"), JsonValue);
				if (!UeAgentAudioAuthoring::DecodeMetaSoundLiteral(Literal, OutLiteral, Detail))
				{
					// Only the literal value is authored. The literal type/storage wrapper is generated.
					if (!Value || (Detail.Source.JsonPath != TEXT("/value")
						&& !Detail.Source.JsonPath.StartsWith(TEXT("/value/"))))
						MarkGraphCompoundNativeDiagnostic(Detail);
					return Forward(MoveTemp(Detail), Field, TEXT("/value"));
				}
				return true;
			}

			bool ReadTextureGraphValue(const FUeAgentCanonicalValue* Value,
				FAudioMediaGraphTextureGraphValue& OutValue, const FString& Field) const
			{
				if (!Value)
					return Fail(TEXT("The TextureGraph parameter value is missing."), Field, nullptr,
						MakeShared<FJsonValueString>(TEXT("number, boolean, string or four-number array")));
				if (Value->GetKind() == EUeAgentValueKind::Integer || Value->GetKind() == EUeAgentValueKind::Decimal)
				{
					TSharedPtr<FJsonValue> Number;
					UE::UAI::Contract::FDiagnostic Detail;
					if (!StateSemantic::CanonicalJson::TryToJsonValue(*Value, Number, Detail))
						return Forward(MoveTemp(Detail), Field);
					OutValue = FAudioMediaGraphTextureGraphValue::Number(MoveTemp(Number));
					return true;
				}
				if (Value->GetKind() == EUeAgentValueKind::Boolean)
				{
					OutValue = FAudioMediaGraphTextureGraphValue::Boolean(Value->GetBoolean());
					return true;
				}
				if (Value->GetKind() == EUeAgentValueKind::String)
				{
					OutValue = FAudioMediaGraphTextureGraphValue::String(Value->GetText());
					return true;
				}
				if (Value->GetKind() != EUeAgentValueKind::Array)
					return Fail(TEXT("The TextureGraph parameter value has an unsupported kind."), Field,
						CanonicalInputFact(Value), MakeShared<FJsonValueString>(TEXT("number, boolean, string or four-number array")));
				TArray<double> Numbers;
				for (int32 Index = 0; Index < Value->GetArrayItems().Num(); ++Index)
				{
					double Number = 0.0;
					if (!ReadNumber(&Value->GetArrayItems()[Index].Get(),
							Field + FString::Printf(TEXT("/%d"), Index), Number)) return false;
					Numbers.Add(Number);
				}
				if (Numbers.Num() != 4)
					return Fail(TEXT("The TextureGraph parameter array must contain exactly four numbers."),
						Field, MakeShared<FJsonValueNumber>(Numbers.Num()), MakeShared<FJsonValueNumber>(4));
				OutValue = FAudioMediaGraphTextureGraphValue::NumberArray(MoveTemp(Numbers));
				return true;
			}

		private:
			static FString FieldPointer(FString Field)
			{
				Field.ReplaceInline(TEXT("~"), TEXT("~0"));
				Field.ReplaceInline(TEXT("/"), TEXT("~1"));
				return TEXT("/") + Field;
			}
			const FGraphCompoundAudioMediaOperationRequest& Request;
			const FGraphCompoundCanonicalEdit& Edit;
			const TCHAR* Code;
			FGraphCompoundRuntimeError& Error;
			FString Prefix;
		};

		bool TryCanonicalObject(
			const TSharedRef<FJsonObject>& Object,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError)
		{
			UE::UAI::Contract::FDiagnostic Error;
			if (!CanonicalJson::TryFromJsonValue(
					MakeShared<FJsonValueObject>(Object),
					Resources, OutValue, Error))
			{
				SetError(OutError, Error, FString());
				MarkGraphCompoundNativeDiagnostic(OutError);
				return false;
			}
			return true;
		}

		EAudioMediaGraphMutationIntent NativeIntent(const EChangeIntent Intent)
		{
			switch (Intent)
			{
			case EChangeIntent::Create:
				return EAudioMediaGraphMutationIntent::Create;
			case EChangeIntent::Update:
				return EAudioMediaGraphMutationIntent::Update;
			case EChangeIntent::Replace:
				return EAudioMediaGraphMutationIntent::Replace;
			case EChangeIntent::Delete:
				return EAudioMediaGraphMutationIntent::Delete;
			}
			return EAudioMediaGraphMutationIntent::Update;
		}


		bool TryParseReadbackSpec(
			const FAudioMediaGraphRouteProfile& Profile,
			const FGraphCompoundAudioMediaOperationRequest& Request,
			FAudioMediaReadbackSpec& OutSpec,
			FGraphCompoundRuntimeError& OutError)
		{
			OutSpec = {};
			const TArray<FGraphCompoundCanonicalEdit>& Edits =
				Request.GetPayload().GetEdits();
			if (Edits.IsEmpty())
			{
				OutError = AudioMediaFailure(TEXT("graph_compound_audio_media_payload_invalid"),
					TEXT("The audio/media operation payload contains no edits."), Request.GetTargetRef(),
					TEXT("edits.count"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one edit")));
				OutError.SubjectId = Profile.GetCapabilityId();
				return false;
			}
			if (Profile.GetInputKind() == EAudioMediaGraphInputKind::AssetCreate)
			{
				OutSpec.Kind = EAudioMediaReadbackKind::AssetCreate;
				const FReadbackInput Input(Request, Edits[0], TEXT("graph_compound_audio_media_asset_spec_invalid"), OutError);
				const FUeAgentCanonicalValuePtr& Value = Edits[0].GetProposedAfter();
				if (!Input.RequireValue(Value)
					|| !Input.ReadString(*Value, TEXT("asset_kind"), OutSpec.AssetKind)
					|| !Input.ReadString(*Value, TEXT("native_class_ref"),
						OutSpec.NativeClassRef, true))
				{
					return false;
				}
				return true;
			}

			switch (Profile.GetInputKind())
			{
			case EAudioMediaGraphInputKind::MetaSoundInterface:
				OutSpec.Kind = EAudioMediaReadbackKind::MetaSoundInterface;
				for (const FGraphCompoundCanonicalEdit& Edit : Edits)
				{
					const FReadbackInput Input(Request, Edit, TEXT("graph_compound_audio_media_metasound_interface_spec_invalid"), OutError);
					if (Edit.GetIntent() == EChangeIntent::Delete)
					{
						const FReadbackInput DeleteInput(Request, Edit, TEXT("graph_compound_audio_media_metasound_interface_delete_selector_invalid"), OutError);
						FString InterfaceKind;
						FString Name;
						if (!DeleteInput.ReadIdentitySelector(TEXT("interface_kind"), InterfaceKind)
							|| !DeleteInput.ReadIdentitySelector(TEXT("name"), Name))
						{
							return false;
						}
						OutSpec.MetaInterfaces.Emplace(
							MoveTemp(InterfaceKind),
							FName(*Name),
							NAME_None,
							false,
							FMetasoundFrontendLiteral(),
							EAudioMediaGraphMutationIntent::Delete);
						continue;
					}
					const FUeAgentCanonicalValuePtr& Value = Edit.GetProposedAfter();
					FString InterfaceKind;
					FString Name;
					FString DataType;
					FString AccessType;
					FMetasoundFrontendLiteral Default;
					if (!Input.RequireValue(Value)
						|| !Input.ReadString(*Value, TEXT("interface_kind"), InterfaceKind)
						|| !Input.ReadString(*Value, TEXT("name"), Name)
						|| !Input.ReadString(*Value, TEXT("data_type_ref"), DataType)
						|| !Input.ReadString(*Value, TEXT("access_type"), AccessType)
						|| !Input.ReadMetaSoundLiteral(
							FindField(*Value, TEXT("default_value")), DataType, Default, TEXT("/default_value")))
					{
						return false;
					}
					OutSpec.MetaInterfaces.Emplace(
						MoveTemp(InterfaceKind), FName(*Name), FName(*DataType),
						AccessType == TEXT("value"), MoveTemp(Default),
						NativeIntent(Edit.GetIntent()));
				}
				return true;
			case EAudioMediaGraphInputKind::MetaSoundNode:
				OutSpec.Kind = EAudioMediaReadbackKind::MetaSoundNodes;
				for (const FGraphCompoundCanonicalEdit& Edit : Edits)
				{
					const FReadbackInput Input(Request, Edit, TEXT("graph_compound_audio_media_metasound_node_spec_invalid"), OutError);
					if (Edit.GetIntent() == EChangeIntent::Delete)
					{
						const FReadbackInput DeleteInput(Request, Edit, TEXT("graph_compound_audio_media_metasound_node_delete_selector_invalid"), OutError);
						FGuid NodeId;
						if (!DeleteInput.ReadIdentityGuid(TEXT("node_guid"), NodeId))
						{
							return false;
						}
						OutSpec.MetaNodes.Emplace(
							NodeId,
							FString(),
							0,
							0,
							EAudioMediaGraphMutationIntent::Delete);
						continue;
					}
					const FUeAgentCanonicalValuePtr& Value = Edit.GetProposedAfter();
					FGuid NodeId;
					FString ClassName;
					int32 Major = 0;
					int32 Minor = 0;
					TOptional<FVector2D> Position;
					if (!Input.RequireValue(Value)
						|| !Input.ReadGuid(*Value, TEXT("node_guid"), NodeId)
						|| !Input.ReadString(*Value, TEXT("class_name"), ClassName)
						|| !Input.ReadInteger(*Value, TEXT("major_version"), Major)
						|| !Input.ReadInteger(*Value, TEXT("minor_version"), Minor)
						|| !Input.ReadOptionalPosition(*Value, Position))
					{
						return false;
					}
					OutSpec.MetaNodes.Emplace(
						NodeId, MoveTemp(ClassName), Major, Minor,
						NativeIntent(Edit.GetIntent()), MoveTemp(Position));
				}
				return true;
			case EAudioMediaGraphInputKind::MetaSoundLink:
				OutSpec.Kind = EAudioMediaReadbackKind::MetaSoundLinks;
				for (const FGraphCompoundCanonicalEdit& Edit : Edits)
				{
					const FReadbackInput Input(Request, Edit, TEXT("graph_compound_audio_media_metasound_link_spec_invalid"), OutError);
					if (Edit.GetIntent() == EChangeIntent::Delete)
					{
						const FReadbackInput DeleteInput(Request, Edit, TEXT("graph_compound_audio_media_metasound_link_delete_selector_invalid"), OutError);
						FGuid FromNode;
						FGuid FromVertex;
						FGuid ToNode;
						FGuid ToVertex;
						if (!DeleteInput.ReadIdentityGuid(TEXT("from_node_guid"), FromNode)
							|| !DeleteInput.ReadIdentityGuid(TEXT("from_vertex_guid"), FromVertex)
							|| !DeleteInput.ReadIdentityGuid(TEXT("to_node_guid"), ToNode)
							|| !DeleteInput.ReadIdentityGuid(TEXT("to_vertex_guid"), ToVertex))
						{
							return false;
						}
						OutSpec.MetaLinks.Emplace(
							FromNode,
							FromVertex,
							ToNode,
							ToVertex,
							EAudioMediaGraphMutationIntent::Delete);
						continue;
					}
					const FUeAgentCanonicalValuePtr& Value = Edit.GetProposedAfter();
					const FUeAgentCanonicalValue* const From = Value.IsValid()
						? FindField(*Value, TEXT("from")) : nullptr;
					const FUeAgentCanonicalValue* const To = Value.IsValid()
						? FindField(*Value, TEXT("to")) : nullptr;
					FGuid FromNode;
					FGuid FromVertex;
					FGuid ToNode;
					FGuid ToVertex;
					if (!Input.RequireValue(Value)
						|| !Input.RequireEndpoint(From, TEXT("from")) || !Input.RequireEndpoint(To, TEXT("to"))
						|| !Input.At(TEXT("/from")).ReadGuid(*From, TEXT("node_guid"), FromNode)
						|| !Input.At(TEXT("/from")).ReadGuid(*From, TEXT("vertex_guid"), FromVertex)
						|| !Input.At(TEXT("/to")).ReadGuid(*To, TEXT("node_guid"), ToNode)
						|| !Input.At(TEXT("/to")).ReadGuid(*To, TEXT("vertex_guid"), ToVertex))
					{
						return false;
					}
					OutSpec.MetaLinks.Emplace(
						FromNode, FromVertex, ToNode, ToVertex,
						NativeIntent(Edit.GetIntent()));
				}
				return true;
			case EAudioMediaGraphInputKind::MetaSoundDefault:
			{
				OutSpec.Kind = EAudioMediaReadbackKind::MetaSoundDefault;
				const FReadbackInput Input(Request, Edits[0], TEXT("graph_compound_audio_media_metasound_default_spec_invalid"), OutError);
				const FUeAgentCanonicalValuePtr& Value = Edits[0].GetProposedAfter();
				FGuid NodeId;
				FGuid VertexId;
				FString DataType;
				FMetasoundFrontendLiteral Literal;
				if (!Input.RequireValue(Value)
					|| !Input.ReadGuid(*Value, TEXT("node_guid"), NodeId)
					|| !Input.ReadGuid(*Value, TEXT("vertex_guid"), VertexId)
					|| !Input.ReadString(*Value, TEXT("data_type_ref"), DataType)
					|| !Input.ReadMetaSoundLiteral(
						FindField(*Value, TEXT("value")), DataType, Literal, TEXT("/value")))
				{
					return false;
				}
				OutSpec.FrozenMetaDefault = UeAgentAudioAuthoring::EncodeMetaSoundLiteral(Literal);
				OutSpec.MetaDefault.Emplace(NodeId, VertexId, MoveTemp(Literal));
				return true;
			}
			case EAudioMediaGraphInputKind::MetaSoundCompileSettle:
				OutSpec.Kind = EAudioMediaReadbackKind::MetaSoundCompileSettle;
				if (!FReadbackInput(Request, Edits[0], TEXT("graph_compound_audio_media_metasound_settle_spec_invalid"), OutError)
					.RequireValue(Edits[0].GetProposedAfter()))
				{
					return false;
				}
				return true;
			case EAudioMediaGraphInputKind::SoundCueNode:
				OutSpec.Kind = EAudioMediaReadbackKind::SoundCueNodes;
				for (const FGraphCompoundCanonicalEdit& Edit : Edits)
				{
					const FReadbackInput Input(Request, Edit, TEXT("graph_compound_audio_media_sound_node_spec_invalid"), OutError);
					if (Edit.GetIntent() == EChangeIntent::Delete)
					{
						const FReadbackInput DeleteInput(Request, Edit, TEXT("graph_compound_audio_media_sound_node_delete_selector_invalid"), OutError);
						FString ObjectPath;
						if (!DeleteInput.ReadIdentitySelector(TEXT("object_path"), ObjectPath))
						{
							return false;
						}
						OutSpec.SoundNodes.Emplace(
							MoveTemp(ObjectPath),
							FString(),
							EAudioMediaGraphMutationIntent::Delete);
						continue;
					}
					FString ObjectPath;
					FString ClassRef;
					const FUeAgentCanonicalValuePtr& Value = Edit.GetProposedAfter();
					if (!Input.RequireValue(Value)
						|| !Input.ReadString(*Value, TEXT("object_path"), ObjectPath)
						|| !Input.ReadString(*Value, TEXT("native_class_ref"), ClassRef))
					{
						return false;
					}
					OutSpec.SoundNodes.Emplace(
						MoveTemp(ObjectPath), MoveTemp(ClassRef),
						NativeIntent(Edit.GetIntent()));
				}
				return true;
			case EAudioMediaGraphInputKind::SoundCueRelation:
				OutSpec.Kind = EAudioMediaReadbackKind::SoundCueRelations;
				for (const FGraphCompoundCanonicalEdit& Edit : Edits)
				{
					const FReadbackInput Input(Request, Edit, TEXT("graph_compound_audio_media_sound_relation_spec_invalid"), OutError);
					if (Edit.GetIntent() == EChangeIntent::Delete)
					{
						const FReadbackInput DeleteInput(Request, Edit, TEXT("graph_compound_audio_media_sound_relation_delete_selector_invalid"), OutError);
						FString Parent;
						FString Child;
						if (!DeleteInput.ReadIdentitySelector(TEXT("parent_object_path"), Parent)
							|| !DeleteInput.ReadIdentitySelector(TEXT("child_object_path"), Child))
						{
							return false;
						}
						OutSpec.SoundRelations.Emplace(
							MoveTemp(Parent),
							MoveTemp(Child),
							INDEX_NONE,
							EAudioMediaGraphMutationIntent::Delete);
						continue;
					}
					FString Parent;
					FString Child;
					int32 Order = INDEX_NONE;
					const FUeAgentCanonicalValuePtr& Value = Edit.GetProposedAfter();
					if (!Input.RequireValue(Value)
						|| !Input.ReadString(*Value, TEXT("parent_object_path"), Parent)
						|| !Input.ReadString(*Value, TEXT("child_object_path"), Child)
						|| !Input.ReadInteger(*Value, TEXT("order_index"), Order))
					{
						return false;
					}
					OutSpec.SoundRelations.Emplace(
						MoveTemp(Parent), MoveTemp(Child), Order,
						NativeIntent(Edit.GetIntent()));
				}
				return true;
			case EAudioMediaGraphInputKind::SoundCueRoot:
			{
				OutSpec.Kind = EAudioMediaReadbackKind::SoundCueRoot;
				const FReadbackInput Input(Request, Edits[0], TEXT("graph_compound_audio_media_sound_root_spec_invalid"), OutError);
				if (!Input.RequireValue(Edits[0].GetProposedAfter())
					|| !Input.ReadString(*Edits[0].GetProposedAfter(),
						TEXT("root_node_object_path"), OutSpec.SoundRoot)) return false;
				return true;
			}
			case EAudioMediaGraphInputKind::SoundCueCompileSettle:
				OutSpec.Kind = EAudioMediaReadbackKind::SoundCueCompileSettle;
				if (!FReadbackInput(Request, Edits[0], TEXT("graph_compound_audio_media_sound_settle_spec_invalid"), OutError)
					.RequireValue(Edits[0].GetProposedAfter()))
				{
					return false;
				}
				return true;
			case EAudioMediaGraphInputKind::TextureGraphNode:
				OutSpec.Kind = EAudioMediaReadbackKind::TextureGraphNodes;
				for (const FGraphCompoundCanonicalEdit& Edit : Edits)
				{
					const FReadbackInput Input(Request, Edit, TEXT("graph_compound_audio_media_texture_node_spec_invalid"), OutError);
					if (Edit.GetIntent() == EChangeIntent::Delete)
					{
						const FReadbackInput DeleteInput(Request, Edit, TEXT("graph_compound_audio_media_texture_node_delete_selector_invalid"), OutError);
						FString ElementId;
						if (!DeleteInput.ReadIdentitySelector(TEXT("element_id"), ElementId))
						{
							return false;
						}
						OutSpec.TextureNodes.Emplace(
							MoveTemp(ElementId),
							FString(),
							EAudioMediaGraphMutationIntent::Delete);
						continue;
					}
					const FUeAgentCanonicalValuePtr& Value = Edit.GetProposedAfter();
					FString ElementId;
					FString ClassRef;
					if (!Input.RequireValue(Value)
						|| !Input.ReadString(*Value, TEXT("element_id"), ElementId)
						|| !Input.ReadString(*Value, TEXT("expression_class_ref"), ClassRef))
					{
						return false;
					}
					OutSpec.TextureNodes.Emplace(
						MoveTemp(ElementId), MoveTemp(ClassRef),
						NativeIntent(Edit.GetIntent()));
				}
				return true;
			case EAudioMediaGraphInputKind::TextureGraphLink:
				OutSpec.Kind = EAudioMediaReadbackKind::TextureGraphLinks;
				for (const FGraphCompoundCanonicalEdit& Edit : Edits)
				{
					const FReadbackInput Input(Request, Edit, TEXT("graph_compound_audio_media_texture_link_spec_invalid"), OutError);
					if (Edit.GetIntent() == EChangeIntent::Delete)
					{
						const FReadbackInput DeleteInput(Request, Edit, TEXT("graph_compound_audio_media_texture_link_delete_selector_invalid"), OutError);
						FString FromElement;
						FString FromPin;
						FString ToElement;
						FString ToPin;
						if (!DeleteInput.ReadIdentitySelector(TEXT("from_element_id"), FromElement)
							|| !DeleteInput.ReadIdentitySelector(TEXT("from_pin_name"), FromPin)
							|| !DeleteInput.ReadIdentitySelector(TEXT("to_element_id"), ToElement)
							|| !DeleteInput.ReadIdentitySelector(TEXT("to_pin_name"), ToPin))
						{
							return false;
						}
						OutSpec.TextureLinks.Emplace(
							FAudioMediaGraphTextureGraphEndpoint(
								MoveTemp(FromElement), MoveTemp(FromPin)),
							FAudioMediaGraphTextureGraphEndpoint(
								MoveTemp(ToElement), MoveTemp(ToPin)),
							EAudioMediaGraphMutationIntent::Delete);
						continue;
					}
					const FUeAgentCanonicalValuePtr& Value = Edit.GetProposedAfter();
					const FUeAgentCanonicalValue* const From = Value.IsValid()
						? FindField(*Value, TEXT("from")) : nullptr;
					const FUeAgentCanonicalValue* const To = Value.IsValid()
						? FindField(*Value, TEXT("to")) : nullptr;
					FString FromElement;
					FString FromPin;
					FString ToElement;
					FString ToPin;
					if (!Input.RequireValue(Value)
						|| !Input.RequireEndpoint(From, TEXT("from")) || !Input.RequireEndpoint(To, TEXT("to"))
						|| !Input.At(TEXT("/from")).ReadString(*From, TEXT("element_id"), FromElement)
						|| !Input.At(TEXT("/from")).ReadString(*From, TEXT("pin_name"), FromPin)
						|| !Input.At(TEXT("/to")).ReadString(*To, TEXT("element_id"), ToElement)
						|| !Input.At(TEXT("/to")).ReadString(*To, TEXT("pin_name"), ToPin))
					{
						return false;
					}
					OutSpec.TextureLinks.Emplace(
						FAudioMediaGraphTextureGraphEndpoint(
							MoveTemp(FromElement), MoveTemp(FromPin)),
						FAudioMediaGraphTextureGraphEndpoint(
							MoveTemp(ToElement), MoveTemp(ToPin)),
						NativeIntent(Edit.GetIntent()));
				}
				return true;
			case EAudioMediaGraphInputKind::TextureGraphParameter:
				OutSpec.Kind = EAudioMediaReadbackKind::TextureGraphParameters;
				for (const FGraphCompoundCanonicalEdit& Edit : Edits)
				{
					const FReadbackInput Input(Request, Edit, TEXT("graph_compound_audio_media_texture_parameter_spec_invalid"), OutError);
					const FUeAgentCanonicalValuePtr& Value = Edit.GetProposedAfter();
					FString ElementId;
					FString PinName;
					FString Alias;
					FAudioMediaGraphTextureGraphValue NativeValue;
					if (!Input.RequireValue(Value)
						|| !Input.ReadString(*Value, TEXT("element_id"), ElementId)
						|| !Input.ReadString(*Value, TEXT("pin_name"), PinName)
						|| !Input.ReadString(*Value, TEXT("alias"), Alias)
						|| !Input.ReadTextureGraphValue(
							FindField(*Value, TEXT("value")), NativeValue, TEXT("/value")))
					{
						return false;
					}
					OutSpec.TextureParameters.Emplace(
						MoveTemp(ElementId), MoveTemp(PinName), MoveTemp(Alias),
						MoveTemp(NativeValue), NativeIntent(Edit.GetIntent()));
				}
				return true;
			case EAudioMediaGraphInputKind::TextureGraphValidateEvaluateSettle:
				OutSpec.Kind =
					EAudioMediaReadbackKind::TextureGraphValidateEvaluateSettle;
				if (!FReadbackInput(Request, Edits[0], TEXT("graph_compound_audio_media_texture_settle_spec_invalid"), OutError)
					.RequireValue(Edits[0].GetProposedAfter()))
				{
					return false;
				}
				return true;
			default:
				OutError = AudioMediaFailure(TEXT("graph_compound_audio_media_production_binding_unimplemented"),
					TEXT("The audio/media production reader does not support this route input kind."),
					Request.GetTargetRef(), TEXT("profile.input_kind"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Profile.GetInputKind())),
					MakeShared<FJsonValueString>(TEXT("registered audio/media readback input kind")));
				OutError.SubjectId = Profile.GetCapabilityId();
				return false;
			}
		}

		bool TryCreateAbsentSnapshot(
			const FGraphCompoundAudioMediaTargetRequest& Request,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError)
		{
			OutSnapshot.Reset();
			FString Filename;
			UE::UAI::Contract::FDiagnostic HeaderError;
			if (!TryResolvePackageHeaderFilename(
					Request.GetTargetRef(),
					EPackageHeaderKind::Asset,
					Filename,
					HeaderError))
			{
				SetError(OutError, HeaderError, Request.GetTargetRef());
				return false;
			}
			const bool bPackageLoaded = FindPackage(nullptr, *TargetPackageName(Request.GetTargetRef())) != nullptr;
			if (bPackageLoaded || IFileManager::Get().FileExists(*Filename))
			{
				OutError = AudioMediaFailure(TEXT("graph_compound_audio_media_target_already_exists"),
					bPackageLoaded ? TEXT("The target audio/media package is already loaded.")
						: TEXT("The target audio/media package already exists on disk."),
					Request.GetTargetRef(), bPackageLoaded ? TEXT("package.loaded") : TEXT("package.persisted"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
				return false;
			}
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("graph-compound:audio-media:absent:")
						+ Request.GetTargetRef(),
					EUeAgentStatePlane::Persisted,
					FUeAgentStateRevision(
						TEXT("graph-compound:audio-media:create"),
						TEXT("absent:") + Request.GetTargetRef(), 0),
					FUeAgentTemporalPoint(
						TEXT("utc-seconds"), FString::Printf(TEXT("%lld"),
							FDateTime::UtcNow().ToUnixTimestamp())),
					FUeAgentStateProvenance(
						TEXT("independent_readback"), Request.GetTargetRef(),
						TEXT("file-absence:") + Filename),
					{}, Resources, OutSnapshot, Error))
			{
				OutError = Error;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
				return false;
			}
			return true;
		}

		bool TryCaptureMetaSoundSnapshot(
			const FGraphCompoundAudioMediaTargetRequest& Request,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError)
		{
			OutSnapshot.Reset();
			UMetaSoundSource* const Source = LoadObject<UMetaSoundSource>(
				nullptr, *TargetObjectPath(Request.GetTargetRef()));
			const IMetaSoundDocumentInterface* const Interface =
				Cast<IMetaSoundDocumentInterface>(Source);
			if (Source == nullptr || Interface == nullptr)
			{
				OutError = Contract::FDiagnostic::Failure(TEXT("graph_compound_audio_media_target_invalid"),
					Source == nullptr ? TEXT("The snapshot target could not be loaded as a MetaSound source.")
						: TEXT("The MetaSound snapshot target has no document interface."), TEXT("snapshot_capture"));
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = Request.GetTargetRef();
				OutError.PropertyPath = Source == nullptr ? TEXT("metasound_source_available") : TEXT("document_interface_available");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				return false;
			}
			FString Revision;
			if (!TryReadPackageAssetRevision(Request.GetTargetRef(), Revision, OutError)) return false;

			const FMetasoundFrontendDocument& Document =
				Interface->GetConstDocument();
			const FMetasoundFrontendGraph& Graph =
				Document.RootGraph.GetConstDefaultGraph();
			TMap<FString, TMap<FString, FString>> Identities;
			for (const FMetasoundFrontendClassInput& Input
				: Document.RootGraph.GetDefaultInterface().Inputs)
			{
				const FString Name = Input.Name.ToString();
				Identities.Add(
					FString::Printf(
						TEXT("input:metasound_interface:input:%s"), *Name),
					AudioMediaIdentity::MetaSoundInterface(
						TEXT("input"), Name));
			}
			for (const FMetasoundFrontendClassOutput& Output
				: Document.RootGraph.GetDefaultInterface().Outputs)
			{
				const FString Name = Output.Name.ToString();
				Identities.Add(
					FString::Printf(
						TEXT("input:metasound_interface:output:%s"), *Name),
					AudioMediaIdentity::MetaSoundInterface(
						TEXT("output"), Name));
			}
			for (const FMetasoundFrontendNode& Node : Graph.Nodes)
			{
				const FString NodeId = Node.GetID().ToString(
					EGuidFormats::DigitsWithHyphensLower);
				Identities.Add(
					TEXT("item:metasound_node:") + NodeId,
					AudioMediaIdentity::MetaSoundNode(NodeId));
				for (const FMetasoundFrontendVertex& Input : Node.Interface.Inputs)
				{
					const FString VertexId = Input.VertexID.ToString(
						EGuidFormats::DigitsWithHyphensLower);
					Identities.Add(
						FString::Printf(
							TEXT("input:metasound_default:%s:%s:%s"),
							*NodeId, *VertexId, *Input.TypeName.ToString()),
						AudioMediaIdentity::MetaSoundDefault(
							NodeId, VertexId));
				}
			}
			for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
			{
				const FMetasoundFrontendNode* const FromNode =
					Graph.Nodes.FindByPredicate([&Edge](const FMetasoundFrontendNode& Node)
					{
						return Node.GetID() == Edge.FromNodeID;
					});
				const FMetasoundFrontendVertex* const FromVertex = FromNode
					? FromNode->Interface.Outputs.FindByPredicate(
						[&Edge](const FMetasoundFrontendVertex& Vertex)
						{
							return Vertex.VertexID == Edge.FromVertexID;
						}) : nullptr;
				if (FromVertex != nullptr)
				{
					const FString FromNodeId = Edge.FromNodeID.ToString(
						EGuidFormats::DigitsWithHyphensLower);
					const FString FromVertexId = Edge.FromVertexID.ToString(
						EGuidFormats::DigitsWithHyphensLower);
					const FString ToNodeId = Edge.ToNodeID.ToString(
						EGuidFormats::DigitsWithHyphensLower);
					const FString ToVertexId = Edge.ToVertexID.ToString(
						EGuidFormats::DigitsWithHyphensLower);
					Identities.Add(
						FString::Printf(
							TEXT("relation:metasound_link:%s:%s->%s:%s:%s"),
							*FromNodeId, *FromVertexId, *ToNodeId,
							*ToVertexId, *FromVertex->TypeName.ToString()),
						AudioMediaIdentity::MetaSoundLink(
							FromNodeId, FromVertexId, ToNodeId, ToVertexId));
				}
			}
			Identities.Add(TEXT("dependency:metasound_compile_settle"), {});

			FUeAgentCanonicalValuePtr Present;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryString(
					TEXT("present"), Resources, Present, StateError))
			{
				OutError = StateError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
				return false;
			}
			const FUeAgentStateRevision StateRevision(
				TEXT("graph-compound:audio-media:metasound"), Revision, 0);
			TArray<FUeAgentStateEntry> Entries;
			for (const TPair<FString, TMap<FString, FString>>& Identity
				: Identities)
			{
				Entries.Emplace(
					AudioMediaIdentity::MakeIdentity(
						Identity.Key,
						Request.GetTargetKind(),
						Request.GetTargetRef(),
						TEXT("graph_compound"),
						Revision,
						Identity.Value),
					FUeAgentCanonicalType(
						TEXT("uai.graph_compound.audio_media.present.v1"),
						EUeAgentValueKind::String, false, TEXT("scalar")),
					Present.ToSharedRef(),
					FUeAgentDefaultValue(
						EUeAgentDefaultSourceKind::DomainNative,
						Request.GetTargetRef(), TEXT("native MetaSound readback"),
						Present.ToSharedRef()),
					FUeAgentStateDetails(
						Identity.Key, TEXT("MetaSound persisted state"),
						{TEXT("GraphCompound")}, true, TEXT("available"), {}),
					StateRevision,
					EUeAgentAuthoredPresence::Present,
					EUeAgentOverrideState::Overridden);
			}
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("graph-compound:audio-media:metasound:")
						+ Request.GetTargetRef(),
					EUeAgentStatePlane::Persisted, StateRevision,
					FUeAgentTemporalPoint(
						TEXT("utc-seconds"), FString::Printf(TEXT("%lld"),
							FDateTime::UtcNow().ToUnixTimestamp())),
					FUeAgentStateProvenance(
						TEXT("independent_readback"), Request.GetTargetRef(),
						TEXT("package-revision:") + Revision),
					MoveTemp(Entries), Resources, OutSnapshot, StateError))
			{
				OutError = StateError;
				MarkGraphCompoundNativeDiagnostic(OutError);
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
				return false;
			}
			return true;
		}

		FString TextureElementId(const UTG_Node& Node)
		{
			const UTG_Expression* const Expression = Node.GetExpression();
			if (Cast<UTG_Expression_Output>(Expression) != nullptr)
			{
				return TEXT("builtin_output");
			}
			return Expression ? Expression->GetName() : FString();
		}

		UTG_Node* FindTextureNode(UTG_Graph& Graph, const FString& ElementId)
		{
			UTG_Node* Result = nullptr;
			Graph.ForEachNodes(
				[&Result, &ElementId](const UTG_Node* Node, const uint32)
				{
					if (Result == nullptr && Node != nullptr
						&& TextureElementId(*Node) == ElementId)
					{
						Result = const_cast<UTG_Node*>(Node);
					}
				});
			return Result;
		}

		bool TryCaptureTextureGraphSnapshot(
			const FGraphCompoundAudioMediaTargetRequest& Request,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError)
		{
			OutSnapshot.Reset();
			UTextureGraph* const Asset = LoadObject<UTextureGraph>(
				nullptr, *TargetObjectPath(Request.GetTargetRef()));
			UTG_Graph* const Graph = Asset ? Asset->Graph() : nullptr;
			if (Graph == nullptr)
			{
				OutError = Contract::FDiagnostic::Failure(TEXT("graph_compound_audio_media_target_invalid"),
					Asset == nullptr ? TEXT("The snapshot target could not be loaded as a TextureGraph asset.")
						: TEXT("The TextureGraph snapshot target has no graph."), TEXT("snapshot_capture"));
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = Request.GetTargetRef();
				OutError.PropertyPath = Asset == nullptr ? TEXT("texture_graph_asset_available") : TEXT("graph_available");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				return false;
			}
			FString Revision;
			if (!TryReadPackageAssetRevision(Request.GetTargetRef(), Revision, OutError)) return false;
			TMap<FString, TMap<FString, FString>> Identities;
			Graph->ForEachNodes(
				[&Identities](const UTG_Node* Node, const uint32)
				{
					if (Node == nullptr || Node->GetExpression() == nullptr)
					{
						return;
					}
					const FString ElementId = TextureElementId(*Node);
					Identities.Add(
						TEXT("item:texture_expression:") + ElementId,
						AudioMediaIdentity::TextureGraphNode(ElementId));
					for (const UTG_Pin* Pin : Node->Pins)
					{
						if (Pin == nullptr)
						{
							continue;
						}
						const FString Direction = Pin->IsInput()
							? TEXT("input") : TEXT("output");
						const FString PinId = FString::Printf(
							TEXT("pin:texture:%s:%s:%s"), *ElementId,
							*Direction, *Pin->GetArgumentName().ToString());
						const FString PinName =
							Pin->GetArgumentName().ToString();
						Identities.Add(
							PinId,
							AudioMediaIdentity::TextureGraphPin(
								ElementId, Direction, PinName));
						if (Pin->IsInput())
						{
							Identities.Add(
								FString::Printf(
									TEXT("input:texture_parameter:%s:%s"),
									*ElementId, *PinName),
								AudioMediaIdentity::TextureGraphParameter(
									ElementId, PinName));
						}
					}
				});
			Graph->ForEachPins(
				[Graph, &Identities](const UTG_Pin* Pin, const uint32, const uint32)
				{
					if (Pin == nullptr || !Pin->IsOutput() || Pin->GetNodePtr() == nullptr)
					{
						return;
					}
					const FString FromId = FString::Printf(
						TEXT("pin:texture:%s:output:%s"),
						*TextureElementId(*Pin->GetNodePtr()),
						*Pin->GetArgumentName().ToString());
					for (const FTG_Id& EdgeId : Pin->GetEdges())
					{
						const UTG_Pin* const ToPin = Graph->GetPin(EdgeId);
						if (ToPin == nullptr || ToPin->GetNodePtr() == nullptr)
						{
							continue;
						}
						const FString ToId = FString::Printf(
							TEXT("pin:texture:%s:input:%s"),
							*TextureElementId(*ToPin->GetNodePtr()),
							*ToPin->GetArgumentName().ToString());
						const FString FromElementId =
							TextureElementId(*Pin->GetNodePtr());
						const FString FromPinName =
							Pin->GetArgumentName().ToString();
						const FString ToElementId =
							TextureElementId(*ToPin->GetNodePtr());
						const FString ToPinName =
							ToPin->GetArgumentName().ToString();
						Identities.Add(
							TEXT("relation:texture_link:") + FromId + TEXT("->") + ToId,
							AudioMediaIdentity::TextureGraphLink(
								FromElementId,
								FromPinName,
								ToElementId,
								ToPinName));
					}
				});
			Identities.Add(TEXT("dependency:texture_graph_settle"), {});

			FUeAgentCanonicalValuePtr Present;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryString(
					TEXT("present"), Resources, Present, StateError))
			{
				OutError = StateError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
				return false;
			}
			const FUeAgentStateRevision StateRevision(
				TEXT("graph-compound:audio-media:texture-graph"), Revision, 0);
			TArray<FUeAgentStateEntry> Entries;
			for (const TPair<FString, TMap<FString, FString>>& Identity
				: Identities)
			{
				Entries.Emplace(
					AudioMediaIdentity::MakeIdentity(
						Identity.Key,
						Request.GetTargetKind(),
						Request.GetTargetRef(),
						TEXT("graph_compound"),
						Revision,
						Identity.Value),
					FUeAgentCanonicalType(
						TEXT("uai.graph_compound.audio_media.present.v1"),
						EUeAgentValueKind::String, false, TEXT("scalar")),
					Present.ToSharedRef(),
					FUeAgentDefaultValue(
						EUeAgentDefaultSourceKind::DomainNative,
						Request.GetTargetRef(), TEXT("native TextureGraph readback"),
						Present.ToSharedRef()),
					FUeAgentStateDetails(
						Identity.Key, TEXT("TextureGraph persisted state"),
						{TEXT("GraphCompound")}, true, TEXT("available"), {}),
					StateRevision,
					EUeAgentAuthoredPresence::Present,
					EUeAgentOverrideState::Overridden);
			}
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("graph-compound:audio-media:texture-graph:")
						+ Request.GetTargetRef(),
					EUeAgentStatePlane::Persisted, StateRevision,
					FUeAgentTemporalPoint(
						TEXT("utc-seconds"), FString::Printf(TEXT("%lld"),
							FDateTime::UtcNow().ToUnixTimestamp())),
					FUeAgentStateProvenance(
						TEXT("independent_readback"), Request.GetTargetRef(),
						TEXT("package-revision:") + Revision),
					MoveTemp(Entries), Resources, OutSnapshot, StateError))
			{
				OutError = StateError;
				MarkGraphCompoundNativeDiagnostic(OutError);
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
				return false;
			}
			return true;
		}

		bool TryCaptureSoundCueSnapshot(
			const FGraphCompoundAudioMediaTargetRequest& Request,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError)
		{
			OutSnapshot.Reset();
			USoundCue* const Cue = LoadObject<USoundCue>(
				nullptr, *TargetObjectPath(Request.GetTargetRef()));
			if (Cue == nullptr)
			{
				OutError = Contract::FDiagnostic::Failure(TEXT("graph_compound_audio_media_target_invalid"),
					TEXT("The snapshot target could not be loaded as a SoundCue asset."), TEXT("snapshot_capture"));
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = Request.GetTargetRef();
				OutError.PropertyPath = TEXT("sound_cue_asset_available");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				return false;
			}
			FString Revision;
			if (!TryReadPackageAssetRevision(Request.GetTargetRef(), Revision, OutError)) return false;

			TMap<FString, TMap<FString, FString>> Identities;
			for (USoundNode* Node : Cue->AllNodes)
			{
				if (Node == nullptr)
				{
					continue;
				}
				const FString ParentPath = Node->GetPathName();
				Identities.Add(
					TEXT("item:sound_node:") + ParentPath,
					AudioMediaIdentity::SoundCueNode(ParentPath));
				for (USoundNode* Child : Node->ChildNodes)
				{
					if (Child != nullptr)
					{
						const FString ChildPath = Child->GetPathName();
						Identities.Add(
							FString::Printf(
								TEXT("relation:sound_child:%s->%s"),
								*ParentPath, *ChildPath),
							AudioMediaIdentity::SoundCueRelation(
								ParentPath, ChildPath));
					}
				}
			}
			Identities.Add(TEXT("binding:sound_root"), {});
			Identities.Add(TEXT("dependency:sound_compile_settle"), {});

			FUeAgentCanonicalValuePtr Present;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryString(
					TEXT("present"), Resources, Present, StateError))
			{
				OutError = StateError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
				return false;
			}
			const FUeAgentStateRevision StateRevision(
				TEXT("graph-compound:audio-media:sound-cue"), Revision, 0);
			TArray<FUeAgentStateEntry> Entries;
			for (const TPair<FString, TMap<FString, FString>>& Identity
				: Identities)
			{
				Entries.Emplace(
					AudioMediaIdentity::MakeIdentity(
						Identity.Key,
						Request.GetTargetKind(),
						Request.GetTargetRef(),
						TEXT("graph_compound"),
						Revision,
						Identity.Value),
					FUeAgentCanonicalType(
						TEXT("uai.graph_compound.audio_media.present.v1"),
						EUeAgentValueKind::String, false, TEXT("scalar")),
					Present.ToSharedRef(),
					FUeAgentDefaultValue(
						EUeAgentDefaultSourceKind::DomainNative,
						Request.GetTargetRef(), TEXT("native sound cue readback"),
						Present.ToSharedRef()),
					FUeAgentStateDetails(
						Identity.Key, TEXT("SoundCue persisted state"),
						{TEXT("GraphCompound")}, true, TEXT("available"), {}),
					StateRevision,
					EUeAgentAuthoredPresence::Present,
					EUeAgentOverrideState::Overridden);
			}
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("graph-compound:audio-media:sound-cue:")
						+ Request.GetTargetRef(),
					EUeAgentStatePlane::Persisted, StateRevision,
					FUeAgentTemporalPoint(
						TEXT("utc-seconds"), FString::Printf(TEXT("%lld"),
							FDateTime::UtcNow().ToUnixTimestamp())),
					FUeAgentStateProvenance(
						TEXT("independent_readback"), Request.GetTargetRef(),
						TEXT("package-revision:") + Revision),
					MoveTemp(Entries), Resources, OutSnapshot, StateError))
			{
				OutError = StateError;
				MarkGraphCompoundNativeDiagnostic(OutError);
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
				return false;
			}
			return true;
		}

		UObject* TryCreateAsset(
			const FAudioMediaReadbackSpec& Spec,
			const FString& TargetRef,
			UPackage& Package,
			FGraphCompoundRuntimeError& OutError)
		{
			const FName Name(*TargetAssetName(TargetRef));
			const EObjectFlags Flags =
				RF_Public | RF_Standalone | RF_Transactional;
			if (Spec.AssetKind == TEXT("metasound_source"))
			{
				UMetaSoundSourceFactory* const Factory =
					NewObject<UMetaSoundSourceFactory>();
				UObject* const Created = Factory
					? Factory->FactoryCreateNew(
						UMetaSoundSource::StaticClass(), &Package, Name,
						Flags, nullptr, GWarn)
					: nullptr;
				if (Created == nullptr)
				{
					OutError = AudioMediaFailure(TEXT("graph_compound_audio_media_factory_failed"),
						Factory ? TEXT("The MetaSound source factory returned no asset.")
							: TEXT("The MetaSound source factory could not be created."),
						TargetRef, Factory ? TEXT("asset") : TEXT("factory"), nullptr,
						MakeShared<FJsonValueString>(Factory ? UMetaSoundSource::StaticClass()->GetPathName()
							: UMetaSoundSourceFactory::StaticClass()->GetPathName()));
				}
				return Created;
			}
			if (Spec.AssetKind == TEXT("sound_cue"))
			{
				USoundCue* const Cue = NewObject<USoundCue>(
					&Package, USoundCue::StaticClass(), Name, Flags);
				if (Cue != nullptr)
				{
					Cue->CreateGraph();
				}
				else
					OutError = AudioMediaFailure(TEXT("graph_compound_audio_media_factory_failed"),
						TEXT("The SoundCue asset could not be created."), TargetRef, TEXT("asset"), nullptr,
						MakeShared<FJsonValueString>(USoundCue::StaticClass()->GetPathName()));
				return Cue;
			}
			if (Spec.AssetKind == TEXT("texture_graph"))
			{
				UTextureGraph* const Graph = NewObject<UTextureGraph>(
					&Package, UTextureGraph::StaticClass(), Name, Flags);
				if (Graph != nullptr)
				{
					Graph->Construct(Name.ToString());
				}
				else
					OutError = AudioMediaFailure(TEXT("graph_compound_audio_media_factory_failed"),
						TEXT("The TextureGraph asset could not be created."), TargetRef, TEXT("asset"), nullptr,
						MakeShared<FJsonValueString>(UTextureGraph::StaticClass()->GetPathName()));
				return Graph;
			}
			OutError = AudioMediaFailure(TEXT("graph_compound_audio_media_asset_kind_invalid"),
				TEXT("The audio/media asset kind has no production factory."), TargetRef, TEXT("asset_kind"),
				MakeShared<FJsonValueString>(Spec.AssetKind), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
					MakeShared<FJsonValueString>(TEXT("metasound_source")), MakeShared<FJsonValueString>(TEXT("sound_cue")),
					MakeShared<FJsonValueString>(TEXT("texture_graph"))}));
			return nullptr;
		}

		bool TryApplyMetaSound(
			UMetaSoundSource& Source,
			const FAudioMediaReadbackSpec& Spec,
			const FAudioMediaGraphMetaSoundNativeHook& Hook,
			FGraphCompoundRuntimeError& OutError)
		{
			FAudioMediaGraphError NativeError;
			bool bApplied = false;
			switch (Spec.Kind)
			{
			case EAudioMediaReadbackKind::MetaSoundInterface:
				bApplied = Hook.TryApplyInterfaceMutations(
					Source, Spec.MetaInterfaces, NativeError);
				break;
			case EAudioMediaReadbackKind::MetaSoundNodes:
				bApplied = Hook.TryApplyNodeMutations(
					Source, Spec.MetaNodes, NativeError);
				break;
			case EAudioMediaReadbackKind::MetaSoundLinks:
				bApplied = Hook.TryApplyLinkMutations(
					Source, Spec.MetaLinks, NativeError);
				break;
			case EAudioMediaReadbackKind::MetaSoundDefault:
				if (!Spec.MetaDefault.IsSet())
					NativeError = AudioMediaFailure(TEXT("graph_compound_audio_media_metasound_native_failed"),
						TEXT("The MetaSound default mutation has no decoded default value."),
						Source.GetPathName(), TEXT("spec.meta_default"), nullptr,
						MakeShared<FJsonValueString>(TEXT("decoded MetaSound default mutation")));
				else
					bApplied = Hook.TryApplyDefaultMutation(Source, Spec.MetaDefault.GetValue(), NativeError);
				break;
			case EAudioMediaReadbackKind::MetaSoundCompileSettle:
				bApplied = Hook.TryRegisterCompileSettle(Source, NativeError);
				break;
			default:
				NativeError = AudioMediaFailure(TEXT("graph_compound_audio_media_metasound_native_failed"),
					TEXT("The MetaSound native operation does not support this readback kind."),
					Source.GetPathName(), TEXT("readback.kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Spec.Kind)),
					MakeShared<FJsonValueString>(TEXT("MetaSound mutation or settle readback kind")));
				break;
			}
			if (!bApplied)
			{
				OutError = MoveTemp(NativeError);
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Source.GetPathName();
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Source.GetPathName();
				return false;
			}
			return true;
		}

		bool TryApplySoundCue(
			USoundCue& Cue,
			const FAudioMediaReadbackSpec& Spec,
			const FAudioMediaGraphSoundCueNativeHook& Hook,
			FGraphCompoundRuntimeError& OutError)
		{
			FAudioMediaGraphError NativeError;
			bool bApplied = false;
			switch (Spec.Kind)
			{
			case EAudioMediaReadbackKind::SoundCueNodes:
				bApplied = Hook.TryApplyNodeMutations(
					Cue, Spec.SoundNodes, NativeError);
				break;
			case EAudioMediaReadbackKind::SoundCueRelations:
				bApplied = Hook.TryApplyRelationMutations(
					Cue, Spec.SoundRelations, NativeError);
				break;
			case EAudioMediaReadbackKind::SoundCueRoot:
				bApplied = Hook.TrySetRoot(Cue, Spec.SoundRoot, NativeError);
				break;
			case EAudioMediaReadbackKind::SoundCueCompileSettle:
				bApplied = Hook.TrySynchronizeAndCompile(Cue, NativeError);
				break;
			default:
				NativeError = AudioMediaFailure(TEXT("graph_compound_audio_media_sound_cue_native_failed"),
					TEXT("The SoundCue native operation does not support this readback kind."),
					Cue.GetPathName(), TEXT("readback.kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Spec.Kind)),
					MakeShared<FJsonValueString>(TEXT("SoundCue mutation or settle readback kind")));
				break;
			}
			if (!bApplied)
			{
				OutError = MoveTemp(NativeError);
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Cue.GetPathName();
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Cue.GetPathName();
				return false;
			}
			return true;
		}

		bool TryApplyTextureGraph(
			UTextureGraph& Graph,
			const FAudioMediaReadbackSpec& Spec,
			const FAudioMediaGraphTextureGraphNativeHook& Hook,
			FGraphCompoundRuntimeError& OutError,
			FAudioMediaGraphTextureGraphNodeBindings* Bindings = nullptr,
			const FString& TargetAssetPath = {})
		{
			FAudioMediaGraphError NativeError;
			bool bApplied = false;
			switch (Spec.Kind)
			{
			case EAudioMediaReadbackKind::TextureGraphNodes:
				bApplied = Hook.TryApplyNodeMutations(
					Graph, Spec.TextureNodes, NativeError, Bindings, TargetAssetPath);
				break;
			case EAudioMediaReadbackKind::TextureGraphLinks:
				bApplied = Hook.TryApplyLinkMutations(
					Graph, Spec.TextureLinks, NativeError);
				break;
			case EAudioMediaReadbackKind::TextureGraphParameters:
				bApplied = Hook.TryApplyParameterMutations(
					Graph, Spec.TextureParameters, NativeError);
				break;
			case EAudioMediaReadbackKind::TextureGraphValidateEvaluateSettle:
				bApplied = Hook.TryValidateEvaluateSettle(Graph, NativeError);
				break;
			case EAudioMediaReadbackKind::TextureGraphSemanticComposite:
			{
				TArray<FAudioMediaGraphTextureGraphLinkMutation> LinkDeletes;
				TArray<FAudioMediaGraphTextureGraphLinkMutation> LinkCreates;
				for (const FAudioMediaGraphTextureGraphLinkMutation& Mutation
					: Spec.TextureLinks)
				{
					(Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Delete
						? LinkDeletes : LinkCreates).Add(Mutation);
				}
				TArray<FAudioMediaGraphTextureGraphNodeMutation> DestructiveNodes;
				TArray<FAudioMediaGraphTextureGraphNodeMutation> ConstructiveNodes;
				for (const FAudioMediaGraphTextureGraphNodeMutation& Mutation
					: Spec.TextureNodes)
				{
					const EAudioMediaGraphMutationIntent Intent = Mutation.GetIntent();
					(Intent == EAudioMediaGraphMutationIntent::Delete
						|| Intent == EAudioMediaGraphMutationIntent::Replace
						? DestructiveNodes : ConstructiveNodes).Add(Mutation);
				}
				bApplied = (LinkDeletes.IsEmpty()
						|| Hook.TryApplyLinkMutations(Graph, LinkDeletes, NativeError, Bindings))
					&& (DestructiveNodes.IsEmpty()
						|| Hook.TryApplyNodeMutations(
							Graph, DestructiveNodes, NativeError, Bindings, TargetAssetPath))
					&& (ConstructiveNodes.IsEmpty()
						|| Hook.TryApplyNodeMutations(
							Graph, ConstructiveNodes, NativeError, Bindings, TargetAssetPath))
					&& (Spec.TextureParameters.IsEmpty()
						|| Hook.TryApplyParameterMutations(
							Graph, Spec.TextureParameters, NativeError, Bindings))
					&& (LinkCreates.IsEmpty()
						|| Hook.TryApplyLinkMutations(Graph, LinkCreates, NativeError, Bindings))
					&& Hook.TryValidateEvaluateSettle(Graph, NativeError);
				break;
			}
			default:
				NativeError = AudioMediaFailure(TEXT("graph_compound_audio_media_texture_graph_native_failed"),
					TEXT("The TextureGraph native operation does not support this readback kind."),
					Graph.GetPathName(), TEXT("readback.kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Spec.Kind)),
					MakeShared<FJsonValueString>(TEXT("TextureGraph mutation, semantic composite or settle readback kind")));
				break;
			}
			if (!bApplied)
			{
				OutError = MoveTemp(NativeError);
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Graph.GetPathName();
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Graph.GetPathName();
				return false;
			}
			return true;
		}

		UE::UAI::Contract::FDiagnostic ReadbackFailure(const FString& Reason, const FString& Target,
			const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = AudioMediaFailure(TEXT("graph_compound_native_readback_failed"),
				Reason, Target, Field, MoveTemp(Actual), MoveTemp(Expected));
			Error.Stage = TEXT("native_readback");
			return Error;
		}

		bool VerifyMetaSoundReadback(
			UMetaSoundSource& Source,
			const FAudioMediaReadbackSpec& Spec,
			UE::UAI::Contract::FDiagnostic& OutDetail)
		{
			OutDetail.Reset();
			const IMetaSoundDocumentInterface* const Interface =
				Cast<IMetaSoundDocumentInterface>(&Source);
			if (Interface == nullptr)
			{
				OutDetail = ReadbackFailure(TEXT("The MetaSound source has no native document interface."),
					Source.GetPathName(), TEXT("document_interface"), nullptr,
					MakeShared<FJsonValueString>(TEXT("IMetaSoundDocumentInterface")));
				return false;
			}
			const FMetasoundFrontendDocument& Document =
				Interface->GetConstDocument();
			const FMetasoundFrontendGraph& Graph =
				Document.RootGraph.GetConstDefaultGraph();
			if (Spec.Kind == EAudioMediaReadbackKind::MetaSoundInterface)
			{
				for (const FAudioMediaGraphMetaSoundInterfaceMutation& Mutation
					: Spec.MetaInterfaces)
				{
					const bool bInput = Mutation.GetInterfaceKind() == TEXT("input");
					const FMetasoundFrontendClassVertex* Vertex = bInput
						? static_cast<const FMetasoundFrontendClassVertex*>(
							Document.RootGraph.GetDefaultInterface().Inputs.FindByPredicate(
								[&Mutation](const FMetasoundFrontendClassInput& Item)
								{
									return Item.Name == Mutation.GetName();
								}))
						: static_cast<const FMetasoundFrontendClassVertex*>(
							Document.RootGraph.GetDefaultInterface().Outputs.FindByPredicate(
								[&Mutation](const FMetasoundFrontendClassOutput& Item)
								{
									return Item.Name == Mutation.GetName();
								}));
					const bool bShouldExist = Mutation.GetIntent()
						!= EAudioMediaGraphMutationIntent::Delete;
					if (bShouldExist != (Vertex != nullptr)
						|| (Vertex != nullptr
							&& Vertex->TypeName != Mutation.GetDataType()))
					{
						const int32 Index = static_cast<int32>(&Mutation - Spec.MetaInterfaces.GetData());
						const bool bPresenceDiffers = bShouldExist != (Vertex != nullptr);
						OutDetail = ReadbackFailure(bPresenceDiffers
								? TEXT("The MetaSound interface presence differs from the requested intent.")
								: TEXT("The MetaSound interface data type differs from the requested type."),
							Source.GetPathName(), FString::Printf(TEXT("/interfaces/%d/%s"), Index,
								bPresenceDiffers ? TEXT("present") : TEXT("data_type_ref")),
							bPresenceDiffers ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(Vertex != nullptr))
								: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Vertex->TypeName.ToString())),
							bPresenceDiffers ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(bShouldExist))
								: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Mutation.GetDataType().ToString())));
						OutDetail.OperationIndex = Index;
						OutDetail.CandidateName = Mutation.GetInterfaceKind() + TEXT(":") + Mutation.GetName().ToString();
						return false;
					}
					if (bInput && Vertex != nullptr)
					{
						const FMetasoundFrontendClassInput* const Input =
							static_cast<const FMetasoundFrontendClassInput*>(Vertex);
						const FMetasoundFrontendLiteral* const Default =
							Input->FindConstDefault(
								Metasound::Frontend::DefaultPageID);
						if (Default == nullptr
							|| *Default != Mutation.GetDefaultValue())
						{
							const int32 Index = static_cast<int32>(&Mutation - Spec.MetaInterfaces.GetData());
							const FString Field = FString::Printf(TEXT("/interfaces/%d/default"), Index);
							OutDetail = ReadbackFailure(Default
									? TEXT("The MetaSound interface default differs from the requested value.")
									: TEXT("The MetaSound interface has no default value."),
								Source.GetPathName(), Field + TEXT("/native_matches_requested"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
							TSharedPtr<FJsonValue> ActualValue;
							TSharedPtr<FJsonValue> ExpectedValue;
							if (Default)
								if (const auto Value = UeAgentAudioAuthoring::EncodeMetaSoundLiteral(*Default))
									ActualValue = MakeShared<FJsonValueObject>(Value);
							if (const auto Value = UeAgentAudioAuthoring::EncodeMetaSoundLiteral(Mutation.GetDefaultValue()))
								ExpectedValue = MakeShared<FJsonValueObject>(Value);
							const auto Difference = CanonicalJson::CompareJsonValues(ExpectedValue, ActualValue, Field);
							if (!Difference.bEquivalent)
							{
								Difference.ApplyTo(OutDetail);
								OutDetail.PropertyPath = Difference.Path;
							}
							else
							{
								OutDetail.PublicMessage += TEXT(" The encoded values are identical or unavailable; the native comparison still failed.");
								OutDetail.ProposedValue = ExpectedValue;
							}
							OutDetail.OperationIndex = Index;
							OutDetail.CandidateName = Mutation.GetInterfaceKind() + TEXT(":") + Mutation.GetName().ToString();
							return false;
						}
					}
				}
				return true;
			}
			UeAgentAudioAuthoring::FAudioGraphIntent Intent;
			if (Spec.Kind == EAudioMediaReadbackKind::MetaSoundNodes)
			{
				for (const auto& Mutation : Spec.MetaNodes)
				{
					auto& Node = Intent.MetaNodes.AddDefaulted_GetRef();
					Node.Id = Mutation.GetNodeId();
					Node.bPresent = Mutation.GetIntent() != EAudioMediaGraphMutationIntent::Delete;
					Node.ClassName = Mutation.GetClassName();
					Node.Major = Mutation.GetMajorVersion();
					Node.Minor = Mutation.GetMinorVersion();
					Node.SourcePointer = TEXT("/nodes/") + ::LexToString(static_cast<int32>(&Mutation - Spec.MetaNodes.GetData()));
				}
				const bool bMatches = Intent.Verify(Source, OutDetail);
				if (!bMatches) MarkGraphCompoundNativeDiagnostic(OutDetail);
				return bMatches;
			}
			if (Spec.Kind == EAudioMediaReadbackKind::MetaSoundLinks)
			{
				for (const auto& Mutation : Spec.MetaLinks)
					Intent.MetaLinks.Add({Mutation.GetFromNodeId(), Mutation.GetFromVertexId(),
						Mutation.GetToNodeId(), Mutation.GetToVertexId(),
						Mutation.GetIntent() != EAudioMediaGraphMutationIntent::Delete, TEXT("/links/") + ::LexToString(static_cast<int32>(&Mutation - Spec.MetaLinks.GetData()))});
				const bool bMatches = Intent.Verify(Source, OutDetail);
				if (!bMatches) MarkGraphCompoundNativeDiagnostic(OutDetail);
				return bMatches;
			}
			if (Spec.Kind == EAudioMediaReadbackKind::MetaSoundDefault)
			{
				if (!Spec.MetaDefault.IsSet() || !Spec.FrozenMetaDefault)
				{
					OutDetail = ReadbackFailure(!Spec.MetaDefault.IsSet()
							? TEXT("The MetaSound readback specification has no default mutation.")
							: TEXT("The MetaSound readback specification has no frozen default value."),
						Source.GetPathName(), !Spec.MetaDefault.IsSet() ? TEXT("spec.meta_default") : TEXT("spec.frozen_meta_default"),
						nullptr, MakeShared<FJsonValueString>(TEXT("expected default value")));
					return false;
				}
				Intent.MetaDefaults.Add({Spec.MetaDefault->GetNodeId(), Spec.MetaDefault->GetVertexId(),
					Spec.FrozenMetaDefault, Spec.FrozenMetaDefault, true});
				const bool bMatches = Intent.Verify(Source, OutDetail);
				if (!bMatches) MarkGraphCompoundNativeDiagnostic(OutDetail);
				return bMatches;
			}
			if (Spec.Kind == EAudioMediaReadbackKind::MetaSoundCompileSettle)
			{
				if (Document.RootGraph.Metadata.GetClassName() != FMetasoundFrontendClassName()) return true;
				OutDetail = ReadbackFailure(TEXT("The MetaSound root graph has no class name after compile/settle."),
					Source.GetPathName(), TEXT("root_graph.class_name.present"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			OutDetail = ReadbackFailure(TEXT("The MetaSound readback does not support this kind."),
				Source.GetPathName(), TEXT("readback.kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Spec.Kind)),
				MakeShared<FJsonValueString>(TEXT("MetaSound interface, nodes, links, default or compile/settle")));
			return false;
		}

		bool VerifySoundCueReadback(
			USoundCue& Cue,
			const FAudioMediaReadbackSpec& Spec,
			UE::UAI::Contract::FDiagnostic& OutDetail)
		{
			OutDetail.Reset();
			UeAgentAudioAuthoring::FAudioGraphIntent Intent;
			const FString Prefix = Cue.GetPathName() + TEXT(":");
			auto RelativeName = [&](const FString& Path) {
				return Path.StartsWith(Prefix) ? FName(*Path.RightChop(Prefix.Len())) : NAME_None;
			};
			if (Spec.Kind == EAudioMediaReadbackKind::SoundCueNodes)
			{
				for (const auto& Mutation : Spec.SoundNodes)
				{
					auto& Node = Intent.SoundNodes.AddDefaulted_GetRef();
					Node.Name = RelativeName(Mutation.GetObjectPath());
					Node.bPresent = Mutation.GetIntent() != EAudioMediaGraphMutationIntent::Delete;
					Node.ClassPath = Mutation.GetNativeClassRef();
					Node.bExactClass = false;
					Node.SourcePointer = TEXT("/nodes/") + ::LexToString(static_cast<int32>(&Mutation - Spec.SoundNodes.GetData()));
				}
				const bool bMatches = Intent.Verify(Cue, OutDetail);
				if (!bMatches) MarkGraphCompoundNativeDiagnostic(OutDetail);
				return bMatches;
			}
			if (Spec.Kind == EAudioMediaReadbackKind::SoundCueRelations)
			{
				for (const auto& Mutation : Spec.SoundRelations)
					Intent.SoundLinks.Add({RelativeName(Mutation.GetParentObjectPath()),
						RelativeName(Mutation.GetChildObjectPath()), Mutation.GetOrder(),
						Mutation.GetIntent() != EAudioMediaGraphMutationIntent::Delete, TEXT("/relations/") + ::LexToString(static_cast<int32>(&Mutation - Spec.SoundRelations.GetData()))});
				const bool bMatches = Intent.Verify(Cue, OutDetail);
				if (!bMatches) MarkGraphCompoundNativeDiagnostic(OutDetail);
				return bMatches;
			}
			if (Spec.Kind == EAudioMediaReadbackKind::SoundCueRoot)
			{
				Intent.SoundRoot = RelativeName(Spec.SoundRoot);
				// This typed operation requires a node, unlike workspace's explicit empty root.
				if (Intent.SoundRoot->IsNone())
				{
					OutDetail = ReadbackFailure(TEXT("The requested SoundCue root does not identify a node owned by this cue."),
						Cue.GetPathName(), TEXT("spec.root_node_object_path"), MakeShared<FJsonValueString>(Spec.SoundRoot),
						MakeShared<FJsonValueString>(Prefix + TEXT("<node name>")));
					return false;
				}
				const bool bMatches = Intent.Verify(Cue, OutDetail);
				if (!bMatches) MarkGraphCompoundNativeDiagnostic(OutDetail);
				return bMatches;
			}
			if (Spec.Kind == EAudioMediaReadbackKind::SoundCueCompileSettle)
			{
				if (Cue.GetGraph() == nullptr)
				{
					OutDetail = ReadbackFailure(TEXT("The SoundCue graph is missing after compilation."),
						Cue.GetPathName(), TEXT("graph"), nullptr, MakeShared<FJsonValueString>(TEXT("SoundCue graph")));
					return false;
				}
				return true;
			}
			OutDetail = ReadbackFailure(TEXT("The SoundCue readback does not support this kind."),
				Cue.GetPathName(), TEXT("readback.kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Spec.Kind)),
				MakeShared<FJsonValueString>(TEXT("SoundCue nodes, relations, root or compile/settle")));
			return false;
		}

		bool VerifyTextureGraphReadback(
			UTextureGraph& Asset,
			const FAudioMediaReadbackSpec& Spec,
			UE::UAI::Contract::FDiagnostic& OutDetail,
			const FAudioMediaGraphTextureGraphNodeBindings* Bindings = nullptr)
		{
			OutDetail.Reset();
			auto Fail = [&](const FString& Reason, const FString& Field,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
				const int32 Index = INDEX_NONE, const FString& Identity = FString())
			{
				OutDetail = ReadbackFailure(Reason, Asset.GetPathName(), Field, MoveTemp(Actual), MoveTemp(Expected));
				if (Index != INDEX_NONE) OutDetail.OperationIndex = Index;
				OutDetail.CandidateName = Identity;
				return false;
			};
			auto NumberFact = [](const double Value) -> TSharedPtr<FJsonValue>
			{
				return FMath::IsFinite(Value) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Value))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), Value)));
			};
			UTG_Graph* const Graph = Asset.Graph();
			if (Graph == nullptr)
			{
				return Fail(TEXT("The TextureGraph asset has no native graph."), TEXT("graph"),
					nullptr, MakeShared<FJsonValueString>(TEXT("TextureGraph native graph")));
			}
			const bool bComposite = Spec.Kind
				== EAudioMediaReadbackKind::TextureGraphSemanticComposite;
			const FAudioMediaGraphTextureGraphNativeHook Hook;
			auto Resolve = [&](const FString& ElementId)
			{
				return Bindings ? Hook.ResolveNode(*Graph, ElementId, Bindings)
					: FindTextureNode(*Graph, ElementId);
			};
			if (Spec.Kind == EAudioMediaReadbackKind::TextureGraphNodes || bComposite)
			{
				for (const FAudioMediaGraphTextureGraphNodeMutation& Mutation
					: Spec.TextureNodes)
				{
					const int32 Index = static_cast<int32>(&Mutation - Spec.TextureNodes.GetData());
					const FString Field = TEXT("/nodes/") + ::LexToString(Index);
					UTG_Node* const Node = Resolve(Mutation.GetElementId());
					if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Delete)
					{
						if (Node != nullptr)
						{
							return Fail(TEXT("The deleted TextureGraph expression still exists."), Field + TEXT("/present"),
								MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false),
								Index, Mutation.GetElementId());
						}
						continue;
					}
					UClass* const ExpectedClass = LoadObject<UClass>(
						nullptr, *Mutation.GetExpressionClassRef());
					if (Node == nullptr || Node->GetExpression() == nullptr
						|| ExpectedClass == nullptr
						|| Node->GetExpression()->GetClass() != ExpectedClass)
					{
						if (!Node)
							return Fail(TEXT("The requested TextureGraph expression node is missing."),
								Field + TEXT("/present"), MakeShared<FJsonValueBoolean>(false),
								MakeShared<FJsonValueBoolean>(true), Index, Mutation.GetElementId());
						if (!Node->GetExpression())
							return Fail(TEXT("The TextureGraph node has no expression object."),
								Field + TEXT("/expression"), nullptr, MakeShared<FJsonValueString>(Mutation.GetExpressionClassRef()),
								Index, Mutation.GetElementId());
						if (!ExpectedClass)
							return Fail(TEXT("The expected TextureGraph expression class could not be loaded."),
								Field + TEXT("/expected_class"), MakeShared<FJsonValueString>(Mutation.GetExpressionClassRef()),
								MakeShared<FJsonValueString>(TEXT("loadable expression class")), Index, Mutation.GetElementId());
						return Fail(TEXT("The TextureGraph expression class differs from the requested exact class."),
							Field + TEXT("/expression/class"), MakeShared<FJsonValueString>(Node->GetExpression()->GetClass()->GetPathName()),
							MakeShared<FJsonValueString>(ExpectedClass->GetPathName()), Index, Mutation.GetElementId());
					}
				}
				if (!bComposite) return true;
			}
			if (Spec.Kind == EAudioMediaReadbackKind::TextureGraphLinks || bComposite)
			{
				for (const FAudioMediaGraphTextureGraphLinkMutation& Mutation
					: Spec.TextureLinks)
				{
					UTG_Node* const From = Resolve(Mutation.GetFrom().GetElementId());
					UTG_Node* const To = Resolve(Mutation.GetTo().GetElementId());
					FTG_Name FromName(*Mutation.GetFrom().GetPinName());
					FTG_Name ToName(*Mutation.GetTo().GetPinName());
					const UTG_Pin* const FromPin = From
						? From->GetOutputPin(FromName) : nullptr;
					const UTG_Pin* const ToPin = To
						? To->GetInputPin(ToName) : nullptr;
					const bool bPresent = FromPin != nullptr && ToPin != nullptr
						&& FromPin->GetEdges().Contains(ToPin->GetId());
					if ((Mutation.GetIntent()
						== EAudioMediaGraphMutationIntent::Delete) ? bPresent : !bPresent)
					{
						const int32 Index = static_cast<int32>(&Mutation - Spec.TextureLinks.GetData());
						const FString Field = TEXT("/links/") + ::LexToString(Index);
						const FString Identity = Mutation.GetFrom().GetElementId() + TEXT(":") + Mutation.GetFrom().GetPinName()
							+ TEXT(" -> ") + Mutation.GetTo().GetElementId() + TEXT(":") + Mutation.GetTo().GetPinName();
						if (!From)
							return Fail(TEXT("The TextureGraph link source node is missing."), Field + TEXT("/from/node"),
								nullptr, MakeShared<FJsonValueString>(Mutation.GetFrom().GetElementId()), Index, Identity);
						if (!FromPin)
							return Fail(TEXT("The TextureGraph link source output pin is missing."), Field + TEXT("/from/pin"),
								nullptr, MakeShared<FJsonValueString>(Mutation.GetFrom().GetPinName()), Index, Identity);
						if (!To)
							return Fail(TEXT("The TextureGraph link destination node is missing."), Field + TEXT("/to/node"),
								nullptr, MakeShared<FJsonValueString>(Mutation.GetTo().GetElementId()), Index, Identity);
						if (!ToPin)
							return Fail(TEXT("The TextureGraph link destination input pin is missing."), Field + TEXT("/to/pin"),
								nullptr, MakeShared<FJsonValueString>(Mutation.GetTo().GetPinName()), Index, Identity);
						return Fail(TEXT("The TextureGraph edge presence differs from the requested intent."),
							Field + TEXT("/present"), MakeShared<FJsonValueBoolean>(bPresent),
							MakeShared<FJsonValueBoolean>(Mutation.GetIntent() != EAudioMediaGraphMutationIntent::Delete), Index, Identity);
					}
				}
				if (!bComposite) return true;
			}
			if (Spec.Kind == EAudioMediaReadbackKind::TextureGraphParameters || bComposite)
			{
				for (const FAudioMediaGraphTextureGraphParameterMutation& Mutation
					: Spec.TextureParameters)
				{
					const int32 Index = static_cast<int32>(&Mutation - Spec.TextureParameters.GetData());
					const FString Field = TEXT("/parameters/") + ::LexToString(Index);
					UTG_Node* const Node = Resolve(Mutation.GetElementId());
					FTG_Name PinName(*Mutation.GetPinName());
					const UTG_Pin* const Pin = Node ? Node->GetInputPin(PinName) : nullptr;
					if (Pin == nullptr || Pin->GetAliasName() != FName(*Mutation.GetAlias()))
					{
						const FString Identity = Mutation.GetElementId() + TEXT(":") + Mutation.GetPinName();
						if (!Node)
							return Fail(TEXT("The TextureGraph parameter node is missing."), Field + TEXT("/node"),
								nullptr, MakeShared<FJsonValueString>(Mutation.GetElementId()), Index, Identity);
						if (!Pin)
							return Fail(TEXT("The TextureGraph parameter input pin is missing."), Field + TEXT("/pin"),
								nullptr, MakeShared<FJsonValueString>(Mutation.GetPinName()), Index, Identity);
						return Fail(TEXT("The TextureGraph parameter alias differs from the requested alias."),
							Field + TEXT("/alias"), MakeShared<FJsonValueString>(Pin->GetAliasName().ToString()),
							MakeShared<FJsonValueString>(Mutation.GetAlias()), Index, Identity);
					}
					if (!Mutation.HasValue()) continue;
					const FString Identity = Mutation.GetElementId() + TEXT(":") + Mutation.GetPinName();
					switch (Mutation.GetValue().GetKind())
					{
					case EAudioMediaGraphTextureGraphValueKind::Number:
					{
						double Actual = 0.0;
						double Expected = 0.0;
						UE::UAI::Contract::FDiagnostic Detail;
						if (!FAudioMediaGraphTextureGraphNativeHook::TryReadPinNumber(*Pin, Actual))
							return Fail(TEXT("The TextureGraph parameter value could not be read as a number."),
								Field + TEXT("/value"), nullptr, Mutation.GetValue().GetNumberValue(), Index, Identity);
						if (!FAudioMediaGraphTextureGraphNativeHook::TryNormalizePinNumber(*Pin, Mutation.GetValue(), Expected, Detail))
						{
							OutDetail = MoveTemp(Detail);
							if (OutDetail.TargetPath.IsEmpty()) OutDetail.TargetPath = Asset.GetPathName();
							OutDetail.OperationIndex = Index;
							OutDetail.CandidateName = Identity;
							if (OutDetail.PropertyPath.IsEmpty()) OutDetail.PropertyPath = Field + TEXT("/value");
							MarkGraphCompoundNativeDiagnostic(OutDetail);
							return false;
						}
						if (Actual != Expected)
							return Fail(TEXT("The TextureGraph parameter number differs from the requested native value."),
								Field + TEXT("/value"), NumberFact(Actual), NumberFact(Expected), Index, Identity);
						break;
					}
					case EAudioMediaGraphTextureGraphValueKind::Boolean:
					{
						bool Actual = false;
						if (!Pin->GetValue(Actual))
							return Fail(TEXT("The TextureGraph parameter value could not be read as a boolean."),
								Field + TEXT("/value"), nullptr, MakeShared<FJsonValueBoolean>(Mutation.GetValue().GetBoolean()), Index, Identity);
						if (Actual != Mutation.GetValue().GetBoolean())
							return Fail(TEXT("The TextureGraph parameter boolean differs from the requested value."),
								Field + TEXT("/value"), MakeShared<FJsonValueBoolean>(Actual),
								MakeShared<FJsonValueBoolean>(Mutation.GetValue().GetBoolean()), Index, Identity);
						break;
					}
					case EAudioMediaGraphTextureGraphValueKind::String:
					{
						FString Actual;
						if (!Pin->GetValue(Actual))
							return Fail(TEXT("The TextureGraph parameter value could not be read as a string."),
								Field + TEXT("/value"), nullptr, MakeShared<FJsonValueString>(Mutation.GetValue().GetString()), Index, Identity);
						if (Actual != Mutation.GetValue().GetString())
							return Fail(TEXT("The TextureGraph parameter string differs from the requested value."),
								Field + TEXT("/value"), MakeShared<FJsonValueString>(Actual),
								MakeShared<FJsonValueString>(Mutation.GetValue().GetString()), Index, Identity);
						break;
					}
					case EAudioMediaGraphTextureGraphValueKind::NumberArray:
					{
						const TArray<double>& Expected = Mutation.GetValue().GetNumberArray();
						FVector4f Actual;
						if (Expected.Num() != 4)
							return Fail(TEXT("The expected TextureGraph parameter vector does not have four components."),
								Field + TEXT("/value/count"), MakeShared<FJsonValueNumber>(Expected.Num()),
								MakeShared<FJsonValueNumber>(4), Index, Identity);
						if (!Pin->GetValue(Actual))
						{
							TArray<TSharedPtr<FJsonValue>> ExpectedValues;
							for (const double Value : Expected) ExpectedValues.Add(NumberFact(Value));
							return Fail(TEXT("The TextureGraph parameter value could not be read as a four-component vector."),
								Field + TEXT("/value"), nullptr, MakeShared<FJsonValueArray>(ExpectedValues), Index, Identity);
						}
						const float Components[] = {Actual.X, Actual.Y, Actual.Z, Actual.W};
						for (int32 Component = 0; Component < 4; ++Component)
						{
							const float ExpectedComponent = static_cast<float>(Expected[Component]);
							if (!FMath::IsNearlyEqual(Components[Component], ExpectedComponent))
								return Fail(TEXT("The TextureGraph parameter vector component differs from the requested native value."),
									Field + TEXT("/value/") + ::LexToString(Component), NumberFact(Components[Component]),
									NumberFact(ExpectedComponent), Index, Identity);
						}
						break;
					}
					default:
						return Fail(TEXT("The TextureGraph parameter readback value kind is unsupported."),
							Field + TEXT("/value/kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Mutation.GetValue().GetKind())),
							MakeShared<FJsonValueString>(TEXT("number, boolean, string or number array")), Index, Identity);
					}
				}
				if (!bComposite) return true;
			}
			if (Spec.Kind
				== EAudioMediaReadbackKind::TextureGraphValidateEvaluateSettle
				|| bComposite)
			{
				if (FindTextureNode(*Graph, TEXT("builtin_output"))) return true;
				return Fail(TEXT("The TextureGraph builtin output is missing after validate/evaluate/settle."),
					TEXT("builtin_output"), nullptr, MakeShared<FJsonValueString>(TEXT("builtin_output node")));
			}
			return Fail(TEXT("The TextureGraph readback does not support this kind."),
				TEXT("readback.kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Spec.Kind)),
				MakeShared<FJsonValueString>(TEXT("TextureGraph nodes, links, parameters, composite or settle")));
		}

		bool TryVerifyReadback(
			const FString& TargetRef,
			const FAudioMediaReadbackSpec& Spec,
			UE::UAI::Contract::FDiagnostic& OutDetail)
		{
			OutDetail.Reset();
			UObject* const Target = LoadObject<UObject>(
				nullptr, *TargetObjectPath(TargetRef));
			if (Target == nullptr)
			{
				OutDetail = ReadbackFailure(TEXT("The persisted audio/media asset could not be loaded."),
					TargetRef, TEXT("asset"), nullptr, MakeShared<FJsonValueString>(TargetObjectPath(TargetRef)));
				return false;
			}
			if (Spec.Kind == EAudioMediaReadbackKind::AssetCreate)
			{
				UClass* const ExpectedClass = LoadObject<UClass>(
					nullptr, *Spec.NativeClassRef);
				if (ExpectedClass == nullptr || !Target->IsA(ExpectedClass))
				{
					OutDetail = ReadbackFailure(ExpectedClass
							? TEXT("The persisted audio/media asset is not an instance of the expected class.")
							: TEXT("The expected audio/media asset class could not be loaded."),
						TargetRef, ExpectedClass ? TEXT("asset.class") : TEXT("spec.native_class_ref"),
						MakeShared<FJsonValueString>(ExpectedClass ? Target->GetClass()->GetPathName() : Spec.NativeClassRef),
						MakeShared<FJsonValueString>(ExpectedClass ? ExpectedClass->GetPathName() : TEXT("loadable asset class")));
					return false;
				}
				if (Spec.AssetKind == TEXT("metasound_source")
					&& Cast<IMetaSoundDocumentInterface>(Target) == nullptr)
				{
					OutDetail = ReadbackFailure(TEXT("The persisted MetaSound asset has no document interface."),
						TargetRef, TEXT("document_interface"), nullptr, MakeShared<FJsonValueString>(TEXT("IMetaSoundDocumentInterface")));
					return false;
				}
				if (Spec.AssetKind == TEXT("texture_graph")
					&& (!Cast<UTextureGraph>(Target)
						|| Cast<UTextureGraph>(Target)->Graph() == nullptr))
				{
					OutDetail = ReadbackFailure(!Cast<UTextureGraph>(Target)
							? TEXT("The persisted TextureGraph asset is not a TextureGraph object.")
							: TEXT("The persisted TextureGraph asset has no native graph."),
						TargetRef, !Cast<UTextureGraph>(Target) ? TEXT("asset.class") : TEXT("graph"),
						!Cast<UTextureGraph>(Target) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Target->GetClass()->GetPathName()))
							: TSharedPtr<FJsonValue>(),
						MakeShared<FJsonValueString>(!Cast<UTextureGraph>(Target)
							? UTextureGraph::StaticClass()->GetPathName() : TEXT("TextureGraph native graph")));
					return false;
				}
				return true;
			}
			if (UMetaSoundSource* const Source = Cast<UMetaSoundSource>(Target))
			{
				return VerifyMetaSoundReadback(*Source, Spec, OutDetail);
			}
			if (UTextureGraph* const TextureGraph = Cast<UTextureGraph>(Target))
			{
				return VerifyTextureGraphReadback(*TextureGraph, Spec, OutDetail);
			}
			USoundCue* const Cue = Cast<USoundCue>(Target);
			if (!Cue)
			{
				OutDetail = ReadbackFailure(TEXT("The persisted audio/media target has no supported readback implementation."),
					TargetRef, TEXT("asset.class"), MakeShared<FJsonValueString>(Target->GetClass()->GetPathName()),
					MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
						MakeShared<FJsonValueString>(UMetaSoundSource::StaticClass()->GetPathName()),
						MakeShared<FJsonValueString>(UTextureGraph::StaticClass()->GetPathName()),
						MakeShared<FJsonValueString>(USoundCue::StaticClass()->GetPathName())}));
				return false;
			}
			return VerifySoundCueReadback(*Cue, Spec, OutDetail);
		}

		class FAudioMediaReadbackOracle final
			: public IIndependentReadbackOracle
		{
		public:
			FAudioMediaReadbackOracle(
				FString InTargetRef,
				FAudioMediaReadbackSpec InSpec)
				: TargetRef(MoveTemp(InTargetRef))
				, Spec(MoveTemp(InSpec))
			{
			}

			const FString& GetOracleId() const override
			{
				return ReadbackOracleId;
			}

			bool TryRead(
				const FIndependentReadbackRequest& Request,
				FUeAgentResourceTracker& Resources,
				FIndependentReadbackObservation& OutObservation,
				FVerificationError& OutError) override
			{
				UE::UAI::Contract::FDiagnostic UnloadDetail;
				if (Request.GetOracleId() != ReadbackOracleId)
				{
					OutError = AudioMediaFailure(TEXT("audio_media_readback_unload_failed"),
						TEXT("The independent readback request uses a different oracle."),
						TargetRef, TEXT("request.oracle_id"), MakeShared<FJsonValueString>(Request.GetOracleId()),
						MakeShared<FJsonValueString>(ReadbackOracleId));
					OutError.Stage = TEXT("native_readback");
					return false;
				}
				if (Request.GetSubjectRef() != TargetRef)
				{
					OutError = AudioMediaFailure(TEXT("audio_media_readback_unload_failed"),
						TEXT("The independent readback request targets a different asset."),
						TargetRef, TEXT("request.subject_ref"), MakeShared<FJsonValueString>(Request.GetSubjectRef()),
						MakeShared<FJsonValueString>(TargetRef));
					OutError.Stage = TEXT("native_readback");
					return false;
				}
				if (!UeAgentAuthoringOperationApply::TryReloadPersistedPackage(TargetRef, UnloadDetail))
				{
					OutError = MoveTemp(UnloadDetail);
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
					return false;
				}
				UE::UAI::Contract::FDiagnostic Detail;
				if (!TryVerifyReadback(TargetRef, Spec, Detail))
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(Detail, TargetRef);
					return true;
				}
				FString Revision;
				if (!TryReadPackageAssetRevision(TargetRef, Revision, Detail))
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(Detail, TargetRef);
					return true;
				}
				const TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
				Actual->SetBoolField(TEXT("verified"), true);
				Actual->SetStringField(TEXT("revision"), Revision);
				FUeAgentCanonicalValuePtr ActualValue;
				FGraphCompoundRuntimeError CanonicalError;
				if (!TryCanonicalObject(
						Actual, Resources, ActualValue, CanonicalError))
				{
					OutError = MoveTemp(CanonicalError);
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
					return false;
				}
				OutObservation = FIndependentReadbackObservation::Available(
					ActualValue.ToSharedRef(),
					TEXT("audio-media-readback:") + TargetRef + TEXT(":")
						+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
				return true;
			}

		private:
			FString TargetRef;
			FAudioMediaReadbackSpec Spec;
		};

		bool TrySerializeResult(
			const FString& TargetRef,
			const FOperationLifecycleReceipt& Receipt,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			const TArray<FString>& Phases = Receipt.GetPhaseHistory();
			const bool bMutationApplied = Receipt.DidMutationStart()
				&& Phases.Contains(LexToString(
					EOperationLifecycleStage::DomainExecute));
			const bool bCompileSettled = Phases.Contains(LexToString(
				EOperationLifecycleStage::Settle));
			const bool bSaved = Phases.Contains(LexToString(
				EOperationLifecycleStage::Persist));
			const bool bReloaded = Phases.Contains(LexToString(
				EOperationLifecycleStage::IndependentVerify));
			auto FailReceipt = [&](const FString& Reason, const FString& Field,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
			{
				OutError = AudioMediaFailure(TEXT("graph_compound_audio_media_lifecycle_receipt_invalid"),
					Reason, TargetRef, Field, MoveTemp(Actual), MoveTemp(Expected));
				OutError.MutationMayHaveStarted = Receipt.DidMutationStart();
				return false;
			};
			if (Receipt.GetStatus() != EOperationLifecycleStatus::Succeeded)
			{
				if (!Receipt.GetDiagnostic().IsEmpty())
				{
					OutError = Receipt.GetDiagnostic();
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
					return false;
				}
				return FailReceipt(TEXT("The audio/media lifecycle receipt does not report success."),
					TEXT("receipt.status"), MakeShared<FJsonValueString>(LexToString(Receipt.GetStatus())),
					MakeShared<FJsonValueString>(LexToString(EOperationLifecycleStatus::Succeeded)));
			}
			if (!bMutationApplied)
				return FailReceipt(Receipt.DidMutationStart()
						? TEXT("The successful audio/media receipt has no domain-execute phase.")
						: TEXT("The successful audio/media receipt reports that mutation never started."),
					Receipt.DidMutationStart() ? TEXT("receipt.domain_execute.present") : TEXT("receipt.mutation_started"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			if (!bCompileSettled)
				return FailReceipt(TEXT("The successful audio/media receipt has no settle phase."),
					TEXT("receipt.settle.present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			if (!bSaved)
				return FailReceipt(TEXT("The successful audio/media receipt has no persist phase."),
					TEXT("receipt.persist.present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			if (!bReloaded)
				return FailReceipt(TEXT("The successful audio/media receipt has no independent-verify phase."),
					TEXT("receipt.independent_verify.present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			if (Receipt.GetPersistenceReceiptId().IsEmpty())
				return FailReceipt(TEXT("The successful audio/media receipt has no persistence receipt id."),
					TEXT("receipt.persistence_receipt_id.present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
			Object->SetStringField(TEXT("target_ref"), TargetRef);
			Object->SetBoolField(TEXT("mutation_applied"), bMutationApplied);
			Object->SetBoolField(TEXT("compile_settled"), bCompileSettled);
			Object->SetBoolField(TEXT("saved"), bSaved);
			Object->SetBoolField(TEXT("reloaded"), bReloaded);
			FString Canonical;
			UE::UAI::Contract::FDiagnostic JsonError;
			if (!Json::TryCanonicalize(Object, Canonical, JsonError))
			{
				SetError(OutError, JsonError, TargetRef);
				MarkGraphCompoundNativeDiagnostic(OutError);
				return false;
			}
			OutResult = FUeAgentContractValue(MoveTemp(Canonical));
			return true;
		}

		class FAudioMediaLifecycleContext final
			: public IGraphCompoundAudioMediaLifecycleContext
		{
		public:
			FAudioMediaLifecycleContext(
				FAudioMediaGraphRouteProfile InProfile,
				FGraphCompoundAudioMediaOperationRequest InRequest,
				FPlanningPreflightPlanRef InPlanningPlan,
				FUeAgentCallerContext InCallerContext,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> InResources,
				TSharedRef<IUeAgentCancellationSignal> InCancellation,
				FAudioMediaReadbackSpec InSpec,
				FString InJournalRoot)
				: Profile(MoveTemp(InProfile))
				, TargetRequest(InRequest.GetTarget())
				, PlanningPlan(MoveTemp(InPlanningPlan))
				, CallerContext(MoveTemp(InCallerContext))
				, Resources(MoveTemp(InResources))
				, Cancellation(MoveTemp(InCancellation))
				, Spec(MoveTemp(InSpec))
				, JournalRoot(MoveTemp(InJournalRoot))
			{
			}

			FAudioMediaLifecycleContext(
				FAudioMediaGraphRouteProfile InProfile,
				FGraphCompoundAudioMediaTargetRequest InTarget,
				FPlanningPreflightPlanRef InPlanningPlan,
				FUeAgentCallerContext InCallerContext,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> InResources,
				TSharedRef<IUeAgentCancellationSignal> InCancellation,
				FAudioMediaReadbackSpec InSpec,
				FString InJournalRoot)
				: Profile(MoveTemp(InProfile)), TargetRequest(MoveTemp(InTarget))
				, PlanningPlan(MoveTemp(InPlanningPlan))
				, CallerContext(MoveTemp(InCallerContext))
				, Resources(MoveTemp(InResources))
				, Cancellation(MoveTemp(InCancellation))
				, Spec(MoveTemp(InSpec)), JournalRoot(MoveTemp(InJournalRoot))
			{
			}

			bool IsCreate() const
			{
				return Profile.GetInputKind()
					== EAudioMediaGraphInputKind::AssetCreate;
			}

			bool IsMetaSound() const
			{
				return Profile.GetInputKind()
						>= EAudioMediaGraphInputKind::MetaSoundInterface
					&& Profile.GetInputKind()
						<= EAudioMediaGraphInputKind::MetaSoundCompileSettle;
			}

			bool IsSoundCue() const
			{
				return Profile.GetInputKind()
						>= EAudioMediaGraphInputKind::SoundCueNode
					&& Profile.GetInputKind()
						<= EAudioMediaGraphInputKind::SoundCueCompileSettle;
			}

			bool IsTextureGraph() const
			{
				return Profile.GetInputKind()
						>= EAudioMediaGraphInputKind::TextureGraphNode
					&& Profile.GetInputKind()
						<= EAudioMediaGraphInputKind::
							TextureGraphValidateEvaluateSettle;
			}

			bool TryCaptureCurrent(
				FUeAgentCanonicalSnapshotPtr& OutSnapshot,
				FGraphCompoundRuntimeError& OutError)
			{
				OutSnapshot.Reset();
				if (IsCreate())
				{
					return TryCreateAbsentSnapshot(
						TargetRequest, *Resources, OutSnapshot, OutError);
				}
				if (IsMetaSound())
				{
					return TryCaptureMetaSoundSnapshot(
						TargetRequest, *Resources, OutSnapshot, OutError);
				}
				if (IsSoundCue())
				{
					return TryCaptureSoundCueSnapshot(
						TargetRequest, *Resources, OutSnapshot, OutError);
				}
				if (IsTextureGraph())
				{
					return TryCaptureTextureGraphSnapshot(
						TargetRequest, *Resources, OutSnapshot, OutError);
				}
				OutError = AudioMediaFailure(TEXT("graph_compound_audio_media_lifecycle_kind_invalid"),
					TEXT("The audio/media lifecycle cannot capture this route input kind."),
					TargetRequest.GetTargetRef(), TEXT("profile.input_kind"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Profile.GetInputKind())),
					MakeShared<FJsonValueString>(TEXT("asset-create, MetaSound, SoundCue or TextureGraph mutation")));
				OutError.SubjectId = Profile.GetCapabilityId();
				return false;
			}

			bool TryBuildResult(
				const FOperationLifecycleReceipt& Receipt,
				FUeAgentContractValue& OutResult,
				FGraphCompoundRuntimeError& OutError) const override
			{
				if (Receipt.GetStatus() != EOperationLifecycleStatus::Succeeded)
				{
					OutError = Receipt.GetDiagnostic();
					if (OutError.IsEmpty())
						OutError = AudioMediaFailure(TEXT("graph_compound_audio_media_terminal_result_invalid"),
							TEXT("The audio/media terminal receipt does not report success."),
							TargetRequest.GetTargetRef(), TEXT("receipt.status"),
							MakeShared<FJsonValueString>(LexToString(Receipt.GetStatus())),
							MakeShared<FJsonValueString>(LexToString(EOperationLifecycleStatus::Succeeded)));
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRequest.GetTargetRef();
					return false;
				}
				if (Revision.IsEmpty())
				{
					OutError = AudioMediaFailure(TEXT("graph_compound_audio_media_terminal_result_invalid"),
						TEXT("The audio/media operation has no resulting asset revision."),
						TargetRequest.GetTargetRef(), TEXT("result.revision.present"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				return TrySerializeResult(
					TargetRequest.GetTargetRef(), Receipt,
					OutResult, OutError);
			}

			FAudioMediaGraphRouteProfile Profile;
			FGraphCompoundAudioMediaTargetRequest TargetRequest;
			FPlanningPreflightPlanRef PlanningPlan;
			FUeAgentCallerContext CallerContext;
			TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources;
			TSharedRef<IUeAgentCancellationSignal> Cancellation;
			FAudioMediaReadbackSpec Spec;
			FString JournalRoot;
			FUeAgentCanonicalSnapshotPtr CurrentSnapshot;
			FPackageAssetDurableBackend DurableBackend;
			FAssetDurableSessionPtr DurableSession;
			FDurableReceipt DurableReceipt;
			int64 FencingToken = 0;
			UPackage* Package = nullptr;
			UObject* Target = nullptr;
			FString Revision;
		};

		FOperationLifecycleError AudioLifecycleFailure(const TCHAR* Code, const TCHAR* Reason,
			const EOperationLifecycleStage Stage, const FString& Target, const FString& Path,
			TSharedPtr<FJsonValue> Actual = nullptr, TSharedPtr<FJsonValue> Expected = nullptr)
		{
			auto Diagnostic = FOperationLifecycleError::Failure(Code, Reason, LexToString(Stage));
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.TargetPath = Target;
			Diagnostic.PropertyPath = Diagnostic.SemanticPointer = Diagnostic.Source.JsonPath = Path;
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			return Diagnostic;
		}

		class FAudioMediaVersionFence final : public IOperationVersionFencePort
		{
		public:
			explicit FAudioMediaVersionFence(
				TSharedRef<FAudioMediaLifecycleContext> InContext)
				: Context(MoveTemp(InContext))
			{
			}

			bool Check(
				const FOperationLifecyclePlan&,
				const EOperationLifecycleStage Stage,
				bool& bOutCurrent,
				FOperationLifecycleError& OutError) override
			{
				bOutCurrent = false;
				if (Context->Cancellation->IsCancelled())
				{
					OutError = AudioLifecycleFailure(TEXT("graph_compound_audio_media_cancelled"), TEXT("The operation was cancelled before the version check."),
						Stage, Context->TargetRequest.GetTargetRef(), TEXT("/cancellation_requested"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
					return false;
				}
				if (Stage == EOperationLifecycleStage::PrePersistFence)
				{
					bOutCurrent = Context->Target != nullptr
						&& Context->Package != nullptr
						&& Context->DurableSession.IsValid();
					if (!bOutCurrent)
					{
						OutError = AudioLifecycleFailure(TEXT("graph_compound_audio_media_pre_persist_fence_failed"),
							!Context->Target ? TEXT("The native target object is unavailable before persistence.")
								: !Context->Package ? TEXT("The native target package is unavailable before persistence.") : TEXT("The prepared durability session is unavailable before persistence."),
							Stage, Context->TargetRequest.GetTargetRef(), !Context->Target ? TEXT("/target") : !Context->Package ? TEXT("/package") : TEXT("/durability_session"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					}
					return true;
				}

				FGraphCompoundRuntimeError RuntimeError;
				if (!Context->TryCaptureCurrent(Context->CurrentSnapshot, RuntimeError))
				{
					OutError = RuntimeError;
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Context->TargetRequest.GetTargetRef();
					return false;
				}
				if (!Context->Spec.ExpectedRevision.IsEmpty()
					&& Context->Spec.ExpectedRevision
						!= Context->CurrentSnapshot->GetRevision().GetGeneration())
				{
					OutError = AudioLifecycleFailure(TEXT("graph_compound_audio_media_revision_conflict"), TEXT("The current asset revision differs from the expected revision."),
						Stage, Context->TargetRequest.GetTargetRef(), TEXT("/expected_revision"));
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Context->CurrentSnapshot->GetRevision().GetGeneration());
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Context->Spec.ExpectedRevision);
					return true;
				}
				FPlanningPreflightError RevalidateError;
				bOutCurrent = TryRevalidatePlan(
					Context->PlanningPlan,
					Context->CurrentSnapshot.ToSharedRef(),
					Context->CallerContext,
					*Context->Resources,
					RevalidateError);
				if (!bOutCurrent)
				{
					OutError = RevalidateError;
				}
				return true;
			}

		private:
			TSharedRef<FAudioMediaLifecycleContext> Context;
		};

		class FAudioMediaMutation final : public IOperationDomainMutationPort
		{
		public:
			explicit FAudioMediaMutation(
				TSharedRef<FAudioMediaLifecycleContext> InContext)
				: Context(MoveTemp(InContext))
			{
			}

			bool Execute(
				const FOperationLifecyclePlan&,
				FOperationMutationFacts& OutFacts,
				FOperationLifecycleError& OutError) override
			{
				const FString& TargetRef = Context->TargetRequest.GetTargetRef();
				const FString PackageName = TargetPackageName(TargetRef);
				FGraphCompoundRuntimeError RuntimeError;
				static int64 NextFencingToken = 10000;
				Context->FencingToken = ++NextFencingToken;
				const FString JournalOwner =
					TEXT("graph-compound-audio-media:")
					+ Context->PlanningPlan->GetRequestId();
				const FAssetDurableRequest DurableRequest(
					Context->PlanningPlan,
					{FAssetDurableTarget(
						Context->PlanningPlan->GetChangeSet().GetChanges()[0]
							.GetIdentity(),
						PackageName,
						EPackageHeaderKind::Asset,
						EAssetDurableIntent::Save,
						ERollbackGrade::DurableBackup)},
					Context->FencingToken,
					JournalOwner,
					EAtomicityMode::CompensatedSaga);
				FAssetDurableError DurableError;
				if (!TryPrepareAssetDurability(
						DurableRequest,
						Context->DurableBackend,
						Context->JournalRoot,
						*Context->Resources,
						Context->DurableSession,
						DurableError))
				{
					OutError = DurableError;
					return false;
				}

				if (Context->IsCreate())
				{
					Context->Package = CreatePackage(*PackageName);
					Context->Target = Context->Package
						? TryCreateAsset(
							Context->Spec, TargetRef,
							*Context->Package, RuntimeError)
						: nullptr;
					if (!Context->Package)
						RuntimeError = AudioLifecycleFailure(TEXT("graph_compound_audio_media_native_failed"),
							TEXT("The destination package could not be created for the audio/media asset."),
							EOperationLifecycleStage::DomainExecute, TargetRef, TEXT("/package"), nullptr,
							MakeShared<FJsonValueString>(PackageName));
					if (Context->Target)
					{
						FAssetRegistryModule::AssetCreated(Context->Target);
					}
				}
				else
				{
					Context->Target = LoadObject<UObject>(
						nullptr, *TargetObjectPath(TargetRef));
					Context->Package = Context->Target
						? Context->Target->GetOutermost() : nullptr;
					bool bApplied = false;
					if (UMetaSoundSource* Source =
						Cast<UMetaSoundSource>(Context->Target))
					{
						bApplied = TryApplyMetaSound(
							*Source, Context->Spec, MetaSoundHook, RuntimeError);
					}
					else if (USoundCue* Cue = Cast<USoundCue>(Context->Target))
					{
						bApplied = TryApplySoundCue(
							*Cue, Context->Spec, SoundCueHook, RuntimeError);
					}
					else if (UTextureGraph* Graph =
						Cast<UTextureGraph>(Context->Target))
					{
						bApplied = TryApplyTextureGraph(
							*Graph, Context->Spec, TextureGraphHook, RuntimeError);
					}
					else
					{
						RuntimeError = AudioLifecycleFailure(TEXT("graph_compound_audio_media_native_failed"),
							Context->Target ? TEXT("The loaded target class has no audio/media mutation implementation.")
								: TEXT("The audio/media mutation target could not be loaded."),
							EOperationLifecycleStage::DomainExecute, TargetRef,
							Context->Target ? TEXT("/target/class") : TEXT("/target"),
							Context->Target ? MakeShared<FJsonValueString>(Context->Target->GetClass()->GetPathName()) : TSharedPtr<FJsonValue>(),
							MakeShared<FJsonValueString>(TEXT("MetaSoundSource, SoundCue or TextureGraph object")));
					}
					if (!bApplied)
					{
						Context->Target = nullptr;
						Context->Package = nullptr;
					}
				}

				OutFacts = FOperationMutationFacts(
					TEXT("graph-compound-audio-media:")
						+ Context->PlanningPlan->GetRequestId(),
					{TargetRef},
					true);
				if (!Context->Target || !Context->Package)
				{
					if (!RuntimeError.IsEmpty()) OutError = RuntimeError;
					else
					{
						OutError = AudioLifecycleFailure(TEXT("graph_compound_audio_media_native_failed"),
							!Context->Target ? TEXT("The native audio operation did not provide a target object.")
								: TEXT("The native audio operation did not provide a target package."),
							EOperationLifecycleStage::DomainExecute, TargetRef,
							!Context->Target ? TEXT("/target") : TEXT("/package"), nullptr,
							MakeShared<FJsonValueString>(!Context->Target ? TEXT("native target object") : TEXT("native target package")));
					}
					return false;
				}
				Context->Package->MarkPackageDirty();
				return OutFacts.TryValidate(OutError);
			}

			bool Rollback(
				const FOperationLifecyclePlan&,
				const FOperationMutationFacts&,
				bool& bOutConfirmed,
				FOperationLifecycleError& OutError) override
			{
				bOutConfirmed = false;
				if (!Context->DurableSession.IsValid())
				{
					OutError = AudioLifecycleFailure(TEXT("graph_compound_audio_media_rollback_session_missing"), TEXT("Rollback cannot proceed because the durability session is missing."),
						EOperationLifecycleStage::Rollback, Context->TargetRequest.GetTargetRef(), TEXT("/durability_session"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				Context->Target = nullptr;
				Context->Package = nullptr;
				UE::UAI::Contract::FDiagnostic Detail;
				bOutConfirmed = TryRollback(
					Context->TargetRequest.GetTargetRef(),
					Context->IsCreate(),
					Context->DurableSession.ToSharedRef(),
					Context->DurableBackend,
					Context->FencingToken,
					*Context->Resources,
					Detail);
				if (!bOutConfirmed)
				{
					OutError = MoveTemp(Detail);
				}
				return bOutConfirmed;
			}

		private:
			TSharedRef<FAudioMediaLifecycleContext> Context;
			FAudioMediaGraphMetaSoundNativeHook MetaSoundHook;
			FAudioMediaGraphSoundCueNativeHook SoundCueHook;
			FAudioMediaGraphTextureGraphNativeHook TextureGraphHook;
		};

		class FAudioMediaStability final : public IOperationStabilityPort
		{
		public:
			explicit FAudioMediaStability(
				TSharedRef<FAudioMediaLifecycleContext> InContext)
				: Context(MoveTemp(InContext))
			{
			}
			bool Settle(
				const FOperationLifecyclePlan&,
				const FOperationMutationFacts&,
				FOperationLifecycleError& OutError) override
			{
				if (!Context->Target)
				{
					OutError = AudioLifecycleFailure(TEXT("graph_compound_audio_media_settle_failed"),
						TEXT("The native target object is unavailable for the stability check."), EOperationLifecycleStage::Settle,
						Context->TargetRequest.GetTargetRef(), TEXT("/target_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				UE::UAI::Contract::FDiagnostic Detail;
				bool bStable = true;
				if (!Context->IsCreate())
				{
					if (UMetaSoundSource* Source =
						Cast<UMetaSoundSource>(Context->Target))
					{
						bStable = VerifyMetaSoundReadback(
							*Source, Context->Spec, Detail);
					}
					else if (USoundCue* Cue = Cast<USoundCue>(Context->Target))
					{
						bStable = VerifySoundCueReadback(
							*Cue, Context->Spec, Detail);
					}
					else if (UTextureGraph* Graph =
						Cast<UTextureGraph>(Context->Target))
					{
						bStable = VerifyTextureGraphReadback(
							*Graph, Context->Spec, Detail);
					}
					else
					{
						OutError = AudioLifecycleFailure(TEXT("graph_compound_audio_media_settle_failed"),
							TEXT("The target class has no audio-media stability verifier."), EOperationLifecycleStage::Settle,
							Context->TargetRequest.GetTargetRef(), TEXT("/target_class"), MakeShared<FJsonValueString>(Context->Target->GetClass()->GetPathName()),
							MakeShared<FJsonValueString>(TEXT("MetaSoundSource, SoundCue or TextureGraph")));
						return false;
					}
				}
				if (!bStable)
				{
					OutError = MoveTemp(Detail);
				}
				return bStable;
			}
		private:
			TSharedRef<FAudioMediaLifecycleContext> Context;
		};

		class FAudioMediaPersistence final : public IOperationPersistencePort
		{
		public:
			explicit FAudioMediaPersistence(
				TSharedRef<FAudioMediaLifecycleContext> InContext)
				: Context(MoveTemp(InContext))
			{
			}
			bool Persist(
				const FOperationLifecyclePlan&,
				const FOperationMutationFacts&,
				FString& OutReceipt,
				FOperationLifecycleError& OutError) override
			{
				FAssetDurableError DurableError;
				const FAssetDurableSessionRef Prepared =
					Context->DurableSession.ToSharedRef();
				if (!TryPersistAssetDurabilityForVerification(
						Prepared,
						Context->DurableBackend,
						Context->FencingToken,
						*Context->Resources,
						Context->DurableReceipt,
						DurableError))
				{
					OutError = DurableError;
					return false;
				}
				Context->Package->SetDirtyFlag(false);
				Context->Target = nullptr;
				Context->Package = nullptr;
				const bool bRevisionRead = TryReadPackageAssetRevision(
					Context->TargetRequest.GetTargetRef(), Context->Revision, OutError);
				Context->DurableSession = PersistedSession(
					Prepared, Context->DurableReceipt);
				OutReceipt = Context->DurableReceipt.GetJournalId();
				if (!bRevisionRead) return false;
				if (OutReceipt.IsEmpty())
				{
					OutError = AudioLifecycleFailure(TEXT("graph_compound_audio_media_persistence_incomplete"),
						TEXT("Persistence returned no journal receipt identifier."),
						EOperationLifecycleStage::Persist, Context->TargetRequest.GetTargetRef(), TEXT("/persistence_receipt"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				return true;
			}
		private:
			TSharedRef<FAudioMediaLifecycleContext> Context;
		};

		class FAudioMediaVerification final
			: public IOperationIndependentVerificationPort
		{
		public:
			explicit FAudioMediaVerification(
				TSharedRef<FAudioMediaLifecycleContext> InContext)
				: Context(MoveTemp(InContext))
			{
			}
			bool Verify(
				const FOperationLifecyclePlan&,
				const FOperationMutationFacts&,
				const FString& PersistenceReceiptId,
				bool& bOutEquivalent,
				FOperationLifecycleError& OutError) override
			{
				bOutEquivalent = false;
				const TSharedRef<FJsonObject> ExpectedObject =
					MakeShared<FJsonObject>();
				ExpectedObject->SetBoolField(TEXT("verified"), true);
				ExpectedObject->SetStringField(
					TEXT("revision"), Context->Revision);
				FUeAgentCanonicalValuePtr Expected;
				FGraphCompoundRuntimeError RuntimeError;
				if (!TryCanonicalObject(ExpectedObject, *Context->Resources, Expected, RuntimeError))
				{
					OutError = RuntimeError;
					MarkGraphCompoundNativeDiagnostic(OutError);
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Context->TargetRequest.GetTargetRef();
					return false;
				}
				TSharedPtr<FIndependentReadbackService> Readback;
				FVerificationError VerifyError;
				const TSharedRef<FAudioMediaReadbackOracle, ESPMode::ThreadSafe> Oracle =
					MakeShared<FAudioMediaReadbackOracle, ESPMode::ThreadSafe>(
						Context->TargetRequest.GetTargetRef(), Context->Spec);
				FIndependentReadbackOutcome Outcome;
				const bool bReadbackCreated = FIndependentReadbackService::TryCreate({Oracle}, Readback, VerifyError);
				if (!bReadbackCreated
					|| !Readback->TryObserve(
						FIndependentReadbackRequest(
							TEXT("graph-compound-audio-media:") + Context->PlanningPlan->GetRequestId(),
							ReadbackOracleId, Context->TargetRequest.GetTargetRef(), Expected.ToSharedRef()),
						*Context->Resources, Outcome, VerifyError))
				{
					OutError = bReadbackCreated ? Outcome.GetFailureDiagnostic(VerifyError) : VerifyError;
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Context->TargetRequest.GetTargetRef();
					return false;
				}
				FAssetDurableError DurableError;
				if (!TryAcknowledgeAssetDurability(
						Context->DurableSession.ToSharedRef(),
						FDurableVerificationAcknowledgement(
							PersistenceReceiptId,
							Context->PlanningPlan->GetFingerprint(),
							Context->DurableSession->GetInventory()
								.GetInventoryHash(),
							Outcome.GetEvidenceId(),
							TEXT("graph-compound-audio-media:")
								+ Context->PlanningPlan->GetRequestId(),
							Context->FencingToken, Outcome.GetStatus() == EVerificationStatus::Verified),
						Context->FencingToken,
						*Context->Resources,
						DurableError))
				{
					OutError = DurableError;
					return false;
				}
				bOutEquivalent = Outcome.GetStatus() == EVerificationStatus::Verified;
				return true;
			}
		private:
			TSharedRef<FAudioMediaLifecycleContext> Context;
		};

		class FAudioMediaProductionEnvironment final
			: public IGraphCompoundAudioMediaEnvironment
		{
		public:
			explicit FAudioMediaProductionEnvironment(
				const FGraphCompoundAudioMediaProductionConfig& Config)
				: JournalRoot(Config.DurableJournalRoot.IsEmpty()
					? FPaths::ProjectSavedDir()
						/ TEXT("UeAgentInterface/GraphCompoundDurable")
					: Config.DurableJournalRoot)
			{
			}

			bool TryCaptureSnapshot(
				const FAudioMediaGraphRouteProfile& Profile,
				const FGraphCompoundAudioMediaTargetRequest& Request,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FUeAgentCanonicalSnapshotPtr& OutSnapshot,
				FGraphCompoundRuntimeError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				if (!IsInGameThread() || Cancellation->IsCancelled())
				{
					const bool bCancelled = Cancellation->IsCancelled();
					OutError = AudioMediaFailure(bCancelled
							? TEXT("graph_compound_audio_media_cancelled") : TEXT("graph_compound_audio_media_thread_invalid"),
						bCancelled ? TEXT("The audio/media snapshot capture was cancelled.")
							: TEXT("Audio/media snapshot capture must run on the game thread."),
						Request.GetTargetRef(), bCancelled ? TEXT("cancellation.requested") : TEXT("execution.game_thread"),
						MakeShared<FJsonValueBoolean>(bCancelled), MakeShared<FJsonValueBoolean>(!bCancelled));
					OutError.SubjectId = Profile.GetCapabilityId();
					return false;
				}
				if (Profile.GetInputKind() == EAudioMediaGraphInputKind::AssetCreate)
				{
					return TryCreateAbsentSnapshot(
						Request, *Resources, OutSnapshot, OutError);
				}
				if (Profile.GetInputKind()
						>= EAudioMediaGraphInputKind::MetaSoundInterface
					&& Profile.GetInputKind()
						<= EAudioMediaGraphInputKind::MetaSoundCompileSettle)
				{
					return TryCaptureMetaSoundSnapshot(
						Request, *Resources, OutSnapshot, OutError);
				}
				if (Profile.GetInputKind() >= EAudioMediaGraphInputKind::SoundCueNode
					&& Profile.GetInputKind()
						<= EAudioMediaGraphInputKind::SoundCueCompileSettle)
				{
					return TryCaptureSoundCueSnapshot(
						Request, *Resources, OutSnapshot, OutError);
				}
				if (Profile.GetInputKind() >= EAudioMediaGraphInputKind::TextureGraphNode
					&& Profile.GetInputKind()
						<= EAudioMediaGraphInputKind::TextureGraphValidateEvaluateSettle)
				{
					return TryCaptureTextureGraphSnapshot(
						Request, *Resources, OutSnapshot, OutError);
				}
				OutError = AudioMediaFailure(TEXT("graph_compound_audio_media_production_binding_unimplemented"),
					TEXT("The audio/media production environment has no snapshot binding for this input kind."),
					Request.GetTargetRef(), TEXT("profile.input_kind"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Profile.GetInputKind())),
					MakeShared<FJsonValueString>(TEXT("asset-create, MetaSound, SoundCue or TextureGraph mutation")));
				OutError.SubjectId = Profile.GetCapabilityId();
				return false;
			}

			bool TryExecuteOperation(
				const FAudioMediaGraphRouteProfile& Profile,
				const FGraphCompoundAudioMediaOperationRequest& Request,
				const FPlanningPreflightPlanRef& Plan,
				const FUeAgentCallerContext& CallerContext,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FUeAgentContractValue& OutResult,
				FGraphCompoundRuntimeError& OutError) override
			{
				OutResult = FUeAgentContractValue(TEXT("{}"));
				OutError = {};
				{
					FGraphCompoundAudioMediaLifecycleInvocation Invocation;
					if (!TryCreateOperationLifecycleInvocation(
							Profile,
							Request,
							Plan,
							CallerContext,
							MoveTemp(Resources),
							Cancellation,
							Invocation,
							OutError))
					{
						return false;
					}
					FOperationLifecycleReceipt Receipt;
					if (!Invocation.GetService()->Run(
							*Invocation.GetPlan(),
							*Invocation.GetAuthorization(),
							Receipt))
					{
						SetError(OutError, Receipt.GetDiagnostic(), Request.GetTargetRef());
						OutError.TerminalStatus = LexToString(Receipt.GetStatus());
						OutError.MutationStarted = Receipt.DidMutationStart();
						OutError.bRetrySafe = Receipt.IsRetrySafe();
						return false;
					}
					return Invocation.GetContext()->TryBuildResult(
						Receipt, OutResult, OutError);
				}
			}
			bool TryCreateOperationLifecycleInvocation(
				const FAudioMediaGraphRouteProfile& Profile,
				const FGraphCompoundAudioMediaOperationRequest& Request,
				const FPlanningPreflightPlanRef& Plan,
				const FUeAgentCallerContext& CallerContext,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FGraphCompoundAudioMediaLifecycleInvocation& OutInvocation,
				FGraphCompoundRuntimeError& OutError) override
			{
				OutInvocation = {};
				OutError = {};
				const bool bSupported = Profile.GetInputKind()
					== EAudioMediaGraphInputKind::AssetCreate
					|| (Profile.GetInputKind()
						>= EAudioMediaGraphInputKind::MetaSoundInterface
						&& Profile.GetInputKind()
							<= EAudioMediaGraphInputKind::MetaSoundCompileSettle)
					|| (Profile.GetInputKind()
						>= EAudioMediaGraphInputKind::SoundCueNode
						&& Profile.GetInputKind()
							<= EAudioMediaGraphInputKind::SoundCueCompileSettle)
					|| (Profile.GetInputKind()
						>= EAudioMediaGraphInputKind::TextureGraphNode
						&& Profile.GetInputKind()
							<= EAudioMediaGraphInputKind::
								TextureGraphValidateEvaluateSettle);
				if (!bSupported || !IsInGameThread()
					|| Cancellation->IsCancelled()
					|| Plan->GetDescriptorIdentity()
						!= Profile.GetExecutionContract().GetDescriptorIdentity()
					|| Plan->GetChangeSet().GetChanges().IsEmpty())
				{
					const bool bCancelled = Cancellation->IsCancelled();
					auto FailPlan = [&](const FString& Reason, const FString& Field,
						TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
					{
						OutError = AudioMediaFailure(bCancelled ? TEXT("graph_compound_audio_media_cancelled")
								: TEXT("graph_compound_audio_media_plan_invalid"),
							Reason, Request.GetTargetRef(), Field, MoveTemp(Actual), MoveTemp(Expected));
						return false;
					};
					if (bCancelled)
						return FailPlan(TEXT("The audio/media operation was cancelled before lifecycle creation."),
							TEXT("cancellation.requested"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
					if (!bSupported)
						return FailPlan(TEXT("The audio/media lifecycle does not support this route input kind."),
							TEXT("profile.input_kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Profile.GetInputKind())),
							MakeShared<FJsonValueString>(TEXT("asset-create, MetaSound, SoundCue or TextureGraph mutation")));
					if (!IsInGameThread())
						return FailPlan(TEXT("Audio/media lifecycle creation must run on the game thread."),
							TEXT("execution.game_thread"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					const FUeAgentDescriptorIdentity& ActualDescriptor = Plan->GetDescriptorIdentity();
					const FUeAgentDescriptorIdentity& ExpectedDescriptor =
						Profile.GetExecutionContract().GetDescriptorIdentity();
					if (ActualDescriptor != ExpectedDescriptor)
					{
						if (ActualDescriptor.GetDescriptorId() != ExpectedDescriptor.GetDescriptorId())
							return FailPlan(TEXT("The plan descriptor id does not match the route descriptor."),
								TEXT("descriptor_identity.descriptor_id"),
								MakeShared<FJsonValueString>(ActualDescriptor.GetDescriptorId()),
								MakeShared<FJsonValueString>(ExpectedDescriptor.GetDescriptorId()));
						if (ActualDescriptor.GetVersion() != ExpectedDescriptor.GetVersion())
							return FailPlan(TEXT("The plan descriptor version does not match the route descriptor."),
								TEXT("descriptor_identity.version"),
								MakeShared<FJsonValueNumber>(ActualDescriptor.GetVersion()),
								MakeShared<FJsonValueNumber>(ExpectedDescriptor.GetVersion()));
						FailPlan(TEXT("The plan descriptor hash does not match the route descriptor."),
							TEXT("descriptor_identity.descriptor_hash"), nullptr, nullptr);
						OutError.ActualEvidence = MakeShared<FJsonValueString>(ActualDescriptor.GetDescriptorHash());
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedDescriptor.GetDescriptorHash());
						return false;
					}
					return FailPlan(TEXT("The audio/media plan contains no changes."),
						TEXT("changes.count"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one change")));
				}
				for (const FPlannedChange& Change
					: Plan->GetChangeSet().GetChanges())
				{
					if (Change.GetIdentity().GetOwnerId()
						!= Request.GetTargetRef())
					{
						const int32 Index = static_cast<int32>(&Change - Plan->GetChangeSet().GetChanges().GetData());
						OutError = AudioMediaFailure(TEXT("graph_compound_audio_media_plan_target_mismatch"),
							TEXT("The planned audio/media change owner differs from the requested target."),
							Request.GetTargetRef(), TEXT("changes/") + ::LexToString(Index) + TEXT("/identity/owner_id"),
							MakeShared<FJsonValueString>(Change.GetIdentity().GetOwnerId()),
							MakeShared<FJsonValueString>(Request.GetTargetRef()));
						OutError.OperationIndex = Index;
						return false;
					}
				}
				FAudioMediaReadbackSpec Spec;
				if (!TryParseReadbackSpec(Profile, Request, Spec, OutError))
				{
					return false;
				}
				const TSharedRef<FAudioMediaLifecycleContext> Context =
					MakeShared<FAudioMediaLifecycleContext>(
						Profile,
						Request,
						Plan,
						CallerContext,
						MoveTemp(Resources),
						Cancellation,
						MoveTemp(Spec),
						JournalRoot);
				const bool bSettle = Profile.GetInputKind()
					== EAudioMediaGraphInputKind::MetaSoundCompileSettle
					|| Profile.GetInputKind()
					== EAudioMediaGraphInputKind::SoundCueCompileSettle
					|| Profile.GetInputKind()
					== EAudioMediaGraphInputKind::TextureGraphValidateEvaluateSettle;
				if (bSettle)
				{
					FGraphCompoundRuntimeError RuntimeError;
					if (!Context->TryCaptureCurrent(Context->CurrentSnapshot, RuntimeError))
					{
						OutError = RuntimeError;
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetTargetRef();
						if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
						return false;
					}
					Context->Spec.ExpectedRevision = Context->CurrentSnapshot
						->GetRevision().GetGeneration();
				}
				const FString ExpectedRevision =
					Profile.GetInputKind() == EAudioMediaGraphInputKind::AssetCreate
						? FString(TEXT("absent"))
						: (Context->Spec.ExpectedRevision.IsEmpty()
							? Plan->GetObservedRevision().GetGeneration()
							: Context->Spec.ExpectedRevision);
				if (ExpectedRevision.IsEmpty())
				{
					OutError = AudioMediaFailure(TEXT("graph_compound_audio_media_expected_revision_missing"),
						TEXT("The audio/media lifecycle has no expected asset revision."),
						Request.GetTargetRef(), TEXT("expected_revision.present"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				const TArray<FString> ImpactScope = {Request.GetTargetRef()};
				const TSharedRef<const FOperationLifecyclePlan> LifecyclePlan =
					MakeShared<FOperationLifecyclePlan>(
						TEXT("graph-compound-audio-media:") + Plan->GetRequestId(),
						Plan->GetFingerprint(),
						ImpactScope,
						TMap<FString, FString>{
							{Request.GetTargetRef(), ExpectedRevision}},
						TArray<FString>{Profile.GetCapabilityId()});
				const TSharedRef<const FOperationImpactAuthorization> Authorization =
					MakeShared<FOperationImpactAuthorization>(
						Plan->GetFingerprint(), ImpactScope);
				const TSharedRef<FOperationLifecycleService> Service =
					MakeShared<FOperationLifecycleService>(
						MakeShared<FAudioMediaVersionFence>(Context),
						MakeShared<FAudioMediaMutation>(Context),
						MakeShared<FAudioMediaStability>(Context),
						MakeShared<FAudioMediaPersistence>(Context),
						MakeShared<FAudioMediaVerification>(Context));
				return FGraphCompoundAudioMediaLifecycleInvocation::TryCreate(
					LifecyclePlan,
					Authorization,
					Service,
					StaticCastSharedRef<
						IGraphCompoundAudioMediaLifecycleContext>(Context),
					OutInvocation,
					OutError);
			}

			bool TryExecuteQuery(
				const FAudioMediaGraphRouteProfile& Profile,
				const FAudioMediaGraphArtifactQueryRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FUeAgentContractValue& OutResult,
				FGraphCompoundRuntimeError& OutError) override
			{
				(void)CallerContext;
				(void)Resources;
				OutResult = FUeAgentContractValue(TEXT("{}"));
				OutError = {};
				if (Profile.GetInputKind() != EAudioMediaGraphInputKind::TextureGraphArtifactCapture
					|| !IsInGameThread() || Cancellation->IsCancelled())
				{
					const bool bCancelled = Cancellation->IsCancelled();
					const bool bWrongKind = Profile.GetInputKind() != EAudioMediaGraphInputKind::TextureGraphArtifactCapture;
					OutError = AudioMediaFailure(bCancelled ? TEXT("graph_compound_audio_media_cancelled")
							: TEXT("graph_compound_audio_media_query_invalid"),
						bCancelled ? TEXT("The audio/media artifact query was cancelled.")
							: bWrongKind ? TEXT("The audio/media artifact query requires the TextureGraph artifact-capture input kind.")
								: TEXT("Audio/media artifact capture must run on the game thread."),
						Request.GetTargetRef(), bCancelled ? TEXT("cancellation.requested")
							: bWrongKind ? TEXT("profile.input_kind") : TEXT("execution.game_thread"),
						bCancelled ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true))
							: bWrongKind ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(static_cast<int32>(Profile.GetInputKind())))
								: TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
						bCancelled ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false))
							: bWrongKind ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(static_cast<int32>(EAudioMediaGraphInputKind::TextureGraphArtifactCapture)))
								: TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)));
					OutError.SubjectId = Profile.GetCapabilityId();
					return false;
				}
				FString Revision;
				if (!TryReadPackageAssetRevision(Request.GetTargetRef(), Revision, OutError)) return false;
				UTextureGraph* const Asset = LoadObject<UTextureGraph>(
					nullptr, *TargetObjectPath(Request.GetTargetRef()));
				if (Asset == nullptr || Asset->Graph() == nullptr)
				{
					OutError = AudioMediaFailure(TEXT("graph_compound_audio_media_target_invalid"),
						Asset ? TEXT("The TextureGraph artifact target has no native graph.")
							: TEXT("The artifact target could not be loaded as a TextureGraph asset."),
						Request.GetTargetRef(), Asset ? TEXT("graph") : TEXT("asset"), nullptr,
						MakeShared<FJsonValueString>(Asset ? TEXT("TextureGraph native graph") : TEXT("loadable TextureGraph asset")));
					return false;
				}
				TArray<FAudioMediaGraphTextureGraphArtifactSelector> Selectors;
				for (const FAudioMediaGraphArtifactOutputSelector& Output
					: Request.GetOutputs())
				{
					Selectors.Emplace(Output.GetElementId(), Output.GetOutputName());
				}
				TArray<FAudioMediaGraphTextureGraphCapturedOutput> Captured;
				FAudioMediaGraphError NativeError;
				if (!TextureGraphHook.TryCaptureArtifacts(
						*Asset, Selectors, Captured, NativeError))
				{
					OutError = MoveTemp(NativeError);
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetTargetRef();
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
					return false;
				}
				const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
				Root->SetBoolField(TEXT("published"), false);
				Root->SetStringField(TEXT("target_ref"), Request.GetTargetRef());
				TArray<TSharedPtr<FJsonValue>> Outputs;
				for (const FAudioMediaGraphTextureGraphCapturedOutput& Output : Captured)
				{
					const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("expression_object_path"),
						Output.GetExpressionObjectPath());
					Item->SetStringField(TEXT("output_name"), Output.GetOutputName());
					Item->SetNumberField(TEXT("node_index"), Output.GetNodeIndex());
					Item->SetNumberField(TEXT("pin_index"), Output.GetPinIndex());
					Item->SetStringField(TEXT("evaluated_value_type"),
						Output.GetEvaluatedValueType());
					Outputs.Add(MakeShared<FJsonValueObject>(Item));
				}
				Root->SetArrayField(TEXT("outputs"), MoveTemp(Outputs));
				FString Canonical;
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!Json::TryCanonicalize(Root, Canonical, JsonError))
				{
					SetError(OutError, JsonError, Request.GetTargetRef());
					MarkGraphCompoundNativeDiagnostic(OutError);
					return false;
				}
				OutResult = FUeAgentContractValue(MoveTemp(Canonical));
				return true;
			}

		private:
			FString JournalRoot;
			FAudioMediaGraphMetaSoundNativeHook MetaSoundHook;
			FAudioMediaGraphSoundCueNativeHook SoundCueHook;
			FAudioMediaGraphTextureGraphNativeHook TextureGraphHook;
			FPackageAssetDurableBackend DurableBackend;
			int64 LastFencingToken = 0;
		};
	}

	bool TryCreateAudioMediaGraphProductionEnvironment(
		const FGraphCompoundAudioMediaProductionConfig& Config,
		TSharedPtr<IGraphCompoundAudioMediaEnvironment>& OutEnvironment,
		FGraphCompoundRuntimeError& OutError)
	{
		OutEnvironment.Reset();
		OutError = {};
		OutEnvironment = MakeShared<FAudioMediaProductionEnvironment>(Config);
		return true;
	}

	bool TryValidateTextureGraphSemanticComposite(
		const FTextureGraphSemanticCompositeRequest& Request,
		const FPlanningPreflightPlanRef& Plan,
		const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
		FGraphCompoundRuntimeError& OutError)
	{
		OutError = {};
		auto FailRequest = [&](const FString& Reason, const FString& Field,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			OutError = AudioMediaFailure(TEXT("texture_graph_semantic_composite_invalid"),
				Reason, Request.TargetRef, Field, MoveTemp(Actual), MoveTemp(Expected));
			return false;
		};
		if (!Request.IsValid())
		{
			if (Request.TargetRef.TrimStartAndEnd().IsEmpty())
				return FailRequest(TEXT("The TextureGraph semantic composite target reference is empty."),
					TEXT("request.target_ref"), MakeShared<FJsonValueString>(Request.TargetRef),
					MakeShared<FJsonValueString>(TEXT("non-blank target reference")));
			return FailRequest(TEXT("The TextureGraph semantic composite has no node, parameter, link or setting changes."),
				TEXT("request.changes.count"), MakeShared<FJsonValueNumber>(0),
				MakeShared<FJsonValueString>(TEXT("at least one node, parameter, link or setting change")));
		}
		if (!IsInGameThread())
			return FailRequest(TEXT("TextureGraph semantic composite validation must run on the game thread."),
				TEXT("execution.game_thread"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		if (Cancellation->IsCancelled())
			return FailRequest(TEXT("The TextureGraph semantic composite was cancelled before validation."),
				TEXT("cancellation.requested"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
		if (Plan->GetChangeSet().GetChanges().IsEmpty())
			return FailRequest(TEXT("The TextureGraph semantic composite plan contains no changes."),
				TEXT("plan.changes.count"), MakeShared<FJsonValueNumber>(0),
				MakeShared<FJsonValueString>(TEXT("at least one planned change")));
		for (const FPlannedChange& Change : Plan->GetChangeSet().GetChanges())
		{
			if (Change.GetIdentity().GetOwnerId() != Request.TargetRef
				&& !(Change.GetIdentity().GetSemanticId() == TEXT("uai.semantic.texture_graph.setting")
					&& Change.GetIdentity().GetSelector(TEXT("asset_path")) == Request.TargetRef))
			{
				const int32 Index = static_cast<int32>(&Change - Plan->GetChangeSet().GetChanges().GetData());
				const TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
				Actual->SetStringField(TEXT("owner_id"), Change.GetIdentity().GetOwnerId());
				Actual->SetStringField(TEXT("asset_path"), Change.GetIdentity().GetSelector(TEXT("asset_path")));
				OutError = AudioMediaFailure(TEXT("texture_graph_semantic_composite_target_mismatch"),
					TEXT("The planned change belongs to a different target and is not a setting bound to the requested TextureGraph."),
					Request.TargetRef, TEXT("changes/") + ::LexToString(Index) + TEXT("/identity"),
					MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(Request.TargetRef));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Change.GetIdentity().GetSemanticId());
				OutError.OperationIndex = Index;
				return false;
			}
		}
		using namespace DomainAdapters::ContentGraphRead;
		FUeAgentContentGraphCapture Capture;
		FUeAgentContentGraphReadError CaptureError;
		UTextureGraph* Asset = LoadObject<UTextureGraph>(nullptr, *TargetObjectPath(Request.TargetRef));
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTrackerOwner();
		FUeAgentCanonicalSnapshotPtr Snapshot;
		if (!Asset)
		{
			OutError = AudioMediaFailure(TEXT("texture_graph_semantic_fence_capture_failed"),
				TEXT("The TextureGraph semantic composite target could not be loaded."),
				Request.TargetRef, TEXT("asset"), nullptr, MakeShared<FJsonValueString>(TEXT("loadable TextureGraph asset")));
			return false;
		}
		TMap<int32, TSet<FString>> Fields;
		for (const auto& Change : Plan->GetChangeSet().GetChanges()) {
			if (Change.GetIdentity().GetSemanticId() != TEXT("uai.semantic.texture_graph.setting")) continue;
			int32 NodeIndex = INDEX_NONE;
			if (LexTryParseString(NodeIndex, *Change.GetIdentity().GetSelector(TEXT("node_index"))) && NodeIndex >= 0)
				Fields.FindOrAdd(NodeIndex).Add(Change.GetIdentity().GetSelector(TEXT("property_path")));
		}
		const bool bTopology = Request.BeforeSnapshot.IsValid();
		if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureTextureGraphSettings(*Asset, Fields, Capture, CaptureError, bTopology)
			|| !TryBuildContentGraphAuthoringSnapshot(Capture, Resources, Snapshot, CaptureError))
		{
			OutError = CaptureError;
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.TargetRef;
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.TargetRef;
			return false;
		}
		// Observation generation describes captured content/scope, not the
		// native identity. Index the existing identity with only generation
		// normalized, then bind the freshly read facts to the original key.
		auto IsNativeBefore = [](const FUeAgentStateEntry& Entry) {
			const auto& Semantic = Entry.GetIdentity().GetSemanticId();
			// Compact editable inputs/aliases are already captured in their node.
			// Evaluated pin output is observation, not a write-before invariant.
			return Semantic != TEXT("uai.semantic.texture_graph.pin_value")
				&& Semantic != TEXT("uai.semantic.texture_graph.parameter");
		};
		auto BeforeValue = [&](const FUeAgentStateEntry& Entry, FUeAgentCanonicalValuePtr& OutValue) {
			OutValue = Entry.GetValue();
			const auto& Semantic = Entry.GetIdentity().GetSemanticId();
			if (Semantic != TEXT("uai.semantic.texture_graph.node") && Semantic != TEXT("uai.semantic.texture_graph.pin")) return true;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
			for (const auto& Field : Entry.GetValue()->GetObjectItems()) {
				if ((Semantic == TEXT("uai.semantic.texture_graph.node") && Field.Key == TEXT("name"))
					|| (Semantic == TEXT("uai.semantic.texture_graph.pin") && Field.Key == TEXT("value"))) continue;
				Fields.Add(Field);
			}
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryObject(Fields, *Resources, OutValue, Error)) { OutError = MoveTemp(Error); return false; }
			return true;
		};
		TMap<FString, const FUeAgentStateEntry*> BeforeByNativeKey;
		if (bTopology) for (const auto& Entry : Request.BeforeSnapshot->GetEntries())
			if (IsNativeBefore(Entry)) BeforeByNativeKey.Add(Entry.GetIdentity().WithGeneration(TEXT("authoring-binding/1")).GetCanonicalKey(), &Entry);
		TArray<FUeAgentStateEntry> Entries;
		Entries.Reserve(Snapshot->NumEntries());
		int32 ComparedEntries = 0;
		for (const auto& Captured : Snapshot->GetEntries()) {
			auto Entry = Captured;
			if (bTopology && IsNativeBefore(Entry)) {
				const auto* Before = BeforeByNativeKey.FindRef(Entry.GetIdentity().WithGeneration(TEXT("authoring-binding/1")).GetCanonicalKey());
				if (!Before) return FailRequest(TEXT("The observed TextureGraph topology or native binding changed before application."),
					TEXT("/before/native_identity"), MakeShared<FJsonValueString>(Entry.GetIdentity().GetCanonicalKey()),
					MakeShared<FJsonValueString>(TEXT("the captured native binding")));
				FUeAgentSemanticComparison Comparison;
				FUeAgentStateSemanticError StateError;
				FUeAgentCanonicalValuePtr ExpectedValue, CurrentValue;
				if (!BeforeValue(*Before, ExpectedValue) || !BeforeValue(Entry, CurrentValue)) return false;
				if (!TryCompareValues(ExpectedValue.ToSharedRef(), CurrentValue.ToSharedRef(), TEXT("/before/entries/") + FString::FromInt(ComparedEntries), *Resources, Comparison, StateError)) {
					OutError = MoveTemp(StateError); return false;
				}
				if (!Comparison.IsEquivalent()) {
					OutError = MakeComparisonDiagnostic(Comparison, TEXT("texture_graph_semantic_before_conflict"),
						TEXT("The captured TextureGraph topology, compact input or selected setting changed before application."), TEXT("preflight"), TEXT("uai.authoring.diagnostic/1"));
					OutError.TargetPath = Request.TargetRef; return false;
				}
				Entry = Entry.WithIdentityAndDefault(Before->GetIdentity(), Entry.GetDefault(), Entry.GetObservedRevision());
				++ComparedEntries;
			} else for (const auto& Condition : Plan->GetPreconditions()) {
				const auto Identity = Entry.GetIdentity().WithCapturedBinding(Condition.GetIdentity().GetCanonicalKey());
				if (Identity.IsSet()) { Entry = Entry.WithIdentityAndDefault(Identity.GetValue(), Entry.GetDefault(), Entry.GetObservedRevision()); break; }
			}
			Entries.Add(MoveTemp(Entry));
		}
		if (bTopology && ComparedEntries != BeforeByNativeKey.Num())
			return FailRequest(TEXT("A captured TextureGraph node, port, relation or selected setting disappeared before application."),
				TEXT("/before/observed_entries"), MakeShared<FJsonValueNumber>(ComparedEntries), MakeShared<FJsonValueNumber>(BeforeByNativeKey.Num()));
		FUeAgentCanonicalSnapshotPtr Bound;
		FUeAgentStateSemanticError StateError;
		if (!FUeAgentCanonicalSnapshot::TryCreate(Snapshot->GetSnapshotId(), Snapshot->GetStatePlane(), Snapshot->GetRevision(),
			Snapshot->GetCapturedAt(), Snapshot->GetProvenance(), MoveTemp(Entries), *Resources, Bound, StateError)) {
			OutError = MoveTemp(StateError); return false;
		}
		FPlanningPreflightError Error;
		const FUeAgentCallerContext Caller(Plan->GetCallerId(), Plan->GetAuthorityScopes(), Plan->GetRequestId());
		if (!TryRevalidateFieldPlan(Plan, Bound.ToSharedRef(), Caller, *Resources, Error)) { OutError = MoveTemp(Error); return false; }
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=texture_semantic_preconditions fields=%d topology_entries=%d complete_asset_capture=false revision_identity=false"),
			Plan->GetPreconditions().Num(), ComparedEntries);
		return true;
	}

	bool TryApplyTextureGraphSemanticCompositeNative(
		UObject& Asset, const FTextureGraphSemanticCompositeRequest& Request,
		FGraphCompoundRuntimeError& OutError,
		FAudioMediaGraphTextureGraphNodeBindings& Bindings)
	{
		UTextureGraph* Graph = Cast<UTextureGraph>(&Asset);
		if (!Graph)
		{
			OutError = AudioMediaFailure(TEXT("texture_graph_semantic_asset_invalid"),
				TEXT("The semantic composite native mutation target is not a TextureGraph asset."),
				Request.TargetRef, TEXT("asset.class"), MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName()),
				MakeShared<FJsonValueString>(UTextureGraph::StaticClass()->GetPathName()));
			return false;
		}
		FAudioMediaReadbackSpec Spec;
		Spec.Kind = EAudioMediaReadbackKind::TextureGraphSemanticComposite;
		Spec.TextureNodes = Request.Nodes;
		Spec.TextureParameters = Request.Parameters;
		Spec.TextureLinks = Request.Links;
		return TryApplyTextureGraph(*Graph, Spec, FAudioMediaGraphTextureGraphNativeHook(), OutError, &Bindings, Request.TargetRef);
	}

	bool TryVerifyTextureGraphSemanticCompositeNative(
		UObject& Asset, const FTextureGraphSemanticCompositeRequest& Request,
		UE::UAI::Contract::FDiagnostic& OutError,
		const FAudioMediaGraphTextureGraphNodeBindings& Bindings)
	{
		UTextureGraph* Graph = Cast<UTextureGraph>(&Asset);
		if (!Graph)
		{
			OutError = AudioMediaFailure(TEXT("texture_graph_semantic_asset_invalid"),
				TEXT("The semantic composite readback target is not a TextureGraph asset."),
				Request.TargetRef, TEXT("asset.class"), MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName()),
				MakeShared<FJsonValueString>(UTextureGraph::StaticClass()->GetPathName()));
			OutError.Stage = TEXT("native_readback");
			return false;
		}
		FAudioMediaReadbackSpec Spec;
		Spec.Kind = EAudioMediaReadbackKind::TextureGraphSemanticComposite;
		Spec.TextureNodes = Request.Nodes;
		Spec.TextureParameters = Request.Parameters;
		Spec.TextureLinks = Request.Links;
		return VerifyTextureGraphReadback(*Graph, Spec, OutError, &Bindings);
	}
}
