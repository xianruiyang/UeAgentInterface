#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "AIGraphNode.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "ControlRigBlueprint.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphStructureCapture.h"
#include "DomainAdapters/BlueprintUmgGraph/UeAgentBlueprintUmgGraphNativeHook.h"
#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphNativeHook.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraStageModuleAuthoring.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQueryGraph.h"
#include "EnvironmentQueryGraphNode_Option.h"
#include "Expressions/TG_Expression.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringState.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundSource.h"
#include "NiagaraEmitter.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "PCGGraph.h"
#include "Retargeter/IKRetargeter.h"
#include "ReferenceSkeleton.h"
#include "Rig/IKRigDefinition.h"
#include "Rigs/RigHierarchy.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "SmartObjectDefinition.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeState.h"
#include "TG_Graph.h"
#include "TG_Node.h"
#include "TG_Pin.h"
#include "TextureGraph.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "WidgetBlueprint.h"
#include "WorldPartition/HLOD/HLODLayer.h"

namespace UE::UAI::DomainAdapters::ContentGraphRead
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::DomainAdapters::AnimationRigGraph;
	using namespace UE::UAI::DomainAdapters::BlueprintUmgGraph;
	using namespace UE::UAI::DomainAdapters::MaterialNiagaraGraph;
	using namespace UE::UAI::StateSemantic;
	using namespace UeAgentNiagaraStageModuleAuthoring;

	namespace
	{
		bool IsUnsignedNumericProperty(
			const FNumericProperty* Property)
		{
			return CastField<FByteProperty>(Property)
				|| CastField<FUInt16Property>(Property)
				|| CastField<FUInt32Property>(Property)
				|| CastField<FUInt64Property>(Property);
		}

		bool Fail(
			FUeAgentContentGraphReadError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		FString DomainId(const UObject& Owner)
		{
			if (Owner.IsA<UControlRigBlueprint>()
				|| Owner.IsA<UIKRigDefinition>()
				|| Owner.IsA<UIKRetargeter>()
				|| Owner.IsA<USkeleton>())
			{
				return TEXT("animation_rig_graph");
			}
			if (Owner.IsA<UAnimBlueprint>()
				|| Owner.IsA<UAnimationAsset>())
			{
				return TEXT("animation");
			}
			if (Owner.IsA<UBlueprint>())
			{
				return TEXT("blueprint");
			}
			if (Owner.IsA<UMaterialInterface>()
				|| Owner.IsA<UMaterialFunctionInterface>())
			{
				return TEXT("material");
			}
			if (Owner.IsA<UNiagaraSystem>()
				|| Owner.IsA<UNiagaraEmitter>()
				|| Owner.IsA<UNiagaraScript>())
			{
				return TEXT("niagara");
			}
			if (Owner.IsA<UPCGGraph>())
			{
				return TEXT("pcg");
			}
			if (Owner.IsA<USoundCue>()
				|| Owner.IsA<UMetaSoundSource>())
			{
				return TEXT("audio");
			}
			if (Owner.IsA<UTextureGraph>())
			{
				return TEXT("texture_graph");
			}
			if (Owner.IsA<UBehaviorTree>()
				|| Owner.IsA<UBlackboardData>()
				|| Owner.IsA<UEnvQuery>()
				|| Owner.IsA<USmartObjectDefinition>()
				|| Owner.IsA<UStateTree>())
			{
				return TEXT("ai_behavior");
			}
			if (Owner.IsA<UHLODLayer>())
			{
				return TEXT("world_hlod");
			}
			return FString();
		}

		UObject* ResolveObject(const FString& PublicRef)
		{
			FString ObjectPath = PublicRef;
			ObjectPath.TrimStartAndEndInline();
			if (ObjectPath.StartsWith(TEXT("uai://")))
			{
				const int32 Query = ObjectPath.Find(TEXT("?object_path="));
				if (Query != INDEX_NONE)
				{
					ObjectPath = FGenericPlatformHttp::
						UrlDecode(ObjectPath.Mid(Query + 13));
				}
			}
			if (ObjectPath.IsEmpty())
			{
				return nullptr;
			}
			if (UObject* Existing = StaticFindObject(
				UObject::StaticClass(),
				nullptr,
				*ObjectPath))
			{
				return Existing;
			}
			return StaticLoadObject(
				UObject::StaticClass(),
				nullptr,
				*ObjectPath);
		}

		FUeAgentResourceTracker MakeCanonicalResources()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 1ll << 34},
				{EUeAgentResourceDimension::OutputBytes, 1ll << 34},
				{EUeAgentResourceDimension::StringBytes, 1ll << 34},
				{EUeAgentResourceDimension::Nodes, 1ll << 30},
				{EUeAgentResourceDimension::Items, 1ll << 30},
				{EUeAgentResourceDimension::ReferenceEdges, 1ll << 30},
				{EUeAgentResourceDimension::WorkUnits, 1ll << 32},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, 1ll << 36},
			}));
		}

		bool TryCanonicalValue(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			if (!JsonValue.IsValid()
				|| JsonValue->Type == EJson::Null)
			{
				return FUeAgentCanonicalValue::TryNull(
					Resources,
					OutValue,
					OutError);
			}
			switch (JsonValue->Type)
			{
			case EJson::Boolean:
				return FUeAgentCanonicalValue::TryBoolean(
					JsonValue->AsBool(),
					Resources,
					OutValue,
					OutError);
			case EJson::String:
				return FUeAgentCanonicalValue::TryString(
					JsonValue->AsString(),
					Resources,
					OutValue,
					OutError);
			case EJson::Number:
				return FUeAgentCanonicalValue::TryDecimal(
					FString::Printf(
						TEXT("%.17g"),
						JsonValue->AsNumber()),
					Resources,
					OutValue,
					OutError);
			case EJson::Array:
				{
					TArray<FUeAgentCanonicalValueRef> Items;
					for (const TSharedPtr<FJsonValue>& Item
						: JsonValue->AsArray())
					{
						FUeAgentCanonicalValuePtr Canonical;
						if (!TryCanonicalValue(
							Item,
							Resources,
							Canonical,
							OutError))
						{
							return false;
						}
						Items.Add(Canonical.ToSharedRef());
					}
					return FUeAgentCanonicalValue::TryArray(
						Items,
						Resources,
						OutValue,
						OutError);
				}
			case EJson::Object:
				{
					const TSharedPtr<FJsonObject> Object =
						JsonValue->AsObject();
					if (!Object.IsValid())
					{
						return false;
					}
					TArray<FString> Keys;
					Object->Values.GetKeys(Keys);
					Keys.Sort();
					TArray<TPair<
						FString,
						FUeAgentCanonicalValueRef>> Items;
					for (const FString& Key : Keys)
					{
						FUeAgentCanonicalValuePtr Canonical;
						if (!TryCanonicalValue(
							Object->Values.FindChecked(Key),
							Resources,
							Canonical,
							OutError))
						{
							return false;
						}
						Items.Emplace(
							Key,
							Canonical.ToSharedRef());
					}
					return FUeAgentCanonicalValue::TryObject(
						Items,
						Resources,
						OutValue,
						OutError);
				}
			default:
				return false;
			}
		}

		bool TryPropertyValue(
			FProperty& Property,
			const void* ValuePtr,
			UObject& Owner,
			const UeAgentPropertyValueCodec::FOptions& Options,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			if (const FBoolProperty* Boolean =
				CastField<FBoolProperty>(&Property))
			{
				return FUeAgentCanonicalValue::TryBoolean(
					Boolean->GetPropertyValue(ValuePtr),
					Resources,
					OutValue,
					OutError);
			}
			if (const FNumericProperty* Numeric =
				CastField<FNumericProperty>(&Property))
			{
				if (Numeric->IsInteger())
				{
					const FString Lexeme =
						IsUnsignedNumericProperty(Numeric)
							? ::LexToString(
								Numeric->
									GetUnsignedIntPropertyValue(
										ValuePtr))
							: ::LexToString(
								Numeric->
									GetSignedIntPropertyValue(
										ValuePtr));
					return FUeAgentCanonicalValue::TryInteger(
						Lexeme,
						Resources,
						OutValue,
						OutError);
				}
				return FUeAgentCanonicalValue::TryDecimal(
					FString::Printf(
						TEXT("%.17g"),
						Numeric->GetFloatingPointPropertyValue(
							ValuePtr)),
					Resources,
					OutValue,
					OutError);
			}
			return TryCanonicalValue(
				UeAgentPropertyValueCodec::Encode(
					&Property,
					ValuePtr,
					&Owner,
					Options),
				Resources,
				OutValue,
				OutError);
		}

		FString ContainerKind(const EUeAgentValueKind Kind)
		{
			if (Kind == EUeAgentValueKind::Array)
			{
				return TEXT("array");
			}
			if (Kind == EUeAgentValueKind::Object)
			{
				return TEXT("object");
			}
			return TEXT("scalar");
		}

		bool AddProperties(
			UObject& Object,
			FUeAgentResourceTracker& Resources,
			TArray<FUeAgentContentGraphProperty>& OutProperties,
			int32& OutPropertyCount,
			FUeAgentContentGraphReadError& OutError,
			TArray<FString>* OutRevisionFacts = nullptr,
			const TSet<FName>* Excluded = nullptr)
		{
			UeAgentPropertyValueCodec::FOptions Options;
			Options.MaxDepth = -1;
			Options.MaxContainerItems =
				TNumericLimits<int32>::Max();
			Options.MaxStringChars =
				TNumericLimits<int32>::Max();
			Options.bAllowPartialStructFields = false;

			TArray<FProperty*> Properties;
			for (TFieldIterator<FProperty> It(
				Object.GetClass(),
				EFieldIterationFlags::IncludeSuper);
				It;
				++It)
			{
				FProperty* Property = *It;
				if (!Property
					|| (Excluded && Excluded->Contains(Property->GetFName()))
					|| !Property->HasAnyPropertyFlags(
						CPF_Edit | CPF_BlueprintVisible)
					|| Property->HasAnyPropertyFlags(
						CPF_Transient | CPF_Deprecated))
				{
					continue;
				}
				FString Unsupported;
				if (!UeAgentPropertyValueCodec::IsSupported(
					Property,
					Unsupported))
				{
					continue;
				}
				Properties.Add(Property);
			}
			Properties.Sort(
				[](const FProperty& Left, const FProperty& Right)
				{
					return Left.GetName() < Right.GetName();
				});

			UObject* DefaultObject =
				Object.GetClass()->GetDefaultObject();
			for (FProperty* Property : Properties)
			{
				const void* ValuePtr =
					Property->ContainerPtrToValuePtr<void>(&Object);
				const void* DefaultPtr = DefaultObject
					? Property->ContainerPtrToValuePtr<void>(
						DefaultObject)
					: ValuePtr;
				FUeAgentCanonicalValuePtr Value;
				FUeAgentCanonicalValuePtr Default;
				FUeAgentStateSemanticError StateError;
				if (!TryPropertyValue(
						*Property,
						ValuePtr,
						Object,
						Options,
						Resources,
						Value,
						StateError)
					|| !TryPropertyValue(
						*Property,
						DefaultPtr,
						Object,
						Options,
						Resources,
						Default,
						StateError))
				{
					continue;
				}
				const bool bOverridden =
					!UeAgentPropertyValueCodec::
						AreSemanticallyEquivalent(
							Property,
							ValuePtr,
							DefaultPtr,
							&Object,
							Options);
				FUeAgentContentGraphFieldState State;
				State.Type = FUeAgentCanonicalType(
					TEXT("reflection.")
						+ Property->GetClass()->GetName()
						+ TEXT(".")
						+ Property->GetName(),
					Value->GetKind(),
					false,
					ContainerKind(Value->GetKind()));
				State.Value = Value;
				State.Default = FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::ClassDefaultObject,
					DefaultObject
						? DefaultObject->GetPathName()
						: Object.GetClass()->GetPathName(),
					TEXT("reflection_class_default"),
					Default.ToSharedRef());
				State.AuthoredPresence = bOverridden
					? EUeAgentAuthoredPresence::Present
					: EUeAgentAuthoredPresence::Absent;
				State.OverrideState = bOverridden
					? EUeAgentOverrideState::Overridden
					: EUeAgentOverrideState::Inherited;
				OutProperties.Add({
					Object.GetPathName(),
					Property->GetName(),
					MoveTemp(State),
				});
				if (OutRevisionFacts)
				{
					TSharedPtr<FJsonValue> JsonValue;
					FString JsonError;
					if (!CanonicalJson::TryToJsonValue(*Value, JsonValue, JsonError)
						|| !JsonValue.IsValid())
					{
						return Fail(OutError,
							TEXT("content_graph_property_revision_invalid"),
							Property->GetName(), JsonError);
					}
					const TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
					Envelope->SetField(TEXT("value"), JsonValue);
					FString Canonical;
					if (!CommandContract::Json::TryCanonicalize(
							Envelope, Canonical, JsonError))
					{
						return Fail(OutError,
							TEXT("content_graph_property_revision_invalid"),
							Property->GetName(), JsonError);
					}
					OutRevisionFacts->Add(Object.GetPathName()
						+ TEXT("|") + Property->GetName() + TEXT("|") + Canonical);
				}
				++OutPropertyCount;
			}
			return true;
		}

		FUeAgentContentGraphSemanticIdentity SemanticIdentity(
			FString SemanticId,
			TMap<FString, FString> Selectors,
			FString ObjectRole)
		{
			FUeAgentContentGraphSemanticIdentity Identity;
			Identity.SemanticId = MoveTemp(SemanticId);
			Identity.Selectors = MoveTemp(Selectors);
			Identity.ObjectRole = MoveTemp(ObjectRole);
			return Identity;
		}

		FString GuidText(const FGuid& Guid)
		{
			return Guid.IsValid()
				? Guid.ToString(EGuidFormats::DigitsWithHyphensLower)
				: FString();
		}

		bool TryCanonicalObject(
			const TSharedRef<FJsonObject>& Object,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentContentGraphReadError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			if (!TryCanonicalValue(
				MakeShared<FJsonValueObject>(Object),
				Resources,
				OutValue,
				StateError))
			{
				return Fail(
					OutError,
					StateError.Code,
					StateError.Path,
					StateError.Detail);
			}
			return true;
		}

		bool TryMakeManualFieldState(
			const TSharedPtr<FJsonValue>& ValueJson,
			const TSharedPtr<FJsonValue>& DefaultJson,
			const FString& TypeId,
			bool bOverridden,
			FUeAgentResourceTracker& Resources,
			FUeAgentContentGraphFieldState& OutState,
			FUeAgentContentGraphReadError& OutError)
		{
			FUeAgentCanonicalValuePtr Value;
			FUeAgentCanonicalValuePtr Default;
			FUeAgentStateSemanticError StateError;
			if (!TryCanonicalValue(
					ValueJson,
					Resources,
					Value,
					StateError)
				|| !TryCanonicalValue(
					DefaultJson,
					Resources,
					Default,
					StateError))
			{
				return Fail(
					OutError,
					StateError.Code,
					StateError.Path,
					StateError.Detail);
			}
			OutState = {};
			OutState.Type = FUeAgentCanonicalType(
				TypeId,
				Value->GetKind(),
				false,
				ContainerKind(Value->GetKind()));
			OutState.Value = Value;
			OutState.Default = FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::DomainNative,
				TEXT("ue-native"),
				TEXT("content-graph-read"),
				Default.ToSharedRef());
			OutState.AuthoredPresence = bOverridden
				? EUeAgentAuthoredPresence::Present
				: EUeAgentAuthoredPresence::Absent;
			OutState.OverrideState = bOverridden
				? EUeAgentOverrideState::Overridden
				: EUeAgentOverrideState::Inherited;
			return true;
		}

		bool AddMappedProperty(
			UObject& Object,
			const FName NativePropertyName,
			const FString& ExternalFieldId,
			const FString& OwnerId,
			const FString& SemanticId,
			TMap<FString, FString> Selectors,
			FUeAgentResourceTracker& Resources,
			TArray<FUeAgentContentGraphProperty>& OutProperties,
			TArray<FString>& OutRevisionFacts,
			FUeAgentContentGraphReadError& OutError)
		{
			FProperty* Property = FindFProperty<FProperty>(
				Object.GetClass(),
				NativePropertyName);
			if (!Property)
			{
				return Fail(
					OutError,
					TEXT("content_graph_read_property_missing"),
					Object.GetPathName(),
					NativePropertyName.ToString());
			}
			FString Unsupported;
			if (!UeAgentPropertyValueCodec::IsSupported(
				Property,
				Unsupported))
			{
				return Fail(
					OutError,
					TEXT("content_graph_read_property_unsupported"),
					Object.GetPathName(),
					Unsupported);
			}

			UeAgentPropertyValueCodec::FOptions Options;
			Options.MaxDepth = -1;
			Options.MaxContainerItems = TNumericLimits<int32>::Max();
			Options.MaxStringChars = TNumericLimits<int32>::Max();
			Options.bAllowPartialStructFields = false;
			UObject* DefaultObject = Object.GetClass()->GetDefaultObject();
			const void* ValuePtr =
				Property->ContainerPtrToValuePtr<void>(&Object);
			const void* DefaultPtr = DefaultObject
				? Property->ContainerPtrToValuePtr<void>(DefaultObject)
				: ValuePtr;
			FUeAgentCanonicalValuePtr Value;
			FUeAgentCanonicalValuePtr Default;
			FUeAgentStateSemanticError StateError;
			if (!TryPropertyValue(
					*Property,
					ValuePtr,
					Object,
					Options,
					Resources,
					Value,
					StateError)
				|| !TryPropertyValue(
					*Property,
					DefaultPtr,
					Object,
					Options,
					Resources,
					Default,
					StateError))
			{
				return Fail(
					OutError,
					StateError.Code,
					Object.GetPathName(),
					NativePropertyName.ToString());
			}
			const bool bOverridden =
				!UeAgentPropertyValueCodec::AreSemanticallyEquivalent(
					Property,
					ValuePtr,
					DefaultPtr,
					&Object,
					Options);
			if (!bOverridden)
			{
				return true;
			}
			FUeAgentContentGraphFieldState State;
			State.Type = FUeAgentCanonicalType(
				TEXT("reflection.")
					+ Property->GetClass()->GetName()
					+ TEXT(".")
					+ Property->GetName(),
				Value->GetKind(),
				false,
				ContainerKind(Value->GetKind()));
			State.Value = Value;
			State.Default = FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::ClassDefaultObject,
				DefaultObject
					? DefaultObject->GetPathName()
					: Object.GetClass()->GetPathName(),
				TEXT("reflection_class_default"),
				Default.ToSharedRef());
			State.AuthoredPresence = bOverridden
				? EUeAgentAuthoredPresence::Present
				: EUeAgentAuthoredPresence::Absent;
			State.OverrideState = bOverridden
				? EUeAgentOverrideState::Overridden
				: EUeAgentOverrideState::Inherited;
			Selectors.Add(TEXT("property_path"), ExternalFieldId);
			Selectors.Add(
				TEXT("native_property_path"),
				NativePropertyName.ToString());
			FUeAgentContentGraphProperty Captured;
			Captured.OwnerId = OwnerId;
			Captured.FieldId = ExternalFieldId;
			Captured.State = MoveTemp(State);
			Captured.SemanticIdentity = SemanticIdentity(
				SemanticId,
				MoveTemp(Selectors),
				TEXT("property"));
			OutProperties.Add(MoveTemp(Captured));
			OutRevisionFacts.Add(
				OwnerId + TEXT("|") + ExternalFieldId + TEXT("|")
					+ Value->GetText());
			return true;
		}

		void MixRevisionFact(uint64& Hash, const FString& Fact)
		{
			const FTCHARToUTF8 Utf8(*Fact);
			for (int32 Index = 0; Index < Utf8.Length(); ++Index)
			{
				Hash ^= static_cast<uint8>(Utf8.Get()[Index]);
				Hash *= 1099511628211ull;
			}
			Hash ^= 0xff;
			Hash *= 1099511628211ull;
		}

		FString CaptureRevision(
			const UObject& Owner,
			TArray<FString> Facts)
		{
			Facts.Sort();
			uint64 Hash = 1469598103934665603ull;
			MixRevisionFact(Hash, Owner.GetClass()->GetPathName());
			MixRevisionFact(Hash, Owner.GetPathName());
			for (const FString& Fact : Facts)
			{
				MixRevisionFact(Hash, Fact);
			}
			return FString::Printf(
				TEXT("content-graph:%016llx"),
				Hash);
		}

		FString RigElementKind(const ERigElementType Type)
		{
			switch (Type)
			{
			case ERigElementType::Bone: return TEXT("bone");
			case ERigElementType::Null: return TEXT("null");
			case ERigElementType::Control: return TEXT("control");
			case ERigElementType::Curve: return TEXT("curve");
			case ERigElementType::Reference: return TEXT("reference");
			case ERigElementType::Connector: return TEXT("connector");
			case ERigElementType::Socket: return TEXT("socket");
			default: return TEXT("element");
			}
		}

		bool AugmentAnimationRigCapture(
			UObject& Owner,
			const FString& RootContainerId,
			FUeAgentResourceTracker& Resources,
			FUeAgentContentGraphCapture& OutCapture,
			FString& OutNativeRevision,
			FUeAgentContentGraphReadError& OutError)
		{
			OutNativeRevision.Reset();
			TArray<FString> RevisionFacts;
			if (UControlRigBlueprint* Blueprint = Cast<UControlRigBlueprint>(&Owner))
			{
				URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
				if (!Hierarchy)
				{
					return Fail(
						OutError,
						TEXT("content_graph_read_animation_rig_hierarchy_missing"),
						Owner.GetPathName());
				}
				const TArray<FRigElementKey> Keys = Hierarchy->GetAllKeys(
					false, ERigElementType::All);
				for (const FRigElementKey& Key : Keys)
				{
					const FString Kind = RigElementKind(Key.Type);
					const FString ItemId = TEXT("rig_element:") + Kind
						+ TEXT(":") + Key.Name.ToString();
					const FString ClassPath = FString(TEXT("/Script/ControlRig.Rig"))
						+ FString::Chr(FChar::ToUpper(Kind[0]))
						+ Kind.Mid(1) + TEXT("Element");
					OutCapture.Items.Add({
						ItemId,
						Kind,
						Owner.GetPathName(),
						ClassPath,
						RootContainerId,
						OutCapture.Items.Num(),
					});
					const FRigElementKey Parent = Hierarchy->GetFirstParent(Key);
					const FString ParentId = Parent.IsValid()
						? TEXT("rig_element:") + RigElementKind(Parent.Type)
							+ TEXT(":") + Parent.Name.ToString()
						: RootContainerId;
					OutCapture.Relations.Add({
						TEXT("animation_rig_graph.relation:") + ItemId,
						TEXT("hierarchy_child"),
						ParentId,
						ItemId,
					});
					RevisionFacts.Add(ItemId + TEXT("|") + ParentId);
				}
			}
			else if (USkeleton* Skeleton = Cast<USkeleton>(&Owner))
			{
				const FReferenceSkeleton& Reference =
					Skeleton->GetReferenceSkeleton();
				for (int32 Index = 0; Index < Reference.GetNum(); ++Index)
				{
					const FString Name = Reference.GetBoneName(Index).ToString();
					const FString ItemId = TEXT("rig_element:bone:") + Name;
					OutCapture.Items.Add({
						ItemId,
						TEXT("bone"),
						Owner.GetPathName(),
						TEXT("/Script/Engine.BoneReference"),
						RootContainerId,
						OutCapture.Items.Num(),
					});
					const int32 ParentIndex = Reference.GetParentIndex(Index);
					const FString ParentId = ParentIndex != INDEX_NONE
						? TEXT("rig_element:bone:")
							+ Reference.GetBoneName(ParentIndex).ToString()
						: RootContainerId;
					OutCapture.Relations.Add({
						TEXT("animation_rig_graph.relation:") + ItemId,
						TEXT("hierarchy_child"),
						ParentId,
						ItemId,
					});
					RevisionFacts.Add(ItemId + TEXT("|") + ParentId);
				}
				OutNativeRevision =
					CaptureRevision(Owner, MoveTemp(RevisionFacts));
				return true;
			}

			TArray<FAnimationRigGraphStructureValue> Structure;
			FAnimationRigGraphAdapterError StructureError;
			if (!TryCaptureAnimationRigGraphStructure(
					Owner, Structure, StructureError))
			{
				return Fail(
					OutError,
					StructureError.GetCode(),
					Owner.GetPathName(),
					StructureError.GetDetail());
			}
			for (const FAnimationRigGraphStructureValue& Captured : Structure)
			{
				FUeAgentCanonicalValuePtr State;
				if (!TryCanonicalObject(
						Captured.Value, Resources, State, OutError))
				{
					return false;
				}
				FString Canonical;
				FString JsonError;
				if (!CommandContract::Json::TryCanonicalize(
						Captured.Value, Canonical, JsonError))
				{
					return Fail(
						OutError,
						TEXT("content_graph_read_animation_rig_state_invalid"),
						Captured.SemanticId,
						JsonError);
				}
				const FUeAgentContentGraphSemanticIdentity Identity =
					SemanticIdentity(
						TEXT("uai.semantic.animation_rig_graph.structure_item"),
						{{TEXT("asset_path"), Owner.GetPathName()},
						 {TEXT("item_id"), Captured.SemanticId}},
						TEXT("item"));
				FUeAgentContentGraphItem* Existing =
					OutCapture.Items.FindByPredicate(
						[&Captured](const FUeAgentContentGraphItem& Item)
						{
							return Item.ItemId == Captured.SemanticId;
						});
				if (Existing)
				{
					Existing->ItemKind = Captured.ItemKind;
					Existing->ClassPath = Captured.ClassPath;
					Existing->SemanticIdentity = Identity;
					Existing->State = State;
				}
				else
				{
					FUeAgentContentGraphItem Item{
						Captured.SemanticId,
						Captured.ItemKind,
						Owner.GetPathName(),
						Captured.ClassPath,
						RootContainerId,
						OutCapture.Items.Num(),
						Identity,
					};
					Item.State = State;
					OutCapture.Items.Add(MoveTemp(Item));
					OutCapture.Relations.Add({
						TEXT("animation_rig_graph.membership:")
							+ Captured.SemanticId,
						TEXT("membership"),
						RootContainerId,
						Captured.SemanticId,
					});
				}
				RevisionFacts.Add(
					Captured.SemanticId + TEXT("|") + Canonical);
			}
			OutNativeRevision = CaptureRevision(Owner, MoveTemp(RevisionFacts));
			return true;
		}

		FString MaterialParameterType(
			const EMaterialNiagaraGraphParameterType Type)
		{
			switch (Type)
			{
			case EMaterialNiagaraGraphParameterType::Scalar:
				return TEXT("scalar");
			case EMaterialNiagaraGraphParameterType::Vector:
				return TEXT("vector");
			case EMaterialNiagaraGraphParameterType::Texture:
				return TEXT("texture");
			case EMaterialNiagaraGraphParameterType::StaticSwitch:
				return TEXT("static_switch");
			case EMaterialNiagaraGraphParameterType::StaticComponentMask:
				return TEXT("static_component_mask");
			default:
				return TEXT("unknown");
			}
		}

		TSharedPtr<FJsonValue> MaterialParameterValue(
			const FMaterialNiagaraGraphEffectiveParameter& Parameter)
		{
			switch (Parameter.GetIdentity().GetParameterType())
			{
			case EMaterialNiagaraGraphParameterType::Scalar:
				return MakeShared<FJsonValueNumber>(Parameter.GetScalarValue());
			case EMaterialNiagaraGraphParameterType::Vector:
			{
				const FLinearColor& Color = Parameter.GetVectorValue();
				return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>({
					MakeShared<FJsonValueNumber>(Color.R),
					MakeShared<FJsonValueNumber>(Color.G),
					MakeShared<FJsonValueNumber>(Color.B),
					MakeShared<FJsonValueNumber>(Color.A),
				}));
			}
			case EMaterialNiagaraGraphParameterType::Texture:
				return MakeShared<FJsonValueString>(Parameter.GetTextureRef());
			case EMaterialNiagaraGraphParameterType::StaticSwitch:
				return MakeShared<FJsonValueBoolean>(Parameter.GetBooleanValue());
			default:
				return MakeShared<FJsonValueNull>();
			}
		}

		bool AugmentMaterialCapture(
			UObject& Owner,
			const FString& RootContainerId,
			FUeAgentResourceTracker& Resources,
			FUeAgentContentGraphCapture& OutCapture,
			FString& OutNativeRevision,
			FUeAgentContentGraphReadError& OutError)
		{
			OutNativeRevision.Reset();
			FMaterialNiagaraGraphNativeHook Hook;
			FMaterialNiagaraGraphAdapterError NativeError;
			FString Evidence;
			if (UMaterialInstanceConstant* Instance =
				Cast<UMaterialInstanceConstant>(&Owner))
			{
				TArray<FMaterialNiagaraGraphEffectiveParameter> Parameters;
				if (!Hook.TryCaptureMaterialInstanceEffectiveParameters(
						*Instance,
						Parameters,
						OutNativeRevision,
						Evidence,
						NativeError))
				{
					return Fail(
						OutError,
						NativeError.GetCode(),
						Owner.GetPathName(),
						NativeError.GetDetail());
				}
				for (const FMaterialNiagaraGraphEffectiveParameter& Parameter
					: Parameters)
				{
					const FMaterialNiagaraGraphParameterIdentity& Identity =
						Parameter.GetIdentity();
					const FString Type = MaterialParameterType(
						Identity.GetParameterType());
					FUeAgentContentGraphFieldState State;
					if (!TryMakeManualFieldState(
							MaterialParameterValue(Parameter),
							MakeShared<FJsonValueNull>(),
							TEXT("material.parameter.") + Type,
							Parameter.IsOverridden(),
							Resources,
							State,
							OutError))
					{
						return false;
					}
					TMap<FString, FString> Selectors = {
						{TEXT("name"), Identity.GetParameterName()},
						{TEXT("association"), Identity.GetParameterAssociation()},
						{TEXT("index"), FString::FromInt(Identity.GetParameterIndex())},
						{TEXT("type"), Type},
					};
					FUeAgentContentGraphParameter Captured;
					Captured.ParameterId = Identity.GetParameterKey();
					Captured.OwnerId = Owner.GetPathName();
					Captured.State = MoveTemp(State);
					Captured.SemanticIdentity = SemanticIdentity(
						TEXT("uai.semantic.material.parameter"),
						MoveTemp(Selectors),
						TEXT("parameter"));
					OutCapture.Parameters.Add(MoveTemp(Captured));
				}
				return true;
			}

			FMaterialNiagaraGraphTopology Topology;
			if (!Hook.TryCaptureMaterialGraph(
					Owner,
					Owner.GetPathName(),
					Topology,
					OutNativeRevision,
					Evidence,
					NativeError))
			{
				return Fail(
					OutError,
					NativeError.GetCode(),
					Owner.GetPathName(),
					NativeError.GetDetail());
			}

			TMap<FString, UMaterialExpression*> ExpressionsByGuid;
			TArray<UObject*> InnerObjects;
			GetObjectsWithOuter(&Owner, InnerObjects, true);
			for (UObject* Object : InnerObjects)
			{
				if (UMaterialExpression* Expression =
					Cast<UMaterialExpression>(Object))
				{
					ExpressionsByGuid.Add(
						GuidText(Expression->MaterialExpressionGuid),
						Expression);
				}
			}

			for (int32 Index = 0; Index < Topology.GetItems().Num(); ++Index)
			{
				const FMaterialNiagaraGraphItemIdentity& Item =
					Topology.GetItems()[Index];
				UMaterialExpression* Expression =
					ExpressionsByGuid.FindRef(Item.GetNativeGuid());
				FUeAgentContentGraphItem Captured;
				Captured.ItemId = Item.GetItemKey();
				Captured.ItemKind = TEXT("material_expression");
				Captured.OwnerId = Owner.GetPathName();
				Captured.ClassPath = Expression
					? Expression->GetClass()->GetPathName()
					: Item.GetStableBuiltinRole();
				Captured.ContainerId = RootContainerId;
				Captured.Order = Index;
				Captured.SemanticIdentity = SemanticIdentity(
					TEXT("uai.semantic.material.expression"),
					{{TEXT("native_guid"), Item.GetNativeGuid()}},
					TEXT("item"));
				FUeAgentContentGraphItem* Existing = Expression
					? OutCapture.Items.FindByPredicate(
						[Expression](const FUeAgentContentGraphItem& Candidate)
						{
							return Candidate.ItemId == Expression->GetPathName();
						})
					: nullptr;
				if (Existing)
				{
					const FString PreviousId = Existing->ItemId;
					*Existing = MoveTemp(Captured);
					for (FUeAgentContentGraphRelation& Relation
						: OutCapture.Relations)
					{
						if (Relation.SourceItemId == PreviousId)
						{
							Relation.SourceItemId = Existing->ItemId;
						}
						if (Relation.TargetItemId == PreviousId)
						{
							Relation.TargetItemId = Existing->ItemId;
						}
					}
				}
				else
				{
					OutCapture.Items.Add(MoveTemp(Captured));
				}
			}

			int32 PinOrder = Topology.GetItems().Num();
			for (const FMaterialNiagaraGraphPinIdentity& Pin : Topology.GetPins())
			{
				const FString Direction =
					Pin.GetDirection() == EMaterialNiagaraGraphPinDirection::Input
						? TEXT("input") : TEXT("output");
				FUeAgentContentGraphItem Captured;
				Captured.ItemId = Pin.GetPinKey();
				Captured.ItemKind = TEXT("material_endpoint_") + Direction;
				Captured.OwnerId = Pin.GetOwnerItemKey();
				Captured.ClassPath = TEXT("uai.material.endpoint");
				Captured.ContainerId = RootContainerId;
				Captured.Order = PinOrder++;
				Captured.SemanticIdentity = SemanticIdentity(
					TEXT("uai.semantic.material.endpoint"),
					{{TEXT("name"), Pin.GetNativeName()},
						{TEXT("index"), FString::FromInt(Pin.GetNativeIndex())},
						{TEXT("direction"), Direction}},
					TEXT("endpoint"));
				OutCapture.Items.Add(MoveTemp(Captured));
				OutCapture.Relations.Add({
					TEXT("material.endpoint:") + Pin.GetPinKey(),
					TEXT("endpoint"),
					Pin.GetOwnerItemKey(),
					Pin.GetPinKey(),
				});
			}
			for (const FMaterialNiagaraGraphLinkIdentity& Link : Topology.GetLinks())
			{
				OutCapture.Relations.Add({
					Link.GetLinkKey(),
					TEXT("material_connection"),
					Link.GetFromPinKey(),
					Link.GetToPinKey(),
				});
			}
			for (const FMaterialNiagaraGraphOrderingIdentity& Ordering
				: Topology.GetOrdering())
			{
				OutCapture.Relations.Add({
					Ordering.GetOrderingKey(),
					TEXT("material_order"),
					Ordering.GetPredecessorItemKey().IsEmpty()
						? RootContainerId
						: Ordering.GetPredecessorItemKey(),
					Ordering.GetItemKey(),
				});
			}
			return true;
		}

		bool AugmentAudioCapture(
			UObject& Owner,
			const FString& RootContainerId,
			FUeAgentContentGraphCapture& OutCapture,
			FString& OutNativeRevision,
			FUeAgentContentGraphReadError& OutError)
		{
			OutNativeRevision.Reset();
			TArray<FString> RevisionFacts;
			auto AddItem = [&] (
				const FString& Id,
				const FString& Kind,
				const FString& ItemOwner,
				const FString& Type,
				const FString& Container,
				const int32 Order)
			{
				if (Id.IsEmpty() || OutCapture.Items.ContainsByPredicate(
					[&Id](const FUeAgentContentGraphItem& Item)
					{
						return Item.ItemId == Id;
					}))
				{
					return;
				}
				FUeAgentContentGraphItem Item;
				Item.ItemId = Id;
				Item.ItemKind = Kind;
				Item.OwnerId = ItemOwner;
				Item.ClassPath = Type;
				Item.ContainerId = Container;
				Item.Order = Order;
				Item.SemanticIdentity = SemanticIdentity(
					TEXT("uai.semantic.audio_graph.item"),
					{{TEXT("identity"), Id}}, TEXT("item"));
				OutCapture.Items.Add(MoveTemp(Item));
				RevisionFacts.Add(Id + TEXT("|") + Kind + TEXT("|")
					+ ItemOwner + TEXT("|") + Type);
			};
			auto AddRelation = [&] (
				const FString& Id,
				const FString& Kind,
				const FString& Source,
				const FString& Target)
			{
				if (Id.IsEmpty() || OutCapture.Relations.ContainsByPredicate(
					[&Id](const FUeAgentContentGraphRelation& Relation)
					{
						return Relation.RelationId == Id;
					}))
				{
					return;
				}
				FUeAgentContentGraphRelation Relation;
				Relation.RelationId = Id;
				Relation.RelationKind = Kind;
				Relation.SourceItemId = Source;
				Relation.TargetItemId = Target;
				Relation.SemanticIdentity = SemanticIdentity(
					TEXT("uai.semantic.audio_graph.relation"),
					{{TEXT("identity"), Id}}, TEXT("relation"));
				OutCapture.Relations.Add(MoveTemp(Relation));
				RevisionFacts.Add(Id + TEXT("|") + Kind + TEXT("|")
					+ Source + TEXT("|") + Target);
			};

			if (USoundCue* Cue = Cast<USoundCue>(&Owner))
			{
				const FString GraphId = TEXT("sound_cue:graph");
				OutCapture.Containers.Add({GraphId, TEXT("sound_cue_graph"),
					Owner.GetPathName(), RootContainerId,
					OutCapture.Containers.Num()});
				for (int32 Index = 0; Index < Cue->AllNodes.Num(); ++Index)
				{
					USoundNode* Node = Cue->AllNodes[Index];
					if (!Node) continue;
					const FString NodeId = TEXT("sound_node:") + Node->GetPathName();
					AddItem(NodeId, TEXT("sound_cue_node"), Owner.GetPathName(),
						Node->GetClass()->GetPathName(), GraphId, Index);
					for (int32 ChildIndex = 0;
						ChildIndex < Node->ChildNodes.Num(); ++ChildIndex)
					{
						USoundNode* Child = Node->ChildNodes[ChildIndex];
						if (!Child) continue;
						const FString ChildId = TEXT("sound_node:") + Child->GetPathName();
						AddRelation(FString::Printf(TEXT("sound_child:%s:%d:%s"),
							*NodeId, ChildIndex, *ChildId),
							TEXT("sound_cue_connection"), NodeId, ChildId);
					}
				}
				if (Cue->FirstNode)
				{
					AddRelation(TEXT("sound_root:") + Cue->FirstNode->GetPathName(),
						TEXT("sound_cue_root"), RootContainerId,
						TEXT("sound_node:") + Cue->FirstNode->GetPathName());
				}
			}
			else if (UMetaSoundSource* Source = Cast<UMetaSoundSource>(&Owner))
			{
				const IMetaSoundDocumentInterface* Interface =
					Cast<IMetaSoundDocumentInterface>(Source);
				if (!Interface)
				{
					return Fail(OutError,
						TEXT("content_graph_read_metasound_document_missing"),
						Owner.GetPathName());
				}
				const FString GraphId = TEXT("metasound:graph");
				OutCapture.Containers.Add({GraphId, TEXT("metasound_graph"),
					Owner.GetPathName(), RootContainerId,
					OutCapture.Containers.Num()});
				const FMetasoundFrontendDocument& Document =
					Interface->GetConstDocument();
				const FMetasoundFrontendGraph& Graph =
					Document.RootGraph.GetConstDefaultGraph();
				TMap<FGuid, FString> DependencyNames;
				for (const FMetasoundFrontendClass& Dependency : Document.Dependencies)
				{
					const FString Name = Dependency.Metadata.GetClassName().ToString();
					DependencyNames.Add(Dependency.ID, Name);
					AddItem(TEXT("metasound_dependency:")
						+ Dependency.ID.ToString(EGuidFormats::DigitsWithHyphensLower),
						TEXT("metasound_dependency"), Owner.GetPathName(), Name,
						GraphId, OutCapture.Items.Num());
				}
				for (const FString& Reference : Source->GetReferencedAssetClassKeys())
				{
					AddItem(TEXT("audio_reference:") + Reference,
						TEXT("external_audio_reference"), Owner.GetPathName(),
						Reference, GraphId, OutCapture.Items.Num());
				}
				const FMetasoundFrontendClassInterface& GraphInterface =
					Document.RootGraph.GetDefaultInterface();
				auto AddEndpoint = [&] (
					const FMetasoundFrontendClassVertex& Vertex,
					const FString& Direction,
					const int32 Order)
				{
					AddItem(FString::Printf(TEXT("metasound_endpoint:%s:%s:%s"),
						*Direction, *Vertex.Name.ToString(), *Vertex.TypeName.ToString()),
						TEXT("metasound_endpoint"), Owner.GetPathName(),
						Vertex.TypeName.ToString(), GraphId, Order);
				};
				for (int32 Index = 0; Index < GraphInterface.Inputs.Num(); ++Index)
				{
					AddEndpoint(GraphInterface.Inputs[Index], TEXT("input"), Index);
				}
				for (int32 Index = 0; Index < GraphInterface.Outputs.Num(); ++Index)
				{
					AddEndpoint(GraphInterface.Outputs[Index], TEXT("output"), Index);
				}
				for (int32 Index = 0; Index < Graph.Nodes.Num(); ++Index)
				{
					const FMetasoundFrontendNode& Node = Graph.Nodes[Index];
					const FString NodeId = TEXT("metasound_node:")
						+ Node.GetID().ToString(EGuidFormats::DigitsWithHyphensLower);
					AddItem(NodeId, TEXT("metasound_node"), Owner.GetPathName(),
						DependencyNames.FindRef(Node.ClassID), GraphId, Index);
					for (int32 InputIndex = 0;
						InputIndex < Node.Interface.Inputs.Num(); ++InputIndex)
					{
						const FMetasoundFrontendVertex& Input =
							Node.Interface.Inputs[InputIndex];
						AddItem(FString::Printf(TEXT("metasound_parameter:%s:%s"),
							*NodeId, *Input.VertexID.ToString(
								EGuidFormats::DigitsWithHyphensLower)),
							TEXT("metasound_parameter"), NodeId,
							Input.TypeName.ToString(), GraphId, InputIndex);
					}
				}
				for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
				{
					const FString SourceId = TEXT("metasound_node:")
						+ Edge.FromNodeID.ToString(EGuidFormats::DigitsWithHyphensLower);
					const FString TargetId = TEXT("metasound_node:")
						+ Edge.ToNodeID.ToString(EGuidFormats::DigitsWithHyphensLower);
					AddRelation(FString::Printf(TEXT("metasound_link:%s:%s:%s:%s"),
						*SourceId, *Edge.FromVertexID.ToString(
							EGuidFormats::DigitsWithHyphensLower), *TargetId,
						*Edge.ToVertexID.ToString(EGuidFormats::DigitsWithHyphensLower)),
						TEXT("metasound_connection"), SourceId, TargetId);
				}
			}
			else
			{
				return Fail(OutError,
					TEXT("content_graph_read_audio_target_unsupported"),
					Owner.GetPathName());
			}
			RevisionFacts.Add(TEXT("compile_settled|true"));
			OutNativeRevision = CaptureRevision(Owner, MoveTemp(RevisionFacts));
			return true;
		}

		FString BehaviorTreeItemId(const UEdGraphNode& Node)
		{
			return Node.IsA<UBehaviorTreeGraphNode_Root>()
				? TEXT("node:root")
				: TEXT("node:") + Node.NodeGuid.ToString(
					EGuidFormats::DigitsWithHyphensLower);
		}

		bool AugmentTextureGraphCapture(
			UObject& Owner,
			const FString& RootContainerId,
			FUeAgentResourceTracker& Resources,
			FUeAgentContentGraphCapture& OutCapture,
			FString& OutNativeRevision,
			FUeAgentContentGraphReadError& OutError)
		{
			OutNativeRevision.Reset();
			UTextureGraph* TextureGraph = Cast<UTextureGraph>(&Owner);
			UTG_Graph* Graph = TextureGraph ? TextureGraph->Graph() : nullptr;
			if (!TextureGraph || !Graph)
			{
				return Fail(
					OutError,
					TEXT("content_graph_read_texture_graph_missing"),
					Owner.GetPathName());
			}

			const FString GraphContainerId =
				TEXT("texture_graph:graph:") + Owner.GetPathName();
			FUeAgentContentGraphContainer GraphContainer{
				GraphContainerId,
				TEXT("texture_graph"),
				Owner.GetPathName(),
				RootContainerId,
				OutCapture.Containers.Num(),
				SemanticIdentity(
					TEXT("uai.semantic.texture_graph.container"),
					{{TEXT("asset_path"), Owner.GetPathName()}},
					TEXT("container")),
			};
			TSharedRef<FJsonObject> GraphContainerState =
				MakeShared<FJsonObject>();
			GraphContainerState->SetStringField(
				TEXT("asset_path"), Owner.GetPathName());
			GraphContainerState->SetStringField(
				TEXT("kind"), TEXT("texture_graph"));
			if (!TryCanonicalObject(
				GraphContainerState,
				Resources,
				GraphContainer.State,
				OutError))
			{
				return false;
			}
			OutCapture.Containers.Add(MoveTemp(GraphContainer));

			TArray<FString> RevisionFacts;
			TMap<FString, const UTG_Pin*> PinsByIdentity;
			auto NodeId = [](const int32 NodeIndex)
			{
				return FString::Printf(
					TEXT("texture_graph.node:%d"), NodeIndex);
			};
			auto PinId = [](const int32 NodeIndex, const int32 PinIndex)
			{
				return FString::Printf(
					TEXT("texture_graph.pin:%d:%d"),
					NodeIndex,
					PinIndex);
			};

			bool bCaptureValid = true;
			Graph->ForEachNodes(
				[&](const UTG_Node* Node, const uint32)
				{
					if (!bCaptureValid || !Node || !Node->GetExpression())
					{
						return;
					}
					const int32 Index = Node->GetId().NodeIdx();
					const FString Id = NodeId(Index);
					const FString ClassPath =
						Node->GetExpression()->GetClass()->GetPathName();
					FUeAgentContentGraphItem Item{
						Id,
						TEXT("texture_graph_node"),
						Owner.GetPathName(),
						ClassPath,
						GraphContainerId,
						Index,
						SemanticIdentity(
							TEXT("uai.semantic.texture_graph.node"),
							{{TEXT("node_index"), FString::FromInt(Index)},
							 {TEXT("item_id"), Id},
							 {TEXT("expression_path"),
								Node->GetExpression()->GetPathName()}},
							TEXT("item")),
					};
					TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
					State->SetNumberField(TEXT("node_index"), Index);
					State->SetStringField(
						TEXT("expression_path"),
						Node->GetExpression()->GetPathName());
					State->SetStringField(TEXT("class_path"), ClassPath);
					bCaptureValid = TryCanonicalObject(
						State, Resources, Item.State, OutError);
					if (!bCaptureValid)
					{
						return;
					}
					OutCapture.Items.Add(MoveTemp(Item));
					RevisionFacts.Add(Id + TEXT("|") + ClassPath + TEXT("|")
						+ Node->GetExpression()->GetName());
				});
			if (!bCaptureValid)
			{
				return false;
			}

			Graph->ForEachPins(
				[&](const UTG_Pin* Pin, const uint32, const uint32)
				{
					if (!Pin)
					{
						return;
					}
					const int32 NodeIndex = Pin->GetId().NodeIdx();
					const int32 PinIndex = Pin->GetId().PinIdx();
					const FString Id = PinId(NodeIndex, PinIndex);
					const FString ParentId = NodeId(NodeIndex);
					const FString Direction = Pin->IsInput()
						? TEXT("input") : TEXT("output");
					const FString ArgumentName =
						Pin->GetArgumentName().ToString();
					const FString CppType =
						Pin->GetArgumentCPPTypeName().ToString();
					const FString EvaluatedValue = Pin->GetEvaluatedVarValue();
					PinsByIdentity.Add(Id, Pin);
					FUeAgentContentGraphItem PinItem{
						Id,
						TEXT("texture_graph_pin_" ) + Direction,
						Owner.GetPathName(),
						CppType,
						GraphContainerId,
						PinIndex,
						SemanticIdentity(
							TEXT("uai.semantic.texture_graph.pin"),
							{{TEXT("node_index"), FString::FromInt(NodeIndex)},
							 {TEXT("pin_index"), FString::FromInt(PinIndex)},
							 {TEXT("item_id"), Id},
							 {TEXT("owner_item_id"), ParentId},
							 {TEXT("name"), ArgumentName},
							 {TEXT("direction"), Direction}},
							TEXT("item")),
					};
					TSharedRef<FJsonObject> PinState = MakeShared<FJsonObject>();
					PinState->SetNumberField(TEXT("node_index"), NodeIndex);
					PinState->SetNumberField(TEXT("pin_index"), PinIndex);
					PinState->SetStringField(TEXT("name"), ArgumentName);
					PinState->SetStringField(TEXT("direction"), Direction);
					PinState->SetStringField(TEXT("cpp_type"), CppType);
					PinState->SetStringField(TEXT("value"), EvaluatedValue);
					bCaptureValid = TryCanonicalObject(
						PinState, Resources, PinItem.State, OutError);
					if (!bCaptureValid)
					{
						return;
					}
					OutCapture.Items.Add(MoveTemp(PinItem));
					FUeAgentContentGraphRelation Membership{
						TEXT("texture_graph.membership:") + Id,
						TEXT("texture_graph_node_pin"),
						ParentId,
						Id,
						SemanticIdentity(
							TEXT("uai.semantic.texture_graph.node_pin"),
							{{TEXT("pin_id"), Id}},
							TEXT("relation")),
					};
					TSharedRef<FJsonObject> MembershipState =
						MakeShared<FJsonObject>();
					MembershipState->SetStringField(TEXT("node_id"), ParentId);
					MembershipState->SetStringField(TEXT("pin_id"), Id);
					bCaptureValid = TryCanonicalObject(
						MembershipState,
						Resources,
						Membership.State,
						OutError);
					if (!bCaptureValid)
					{
						return;
					}
					OutCapture.Relations.Add(MoveTemp(Membership));

					FUeAgentContentGraphFieldState State;
					if (TryMakeManualFieldState(
						MakeShared<FJsonValueString>(EvaluatedValue),
						MakeShared<FJsonValueString>(FString()),
						TEXT("texture_graph.pin.") + CppType,
						true,
						Resources,
						State,
						OutError))
					{
						FUeAgentContentGraphProperty Property;
						Property.OwnerId = Id;
						Property.FieldId = TEXT("value");
						Property.State = State;
						Property.SemanticIdentity = SemanticIdentity(
							TEXT("uai.semantic.texture_graph.pin_value"),
							{{TEXT("pin_id"), Id},
							 {TEXT("owner_item_id"), Id},
							 {TEXT("cpp_type"), CppType}},
							TEXT("property"));
						OutCapture.Properties.Add(MoveTemp(Property));
						if (Pin->IsParam())
						{
							FUeAgentContentGraphParameter Parameter;
							Parameter.ParameterId = Id;
							Parameter.OwnerId = ParentId;
							Parameter.State = State;
							Parameter.SemanticIdentity = SemanticIdentity(
								TEXT("uai.semantic.texture_graph.parameter"),
								{{TEXT("pin_id"), Id},
								 {TEXT("owner_item_id"), ParentId},
								 {TEXT("name"), Pin->GetAliasName().ToString()}},
								TEXT("parameter"));
							OutCapture.Parameters.Add(MoveTemp(Parameter));
						}
					}
					RevisionFacts.Add(Id + TEXT("|") + ArgumentName + TEXT("|")
						+ Direction + TEXT("|") + CppType + TEXT("|")
						+ EvaluatedValue);
				});
			if (!bCaptureValid)
			{
				return false;
			}

			for (const TPair<FString, const UTG_Pin*>& Pair : PinsByIdentity)
			{
				const UTG_Pin* Pin = Pair.Value;
				if (!Pin || !Pin->IsOutput())
				{
					continue;
				}
				for (const FTG_Id& Edge : Pin->GetEdges())
				{
					const FString TargetId = PinId(
						Edge.NodeIdx(), Edge.PinIdx());
					if (!PinsByIdentity.Contains(TargetId))
					{
						continue;
					}
					const FString RelationId = TEXT("texture_graph.edge:")
						+ Pair.Key + TEXT("->") + TargetId;
					FUeAgentContentGraphRelation EdgeRelation{
						RelationId,
						TEXT("texture_graph_edge"),
						Pair.Key,
						TargetId,
						SemanticIdentity(
							TEXT("uai.semantic.texture_graph.edge"),
							{{TEXT("source_pin"), Pair.Key},
							 {TEXT("target_pin"), TargetId}},
							TEXT("relation")),
					};
					TSharedRef<FJsonObject> EdgeState = MakeShared<FJsonObject>();
					EdgeState->SetStringField(TEXT("source_pin"), Pair.Key);
					EdgeState->SetStringField(TEXT("target_pin"), TargetId);
					if (!TryCanonicalObject(
						EdgeState,
						Resources,
						EdgeRelation.State,
						OutError))
					{
						return false;
					}
					OutCapture.Relations.Add(MoveTemp(EdgeRelation));
					RevisionFacts.Add(RelationId);
				}
			}

			OutNativeRevision = CaptureRevision(Owner, MoveTemp(RevisionFacts));
			return !OutNativeRevision.IsEmpty();
		}

		bool AugmentAiBehaviorCapture(
			UObject& Owner,
			const FString& RootContainerId,
			FUeAgentContentGraphCapture& OutCapture,
			FString& OutNativeRevision,
			FUeAgentContentGraphReadError& OutError)
		{
			OutNativeRevision.Reset();
			TArray<FString> RevisionFacts;
			auto AddItem = [&] (
				const FString& ItemId,
				const FString& ItemKind,
				const FString& OwnerId,
				const FString& ClassPath,
				const FString& ContainerId,
				const int32 Order,
				const FString& SemanticId)
			{
				if (ItemId.IsEmpty() || OutCapture.Items.ContainsByPredicate(
					[&ItemId](const FUeAgentContentGraphItem& Item)
					{
						return Item.ItemId == ItemId;
					}))
				{
					return;
				}
				FUeAgentContentGraphItem Item;
				Item.ItemId = ItemId;
				Item.ItemKind = ItemKind;
				Item.OwnerId = OwnerId;
				Item.ClassPath = ClassPath;
				Item.ContainerId = ContainerId;
				Item.Order = Order;
				Item.SemanticIdentity = SemanticIdentity(
					SemanticId,
					{{TEXT("identity"), ItemId}},
					TEXT("item"));
				OutCapture.Items.Add(MoveTemp(Item));
				RevisionFacts.Add(ItemId + TEXT("|") + ItemKind + TEXT("|")
					+ OwnerId + TEXT("|") + ClassPath);
			};
			auto AddRelation = [&] (
				const FString& RelationId,
				const FString& RelationKind,
				const FString& Source,
				const FString& Target)
			{
				if (RelationId.IsEmpty() || OutCapture.Relations.ContainsByPredicate(
					[&RelationId](const FUeAgentContentGraphRelation& Relation)
					{
						return Relation.RelationId == RelationId;
					}))
				{
					return;
				}
				FUeAgentContentGraphRelation Relation;
				Relation.RelationId = RelationId;
				Relation.RelationKind = RelationKind;
				Relation.SourceItemId = Source;
				Relation.TargetItemId = Target;
				Relation.SemanticIdentity = SemanticIdentity(
					TEXT("uai.semantic.ai_behavior.relation"),
					{{TEXT("identity"), RelationId}},
					TEXT("relation"));
				OutCapture.Relations.Add(MoveTemp(Relation));
				RevisionFacts.Add(RelationId + TEXT("|") + RelationKind + TEXT("|")
					+ Source + TEXT("|") + Target);
			};

			if (UBehaviorTree* Tree = Cast<UBehaviorTree>(&Owner))
			{
#if WITH_EDITORONLY_DATA
				const UBehaviorTreeGraph* Graph =
					Cast<UBehaviorTreeGraph>(Tree->BTGraph);
				const FString GraphId = TEXT("behavior_tree:graph");
				OutCapture.Containers.Add({
					GraphId,
					TEXT("behavior_tree_graph"),
					Owner.GetPathName(),
					RootContainerId,
					OutCapture.Containers.Num(),
				});
				if (Graph)
				{
					TMap<const UEdGraphNode*, FString> NodeIds;
					for (int32 Index = 0; Index < Graph->Nodes.Num(); ++Index)
					{
						const UEdGraphNode* Node = Graph->Nodes[Index];
						if (!Node || (!Node->NodeGuid.IsValid()
							&& !Node->IsA<UBehaviorTreeGraphNode_Root>()))
						{
							continue;
						}
						const FString ItemId = BehaviorTreeItemId(*Node);
						const UAIGraphNode* AiNode = Cast<UAIGraphNode>(Node);
						const FString PayloadClass = AiNode && AiNode->NodeInstance
							? AiNode->NodeInstance->GetClass()->GetPathName()
							: Node->GetClass()->GetPathName();
						AddItem(ItemId, TEXT("behavior_node"), GraphId,
							PayloadClass, GraphId, Index,
							TEXT("uai.semantic.ai_behavior.behavior_node"));
						NodeIds.Add(Node, ItemId);
						const UBehaviorTreeGraphNode* BehaviorNode =
							Cast<UBehaviorTreeGraphNode>(Node);
						if (!BehaviorNode)
						{
							continue;
						}
						for (int32 SubIndex = 0;
							SubIndex < BehaviorNode->SubNodes.Num(); ++SubIndex)
						{
							const UAIGraphNode* SubNode = BehaviorNode->SubNodes[SubIndex];
							if (!SubNode || !SubNode->NodeGuid.IsValid()
								|| !SubNode->NodeInstance)
							{
								continue;
							}
							const FString SubItemId = TEXT("node:")
								+ SubNode->NodeGuid.ToString(
									EGuidFormats::DigitsWithHyphensLower);
							AddItem(SubItemId, TEXT("behavior_subnode"), ItemId,
								SubNode->NodeInstance->GetClass()->GetPathName(),
								GraphId, SubIndex,
								TEXT("uai.semantic.ai_behavior.behavior_subnode"));
							AddRelation(TEXT("attachment:") + ItemId + TEXT(":")
								+ SubItemId, TEXT("behavior_attachment"),
								ItemId, SubItemId);
						}
					}
					for (const UEdGraphNode* Node : Graph->Nodes)
					{
						const FString* SourceId = NodeIds.Find(Node);
						if (!Node || !SourceId)
						{
							continue;
						}
						for (int32 PinIndex = 0; PinIndex < Node->Pins.Num(); ++PinIndex)
						{
							const UEdGraphPin* Pin = Node->Pins[PinIndex];
							if (!Pin || Pin->Direction != EGPD_Output)
							{
								continue;
							}
							for (const UEdGraphPin* Linked : Pin->LinkedTo)
							{
								const FString* TargetId = Linked
									? NodeIds.Find(Linked->GetOwningNode()) : nullptr;
								if (!TargetId)
								{
									continue;
								}
								const FString RelationId = FString::Printf(
									TEXT("edge:%s:%d:%s:%d"), **SourceId,
									PinIndex, **TargetId,
									Linked->GetOwningNode()->Pins.IndexOfByKey(Linked));
								AddRelation(RelationId, TEXT("behavior_connection"),
									*SourceId, *TargetId);
							}
						}
					}
				}
#endif
				if (Tree->BlackboardAsset)
				{
					const FString BindingId = TEXT("binding:blackboard_asset");
					AddItem(BindingId, TEXT("blackboard_binding"),
						Owner.GetPathName(), Tree->BlackboardAsset->GetPathName(),
						RootContainerId, OutCapture.Items.Num(),
						TEXT("uai.semantic.ai_behavior.blackboard_binding"));
				}
			}
			else if (UBlackboardData* Blackboard = Cast<UBlackboardData>(&Owner))
			{
				auto AddEntries = [&] (
					const TArray<FBlackboardEntry>& Entries,
					const FString& Ownership)
				{
					for (int32 Index = 0; Index < Entries.Num(); ++Index)
					{
						const FBlackboardEntry& Entry = Entries[Index];
						if (Entry.EntryName.IsNone() || !Entry.KeyType)
						{
							continue;
						}
						const FString ClassPath =
							Entry.KeyType->GetClass()->GetPathName();
						const FString ItemId = TEXT("item:blackboard_entry:")
							+ Entry.EntryName.ToString() + TEXT(":") + ClassPath
							+ TEXT(":") + Ownership;
						AddItem(ItemId, TEXT("blackboard_entry"),
							Owner.GetPathName(), ClassPath, RootContainerId, Index,
							TEXT("uai.semantic.ai_behavior.blackboard_entry"));
					}
				};
				AddEntries(Blackboard->Keys, TEXT("local"));
#if WITH_EDITORONLY_DATA
				AddEntries(Blackboard->ParentKeys, TEXT("inherited"));
#endif
			}
			else if (UEnvQuery* Query = Cast<UEnvQuery>(&Owner))
			{
#if WITH_EDITORONLY_DATA
				const UEnvironmentQueryGraph* Graph =
					Cast<UEnvironmentQueryGraph>(Query->EdGraph);
				const FString GraphId = TEXT("eqs:graph");
				OutCapture.Containers.Add({
					GraphId,
					TEXT("eqs_graph"),
					Owner.GetPathName(),
					RootContainerId,
					OutCapture.Containers.Num(),
				});
				if (Graph)
				{
					for (int32 Index = 0; Index < Graph->Nodes.Num(); ++Index)
					{
						const UEnvironmentQueryGraphNode_Option* OptionNode =
							Cast<UEnvironmentQueryGraphNode_Option>(
								Graph->Nodes[Index]);
						const UEnvQueryOption* Option = OptionNode
							? Cast<UEnvQueryOption>(
								OptionNode->NodeInstance.Get())
							: nullptr;
						if (!OptionNode || !OptionNode->NodeGuid.IsValid()
							|| !Option || !Option->Generator)
						{
							continue;
						}
						const FString ItemId = TEXT("node:")
							+ OptionNode->NodeGuid.ToString(
								EGuidFormats::DigitsWithHyphensLower);
						AddItem(
							ItemId,
							TEXT("eqs_option"),
							GraphId,
							Option->Generator->GetClass()->GetPathName(),
							GraphId,
							Index,
							TEXT("uai.semantic.ai_behavior.eqs_option"));
					}
				}
#endif
			}
			else if (USmartObjectDefinition* Definition =
				Cast<USmartObjectDefinition>(&Owner))
			{
				const FString SlotsId = TEXT("smart_object:slots");
				OutCapture.Containers.Add({
					SlotsId,
					TEXT("smart_object_slots"),
					Owner.GetPathName(),
					RootContainerId,
					OutCapture.Containers.Num(),
				});
				const TConstArrayView<FSmartObjectSlotDefinition> Slots =
					Definition->GetSlots();
				for (int32 Index = 0; Index < Slots.Num(); ++Index)
				{
					const FSmartObjectSlotDefinition& Slot = Slots[Index];
					FString ItemId;
#if WITH_EDITORONLY_DATA
					if (!Slot.ID.IsValid())
					{
						continue;
					}
					ItemId = TEXT("item:slot:")
						+ Slot.ID.ToString(
							EGuidFormats::DigitsWithHyphensLower);
#else
					continue;
#endif
					const USmartObjectBehaviorDefinition* Behavior =
						Slot.BehaviorDefinitions.IsEmpty()
							? nullptr
							: Slot.BehaviorDefinitions[0].Get();
					AddItem(
						ItemId,
						TEXT("smart_object_slot"),
						Owner.GetPathName(),
						Behavior
							? Behavior->GetClass()->GetPathName()
							: TEXT("none"),
						SlotsId,
						Index,
						TEXT("uai.semantic.ai_behavior.smart_object_slot"));
				}
			}
			else if (UStateTree* StateTreeAsset = Cast<UStateTree>(&Owner))
			{
#if WITH_EDITORONLY_DATA
				UStateTreeEditorData* EditorData =
					Cast<UStateTreeEditorData>(StateTreeAsset->EditorData);
				if (!EditorData)
				{
					return Fail(OutError,
						TEXT("content_graph_read_state_tree_editor_data_missing"),
						Owner.GetPathName());
				}
				const FString TreeId = TEXT("state_tree:editor_data");
				OutCapture.Containers.Add({
					TreeId,
					TEXT("state_tree_editor_data"),
					Owner.GetPathName(),
					RootContainerId,
					OutCapture.Containers.Num(),
				});
				auto AddNodes = [&] (
					const TArray<FStateTreeEditorNode>& Nodes,
					const FString& OwnerState,
					const FString& Placement)
				{
					for (int32 Index = 0; Index < Nodes.Num(); ++Index)
					{
						const FStateTreeEditorNode& Node = Nodes[Index];
						if (!Node.ID.IsValid())
						{
							continue;
						}
						const FString ClassPath = Node.InstanceObject
							? Node.InstanceObject->GetClass()->GetPathName()
							: (Node.Node.GetScriptStruct()
								? Node.Node.GetScriptStruct()->GetPathName()
								: FString());
						if (ClassPath.IsEmpty())
						{
							continue;
						}
						const FString ItemId = TEXT("node:")
							+ Node.ID.ToString(EGuidFormats::DigitsWithHyphensLower);
						AddItem(ItemId, Placement, OwnerState, ClassPath,
							TreeId, Index,
							TEXT("uai.semantic.ai_behavior.state_tree_node"));
						AddRelation(TEXT("placement:") + OwnerState + TEXT(":")
							+ ItemId, TEXT("state_tree_placement"),
							OwnerState, ItemId);
					}
				};
				AddNodes(EditorData->GlobalTasks, TEXT("global"), TEXT("global_task"));
				AddNodes(EditorData->Evaluators, TEXT("global"), TEXT("evaluator"));
				struct FPendingState
				{
					UStateTreeState* State = nullptr;
					FString ParentId;
				};
				TArray<FPendingState> Pending;
				for (int32 Index = EditorData->SubTrees.Num() - 1; Index >= 0; --Index)
				{
					Pending.Add({EditorData->SubTrees[Index], TreeId});
				}
				TSet<const UStateTreeState*> Visited;
				while (!Pending.IsEmpty())
				{
					const FPendingState Current = Pending.Pop(EAllowShrinking::No);
					if (!Current.State || Visited.Contains(Current.State)
						|| !Current.State->ID.IsValid())
					{
						return Fail(OutError,
							TEXT("content_graph_read_state_tree_hierarchy_invalid"),
							Owner.GetPathName());
					}
					Visited.Add(Current.State);
					const FString StateId = TEXT("state:")
						+ Current.State->ID.ToString(
							EGuidFormats::DigitsWithHyphensLower);
					AddItem(StateId, TEXT("state_tree_state"), Current.ParentId,
						Current.State->GetClass()->GetPathName(), TreeId,
						OutCapture.Items.Num(),
						TEXT("uai.semantic.ai_behavior.state_tree_state"));
					AddRelation(TEXT("hierarchy:") + Current.ParentId + TEXT(":")
						+ StateId, TEXT("state_tree_hierarchy"),
						Current.ParentId, StateId);
					AddNodes(Current.State->Tasks, StateId, TEXT("state_task"));
					AddNodes(Current.State->EnterConditions,
						StateId, TEXT("state_condition"));
					for (int32 Index = Current.State->Children.Num() - 1;
						Index >= 0; --Index)
					{
						Pending.Add({Current.State->Children[Index], StateId});
					}
				}
#endif
			}

			OutNativeRevision = CaptureRevision(Owner, MoveTemp(RevisionFacts));
			return true;
		}

		bool CaptureBlueprint(
			UBlueprint& Blueprint,
			FUeAgentContentGraphCapture& OutCapture,
			FUeAgentContentGraphReadError& OutError)
		{
			const FString OwnerId = Blueprint.GetPathName();
			const TMap<FString, FString> AssetSelectors = {
				{TEXT("asset_path"), OwnerId},
			};
			FUeAgentResourceTracker Resources = MakeCanonicalResources();
			TArray<FString> RevisionFacts;

			TSharedRef<FJsonObject> TargetState = MakeShared<FJsonObject>();
			TargetState->SetStringField(TEXT("asset_path"), OwnerId);
			const bool bWidgetBlueprint = Blueprint.IsA<UWidgetBlueprint>();
			TargetState->SetStringField(
				TEXT("asset_kind"),
				bWidgetBlueprint ? TEXT("widget_blueprint") : TEXT("blueprint"));
			TargetState->SetStringField(TEXT("parent_class"),
				Blueprint.ParentClass ? Blueprint.ParentClass->GetPathName() : FString());
			TArray<FString> InterfacePaths;
			for (const FBPInterfaceDescription& Description : Blueprint.ImplementedInterfaces)
			{
				if (const UClass* InterfaceClass = Description.Interface.Get())
				{
					InterfacePaths.Add(InterfaceClass->GetPathName());
				}
			}
			InterfacePaths.Sort();
			TArray<TSharedPtr<FJsonValue>> InterfaceValues;
			for (const FString& InterfacePath : InterfacePaths)
			{
				InterfaceValues.Add(MakeShared<FJsonValueString>(InterfacePath));
				RevisionFacts.Add(TEXT("interface|") + InterfacePath);
			}
			TargetState->SetArrayField(TEXT("interfaces"), MoveTemp(InterfaceValues));
			RevisionFacts.Add(TEXT("parent|")
				+ (Blueprint.ParentClass ? Blueprint.ParentClass->GetPathName() : FString()));
			FUeAgentCanonicalValuePtr TargetValue;
			if (!TryCanonicalObject(
				TargetState,
				Resources,
				TargetValue,
				OutError))
			{
				return false;
			}
			FUeAgentContentGraphContainer Target;
			Target.ContainerId = TEXT("asset:") + OwnerId;
			Target.ContainerKind = bWidgetBlueprint
				? TEXT("widget_blueprint_asset")
				: TEXT("blueprint_asset");
			Target.OwnerId = OwnerId;
			Target.SemanticIdentity = SemanticIdentity(
				TEXT("uai.semantic.blueprint.export.target"),
				AssetSelectors,
				TEXT("target"));
			Target.State = TargetValue;
			OutCapture.Containers.Add(MoveTemp(Target));
			RevisionFacts.Add(TEXT("target|") + OwnerId);

			if (!AddMappedProperty(
				Blueprint,
				TEXT("BlueprintDescription"),
				TEXT("blueprint_description"),
				OwnerId,
				TEXT("uai.semantic.blueprint.asset.property"),
				AssetSelectors,
				Resources,
				OutCapture.Properties,
				RevisionFacts,
				OutError))
			{
				return false;
			}

			int32 BlueprintPropertyCount = 0;
			const TSet<FName> BlueprintPropertyExclusions = {
				TEXT("BlueprintDescription"), TEXT("ParentClass"),
				TEXT("GeneratedClass"), TEXT("SkeletonGeneratedClass"),
				TEXT("SimpleConstructionScript"), TEXT("UbergraphPages"),
				TEXT("FunctionGraphs"), TEXT("MacroGraphs"),
				TEXT("DelegateSignatureGraphs"), TEXT("NewVariables"),
				TEXT("ImplementedInterfaces"),
			};
			if (!AddProperties(Blueprint, Resources, OutCapture.Properties,
				BlueprintPropertyCount, OutError, &RevisionFacts,
				&BlueprintPropertyExclusions))
			{
				return false;
			}
			if (Blueprint.GeneratedClass)
			{
				if (UObject* ClassDefault = Blueprint.GeneratedClass->GetDefaultObject(false))
				{
					if (!AddProperties(*ClassDefault, Resources, OutCapture.Properties,
						BlueprintPropertyCount, OutError, &RevisionFacts))
					{
						return false;
					}
				}
			}

			int32 SurfaceOrder = 0;
			for (const FString& InterfacePath : InterfacePaths)
			{
				TSharedRef<FJsonObject> InterfaceState = MakeShared<FJsonObject>();
				InterfaceState->SetStringField(TEXT("class_path"), InterfacePath);
				FUeAgentCanonicalValuePtr InterfaceValue;
				if (!TryCanonicalObject(InterfaceState, Resources, InterfaceValue, OutError))
				{
					return false;
				}
				FUeAgentContentGraphItem Item;
				Item.ItemId = TEXT("interface:") + InterfacePath;
				Item.ItemKind = TEXT("blueprint_interface");
				Item.OwnerId = OwnerId;
				Item.ClassPath = InterfacePath;
				Item.ContainerId = TEXT("asset:") + OwnerId;
				Item.Order = SurfaceOrder++;
				Item.SemanticIdentity = SemanticIdentity(
					TEXT("uai.semantic.blueprint.interface"),
					{ {TEXT("asset_path"), OwnerId},
						{TEXT("interface_class"), InterfacePath} }, TEXT("item"));
				Item.State = InterfaceValue;
				OutCapture.Items.Add(MoveTemp(Item));
			}
			TArray<const FBPVariableDescription*> Members;
			for (const FBPVariableDescription& Variable : Blueprint.NewVariables)
			{
				Members.Add(&Variable);
			}
			Members.Sort([](const FBPVariableDescription& Left,
				const FBPVariableDescription& Right)
			{
				return Left.VarName.LexicalLess(Right.VarName);
			});
			for (const FBPVariableDescription* Variable : Members)
			{
				if (!Variable) continue;
				const FString Guid = GuidText(Variable->VarGuid);
				const FString Kind = Variable->VarType.PinCategory
					== UEdGraphSchema_K2::PC_MCDelegate
					? TEXT("delegate") : TEXT("variable");
				TSharedRef<FJsonObject> MemberState = MakeShared<FJsonObject>();
				MemberState->SetStringField(TEXT("name"), Variable->VarName.ToString());
				MemberState->SetStringField(TEXT("kind"), Kind);
				MemberState->SetStringField(TEXT("pin_category"),
					Variable->VarType.PinCategory.ToString());
				FUeAgentCanonicalValuePtr MemberValue;
				if (!TryCanonicalObject(MemberState, Resources, MemberValue, OutError))
				{
					return false;
				}
				FUeAgentContentGraphItem Item;
				Item.ItemId = TEXT("member:") + (!Guid.IsEmpty()
					? Guid : Variable->VarName.ToString());
				Item.ItemKind = TEXT("blueprint_member");
				Item.OwnerId = OwnerId;
				Item.ClassPath = Variable->VarType.PinCategory.ToString();
				Item.ContainerId = TEXT("asset:") + OwnerId;
				Item.Order = SurfaceOrder++;
				Item.SemanticIdentity = SemanticIdentity(
					TEXT("uai.semantic.blueprint.member.variable"),
					{ {TEXT("asset_path"), OwnerId}, {TEXT("member_guid"), Guid},
						{TEXT("member_kind"), Kind},
						{TEXT("member_name"), Variable->VarName.ToString()} },
					TEXT("item"));
				Item.State = MemberValue;
				RevisionFacts.Add(Item.ItemId + TEXT("|")
					+ Variable->VarType.PinCategory.ToString());
				OutCapture.Items.Add(MoveTemp(Item));
			}

			FBlueprintUmgGraphNativeHook Hook;
			auto AppendGraphs = [&] (
				const TArray<TObjectPtr<UEdGraph>>& Graphs,
				const FString& GraphKind) -> bool
			{
				for (UEdGraph* Graph : Graphs)
				{
					if (!Graph)
					{
						continue;
					}
					const FString GraphGuid = GuidText(Graph->GraphGuid);
					const FString GraphId = !GraphGuid.IsEmpty()
						? TEXT("graph:") + GraphGuid
						: TEXT("graph:") + Graph->GetName();
					TMap<FString, FString> GraphSelectors = AssetSelectors;
					GraphSelectors.Add(TEXT("graph_guid"), GraphGuid);
					GraphSelectors.Add(TEXT("graph_kind"), GraphKind);
					GraphSelectors.Add(TEXT("graph_name"), Graph->GetName());
					FBlueprintUmgGraphTopology Topology;
					FString TopologyRevision;
					FString Evidence;
					FBlueprintUmgGraphAdapterError HookError;
					if (!Hook.TryCaptureBlueprintGraph(
						Blueprint,
						*Graph,
						OwnerId,
						GraphId,
						Topology,
						TopologyRevision,
						Evidence,
						HookError))
					{
						return Fail(
							OutError,
							HookError.GetCode(),
							Graph->GetPathName(),
							HookError.GetDetail());
					}
					TSharedRef<FJsonObject> GraphState = MakeShared<FJsonObject>();
					GraphState->SetStringField(TEXT("name"), Graph->GetName());
					GraphState->SetStringField(TEXT("graph_kind"), GraphKind);
					FUeAgentCanonicalValuePtr GraphValue;
					if (!TryCanonicalObject(
						GraphState,
						Resources,
						GraphValue,
						OutError))
					{
						return false;
					}
					FUeAgentContentGraphContainer CapturedGraph;
					CapturedGraph.ContainerId = GraphId;
					CapturedGraph.ContainerKind = TEXT("blueprint_graph");
					CapturedGraph.OwnerId = OwnerId;
					CapturedGraph.Order = OutCapture.Containers.Num();
					CapturedGraph.SemanticIdentity = SemanticIdentity(
						TEXT("uai.semantic.blueprint.graph"),
						GraphSelectors,
						TEXT("structure"));
					CapturedGraph.State = GraphValue;
					OutCapture.Containers.Add(MoveTemp(CapturedGraph));
					RevisionFacts.Add(TopologyRevision);

					TMap<FString, UEdGraphNode*> NodesByGuid;
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						if (Node && Node->NodeGuid.IsValid())
						{
							NodesByGuid.Add(GuidText(Node->NodeGuid), Node);
						}
					}
					for (int32 Index = 0; Index < Topology.GetItems().Num(); ++Index)
					{
						const FBlueprintUmgGraphItemIdentity& Item =
							Topology.GetItems()[Index];
						UEdGraphNode* Node = NodesByGuid.FindRef(Item.GetNativeGuid());
						TMap<FString, FString> NodeSelectors = GraphSelectors;
						NodeSelectors.Add(TEXT("node_key"), Item.GetItemKey());
						if (!Item.GetNativeGuid().IsEmpty())
						{
							NodeSelectors.Add(TEXT("node_guid"), Item.GetNativeGuid());
						}
						NodeSelectors.Add(
							TEXT("node_kind"),
							Node ? Node->GetClass()->GetName() : TEXT("builtin"));
						TSharedRef<FJsonObject> NodeState = MakeShared<FJsonObject>();
						NodeState->SetStringField(TEXT("id"), Item.GetItemKey());
						NodeState->SetStringField(
							TEXT("node_type"),
							Node ? Node->GetClass()->GetName() : TEXT("builtin"));
						if (Node)
						{
							TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
							Position->SetNumberField(TEXT("x"), Node->NodePosX);
							Position->SetNumberField(TEXT("y"), Node->NodePosY);
							NodeState->SetObjectField(TEXT("pos"), Position);
							UeAgentBlueprintAuthoring::
								AppendGraphNodeSemanticState(*Node, *NodeState);
						}
						FUeAgentCanonicalValuePtr NodeValue;
						if (!TryCanonicalObject(
							NodeState,
							Resources,
							NodeValue,
							OutError))
						{
							return false;
						}
						FUeAgentContentGraphItem CapturedNode;
						CapturedNode.ItemId = Item.GetItemKey();
						CapturedNode.ItemKind = TEXT("blueprint_node");
						CapturedNode.OwnerId = GraphId;
						CapturedNode.ClassPath = Node
							? Node->GetClass()->GetPathName()
							: TEXT("blueprint_builtin_node");
						CapturedNode.ContainerId = GraphId;
						CapturedNode.Order = Index;
						CapturedNode.SemanticIdentity = SemanticIdentity(
							TEXT("uai.semantic.blueprint.node"),
							NodeSelectors,
							TEXT("item"));
						CapturedNode.State = NodeValue;
						OutCapture.Items.Add(MoveTemp(CapturedNode));
						RevisionFacts.Add(
							Item.GetItemKey() + TEXT("|")
								+ (Node
									? FString::Printf(
										TEXT("%d|%d"),
										Node->NodePosX,
										Node->NodePosY)
									: Item.GetStableBuiltinRole()));
						if (Node
							&& (!AddMappedProperty(
								*Node,
								TEXT("NodeComment"),
								TEXT("node_comment"),
								Item.GetItemKey(),
								TEXT("uai.semantic.blueprint.node.property"),
								NodeSelectors,
								Resources,
								OutCapture.Properties,
								RevisionFacts,
								OutError)
								|| !AddMappedProperty(
									*Node,
									TEXT("bCommentBubbleVisible"),
									TEXT("comment_bubble_visible"),
									Item.GetItemKey(),
									TEXT("uai.semantic.blueprint.node.property"),
									NodeSelectors,
									Resources,
									OutCapture.Properties,
									RevisionFacts,
									OutError)))
						{
							return false;
						}
					}

					TMap<FString, FBlueprintUmgGraphPinIdentity> PinsByKey;
					for (const FBlueprintUmgGraphPinIdentity& Pin : Topology.GetPins())
					{
						PinsByKey.Add(Pin.GetPinKey(), Pin);
					}
					for (const FBlueprintUmgGraphLinkIdentity& Link : Topology.GetLinks())
					{
						const FBlueprintUmgGraphPinIdentity* From =
							PinsByKey.Find(Link.GetFromPinKey());
						const FBlueprintUmgGraphPinIdentity* To =
							PinsByKey.Find(Link.GetToPinKey());
						if (!From || !To)
						{
							return Fail(
								OutError,
								TEXT("content_graph_read_blueprint_edge_endpoint_missing"),
								Link.GetLinkKey());
						}
						TSharedRef<FJsonObject> EdgeState = MakeShared<FJsonObject>();
						EdgeState->SetStringField(
							TEXT("from_node"), From->GetOwnerItemKey());
						EdgeState->SetStringField(
							TEXT("from_pin"), From->GetNativeName());
						EdgeState->SetStringField(
							TEXT("to_node"), To->GetOwnerItemKey());
						EdgeState->SetStringField(
							TEXT("to_pin"), To->GetNativeName());
						FUeAgentCanonicalValuePtr EdgeValue;
						if (!TryCanonicalObject(
							EdgeState,
							Resources,
							EdgeValue,
							OutError))
						{
							return false;
						}
						TMap<FString, FString> EdgeSelectors = GraphSelectors;
						EdgeSelectors.Add(TEXT("edge_key"), Link.GetLinkKey());
						FUeAgentContentGraphRelation CapturedEdge;
						CapturedEdge.RelationId = Link.GetLinkKey();
						CapturedEdge.RelationKind = TEXT("blueprint_edge");
						CapturedEdge.SourceItemId = From->GetOwnerItemKey();
						CapturedEdge.TargetItemId = To->GetOwnerItemKey();
						CapturedEdge.SemanticIdentity = SemanticIdentity(
							TEXT("uai.semantic.blueprint.edge"),
							MoveTemp(EdgeSelectors),
							TEXT("relation"));
						CapturedEdge.State = EdgeValue;
						OutCapture.Relations.Add(MoveTemp(CapturedEdge));
						RevisionFacts.Add(Link.GetLinkKey());
					}
				}
				return true;
			};

			if (!AppendGraphs(Blueprint.UbergraphPages, TEXT("event_graph"))
				|| !AppendGraphs(Blueprint.FunctionGraphs, TEXT("function_graph"))
				|| !AppendGraphs(Blueprint.MacroGraphs, TEXT("macro_graph")))
			{
				return false;
			}

			if (Blueprint.SimpleConstructionScript)
			{
				FBlueprintUmgGraphTopology Scs;
				FString ScsRevision;
				FString Evidence;
				FBlueprintUmgGraphAdapterError HookError;
				if (!Hook.TryCaptureBlueprintScs(
					Blueprint,
					OwnerId,
					Scs,
					ScsRevision,
					Evidence,
					HookError))
				{
					return Fail(
						OutError,
						HookError.GetCode(),
						OwnerId,
						HookError.GetDetail());
				}
				TMap<FString, FString> Parents;
				for (const FBlueprintUmgGraphHierarchyRelation& Relation
					: Scs.GetHierarchy())
				{
					Parents.Add(Relation.GetItemKey(), Relation.GetParentKey());
					RevisionFacts.Add(Relation.GetRelationKey());
				}
				TMap<FString, USCS_Node*> NodesByKey;
				for (USCS_Node* Node
					: Blueprint.SimpleConstructionScript->GetAllNodes())
				{
					if (Node)
					{
						NodesByKey.Add(
							TEXT("component:")
								+ Node->GetVariableName().ToString(),
							Node);
						if (Node->ComponentTemplate)
						{
							TArray<FUeAgentContentGraphProperty>
								RevisionOnlyProperties;
							int32 RevisionOnlyPropertyCount = 0;
							if (!AddProperties(
									*Node->ComponentTemplate,
									Resources,
									RevisionOnlyProperties,
									RevisionOnlyPropertyCount,
									OutError,
									&RevisionFacts))
							{
								return false;
							}
						}
					}
				}
				for (int32 Index = 0; Index < Scs.GetItems().Num(); ++Index)
				{
					const FBlueprintUmgGraphItemIdentity& Item =
						Scs.GetItems()[Index];
					USCS_Node* Node = NodesByKey.FindRef(Item.GetItemKey());
					const FString ComponentName = Item.GetItemKey().Mid(10);
					const FString ComponentClass = Node && Node->ComponentClass
						? Node->ComponentClass->GetPathName()
						: TEXT("blueprint_component");
					TMap<FString, FString> Selectors = AssetSelectors;
					Selectors.Add(TEXT("component_name"), ComponentName);
					Selectors.Add(TEXT("component_kind"), ComponentClass);
					if (!Item.GetNativeGuid().IsEmpty())
					{
						Selectors.Add(TEXT("component_guid"), Item.GetNativeGuid());
					}
					TSharedRef<FJsonObject> ComponentState = MakeShared<FJsonObject>();
					ComponentState->SetStringField(TEXT("name"), ComponentName);
					ComponentState->SetStringField(TEXT("class"), ComponentClass);
					ComponentState->SetStringField(
						TEXT("parent"),
						Parents.FindRef(Item.GetItemKey()));
					FUeAgentCanonicalValuePtr ComponentValue;
					if (!TryCanonicalObject(
						ComponentState,
						Resources,
						ComponentValue,
						OutError))
					{
						return false;
					}
					FUeAgentContentGraphItem CapturedComponent;
					CapturedComponent.ItemId = Item.GetItemKey();
					CapturedComponent.ItemKind = TEXT("blueprint_component");
					CapturedComponent.OwnerId = OwnerId;
					CapturedComponent.ClassPath = ComponentClass;
					CapturedComponent.ContainerId = TEXT("asset:") + OwnerId;
					CapturedComponent.Order = Index;
					CapturedComponent.SemanticIdentity = SemanticIdentity(
						TEXT("uai.semantic.blueprint.component"),
						MoveTemp(Selectors),
						TEXT("item"));
					CapturedComponent.State = ComponentValue;
					OutCapture.Items.Add(MoveTemp(CapturedComponent));
					RevisionFacts.Add(Item.GetItemKey());
				}
				RevisionFacts.Add(ScsRevision);
			}

			if (UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(&Blueprint))
			{
				FBlueprintUmgGraphTopology WidgetTree;
				FString WidgetTreeRevision;
				FString Evidence;
				FBlueprintUmgGraphAdapterError HookError;
				if (!Hook.TryCaptureWidgetTree(
					*WidgetBlueprint,
					OwnerId,
					WidgetTree,
					WidgetTreeRevision,
					Evidence,
					HookError))
				{
					return Fail(
						OutError,
						HookError.GetCode(),
						OwnerId,
						HookError.GetDetail());
				}
				const FString TreeId(TEXT("tree:WidgetTree"));
				TSharedRef<FJsonObject> TreeState = MakeShared<FJsonObject>();
				TreeState->SetNumberField(
					TEXT("widget_count"),
					WidgetTree.GetItems().Num());
				FUeAgentCanonicalValuePtr TreeValue;
				if (!TryCanonicalObject(
					TreeState,
					Resources,
					TreeValue,
					OutError))
				{
					return false;
				}
				FUeAgentContentGraphContainer CapturedTree;
				CapturedTree.ContainerId = TreeId;
				CapturedTree.ContainerKind = TEXT("widget_tree");
				CapturedTree.OwnerId = OwnerId;
				CapturedTree.ParentContainerId = TEXT("asset:") + OwnerId;
				CapturedTree.Order = OutCapture.Containers.Num();
				CapturedTree.SemanticIdentity = SemanticIdentity(
					TEXT("uai.semantic.widget_blueprint.widget_tree"),
					AssetSelectors,
					TEXT("structure"));
				CapturedTree.State = TreeValue;
				OutCapture.Containers.Add(MoveTemp(CapturedTree));

				TMap<FString, UWidget*> WidgetsByKey;
				if (WidgetBlueprint->WidgetTree)
				{
					WidgetBlueprint->WidgetTree->ForEachWidget(
						[&WidgetsByKey](UWidget* Widget)
						{
							if (Widget)
							{
								WidgetsByKey.Add(
									TEXT("widget:") + Widget->GetName(),
									Widget);
							}
						});
				}
				for (int32 Index = 0; Index < WidgetTree.GetItems().Num(); ++Index)
				{
					const FBlueprintUmgGraphItemIdentity& Item =
						WidgetTree.GetItems()[Index];
					UWidget* Widget = WidgetsByKey.FindRef(Item.GetItemKey());
					const FString WidgetName = Item.GetItemKey().Mid(7);
					const FString WidgetClass = Widget
						? Widget->GetClass()->GetPathName()
						: TEXT("widget");
					TMap<FString, FString> Selectors = AssetSelectors;
					Selectors.Add(TEXT("widget_name"), WidgetName);
					Selectors.Add(TEXT("widget_class"), WidgetClass);
					if (!Item.GetNativeGuid().IsEmpty())
					{
						Selectors.Add(TEXT("widget_guid"), Item.GetNativeGuid());
					}
					TSharedRef<FJsonObject> WidgetState = MakeShared<FJsonObject>();
					WidgetState->SetStringField(TEXT("name"), WidgetName);
					WidgetState->SetStringField(TEXT("class"), WidgetClass);
					FUeAgentCanonicalValuePtr WidgetValue;
					if (!TryCanonicalObject(
						WidgetState,
						Resources,
						WidgetValue,
						OutError))
					{
						return false;
					}
					FUeAgentContentGraphItem CapturedWidget;
					CapturedWidget.ItemId = Item.GetItemKey();
					CapturedWidget.ItemKind = TEXT("widget");
					CapturedWidget.OwnerId = TreeId;
					CapturedWidget.ClassPath = WidgetClass;
					CapturedWidget.ContainerId = TreeId;
					CapturedWidget.Order = Index;
					CapturedWidget.SemanticIdentity = SemanticIdentity(
						TEXT("uai.semantic.widget_blueprint.widget"),
						Selectors,
						TEXT("item"));
					CapturedWidget.State = WidgetValue;
					OutCapture.Items.Add(MoveTemp(CapturedWidget));
					RevisionFacts.Add(Item.GetItemKey() + TEXT("|") + WidgetClass);
					if (Widget
						&& (!AddMappedProperty(
							*Widget,
							TEXT("Visibility"),
							TEXT("visibility"),
							Item.GetItemKey(),
							TEXT("uai.semantic.widget_blueprint.widget.property"),
							Selectors,
							Resources,
							OutCapture.Properties,
							RevisionFacts,
							OutError)
							|| !AddMappedProperty(
								*Widget,
								TEXT("bIsEnabled"),
								TEXT("is_enabled"),
								Item.GetItemKey(),
								TEXT("uai.semantic.widget_blueprint.widget.property"),
								Selectors,
								Resources,
								OutCapture.Properties,
								RevisionFacts,
								OutError)
							|| !AddMappedProperty(
								*Widget,
								TEXT("RenderOpacity"),
								TEXT("render_opacity"),
								Item.GetItemKey(),
								TEXT("uai.semantic.widget_blueprint.widget.property"),
								Selectors,
								Resources,
								OutCapture.Properties,
								RevisionFacts,
								OutError)))
					{
						return false;
					}
				}
				for (const FBlueprintUmgGraphHierarchyRelation& Relation
					: WidgetTree.GetHierarchy())
				{
					TSharedRef<FJsonObject> RelationState = MakeShared<FJsonObject>();
					RelationState->SetStringField(
						TEXT("child"),
						Relation.GetItemKey());
					RelationState->SetStringField(
						TEXT("parent"),
						Relation.GetParentKey());
					RelationState->SetStringField(
						TEXT("ordering_key"),
						Relation.GetOrderingKey());
					FUeAgentCanonicalValuePtr RelationValue;
					if (!TryCanonicalObject(
						RelationState,
						Resources,
						RelationValue,
						OutError))
					{
						return false;
					}
					TMap<FString, FString> Selectors = AssetSelectors;
					Selectors.Add(TEXT("slot_key"), Relation.GetRelationKey());
					FUeAgentContentGraphRelation CapturedRelation;
					CapturedRelation.RelationId = Relation.GetRelationKey();
					CapturedRelation.RelationKind = TEXT("widget_slot");
					CapturedRelation.SourceItemId = Relation.GetItemKey();
					CapturedRelation.TargetItemId = Relation.GetParentKey();
					CapturedRelation.SemanticIdentity = SemanticIdentity(
						TEXT("uai.semantic.widget_blueprint.slot"),
						MoveTemp(Selectors),
						TEXT("relation"));
					CapturedRelation.State = RelationValue;
					OutCapture.Relations.Add(MoveTemp(CapturedRelation));
					RevisionFacts.Add(Relation.GetRelationKey());
				}
				RevisionFacts.Add(WidgetTreeRevision);
			}

			OutCapture.Revision = CaptureRevision(Blueprint, MoveTemp(RevisionFacts));
			return true;
		}

		bool CaptureNiagara(
			UObject& Asset,
			FUeAgentContentGraphCapture& OutCapture,
			FUeAgentContentGraphReadError& OutError)
		{
			UNiagaraSystem* System = Cast<UNiagaraSystem>(&Asset);
			UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(&Asset);
			UNiagaraScript* Script = Cast<UNiagaraScript>(&Asset);
			if (!System && !Emitter && !Script)
			{
				return Fail(
					OutError,
					TEXT("content_graph_read_niagara_target_invalid"),
					Asset.GetPathName(),
					Asset.GetClass()->GetPathName());
			}
			const FString OwnerId = Asset.GetPathName();
			const TMap<FString, FString> AssetSelectors = {
				{TEXT("asset_path"), OwnerId},
			};
			FUeAgentResourceTracker Resources = MakeCanonicalResources();
			TArray<FString> RevisionFacts;
			const FString AssetKind = System
				? TEXT("niagara_system")
				: Emitter
					? TEXT("niagara_emitter")
					: TEXT("niagara_script");
			const FString TargetSemanticId = System
				? TEXT("uai.semantic.niagara.system")
				: Emitter
					? TEXT("uai.semantic.niagara.emitter")
					: TEXT("uai.semantic.niagara.script");

			TSharedRef<FJsonObject> TargetState = MakeShared<FJsonObject>();
			TargetState->SetStringField(TEXT("asset_path"), OwnerId);
			TargetState->SetStringField(TEXT("asset_kind"), AssetKind);
			FUeAgentCanonicalValuePtr TargetValue;
			if (!TryCanonicalObject(
				TargetState,
				Resources,
				TargetValue,
				OutError))
			{
				return false;
			}
			FUeAgentContentGraphContainer Target;
			Target.ContainerId = TEXT("asset:") + OwnerId;
			Target.ContainerKind = TEXT("niagara_asset");
			Target.OwnerId = OwnerId;
			Target.SemanticIdentity = SemanticIdentity(
				TargetSemanticId,
				AssetSelectors,
				TEXT("target"));
			Target.State = TargetValue;
			OutCapture.Containers.Add(MoveTemp(Target));

			FUeAgentContentGraphFieldState AssetFieldState;
			FUeAgentContentGraphProperty AssetField;
			TMap<FString, FString> AssetFieldSelectors = AssetSelectors;
			if (System)
			{
				const double WarmupTime = System->GetWarmupTime();
				if (!TryMakeManualFieldState(
					MakeShared<FJsonValueNumber>(WarmupTime),
					MakeShared<FJsonValueNumber>(0.0),
					TEXT("niagara.system.warmup_time"),
					!FMath::IsNearlyZero(WarmupTime),
					Resources,
					AssetFieldState,
					OutError))
				{
					return false;
				}
				AssetField.FieldId = TEXT("warmup_time");
				AssetFieldSelectors.Add(TEXT("property_path"), TEXT("warmup_time"));
				AssetFieldSelectors.Add(
					TEXT("native_property_path"),
					TEXT("WarmupTime"));
				AssetField.SemanticIdentity = SemanticIdentity(
					TEXT("uai.semantic.niagara.system.property"),
					MoveTemp(AssetFieldSelectors),
					TEXT("property"));
				RevisionFacts.Add(
					FString::Printf(TEXT("warmup|%.17g"), WarmupTime));
			}
			else if (Emitter)
			{
				const FVersionedNiagaraEmitterData* Data =
					Emitter->GetLatestEmitterData();
				if (!Data)
				{
					return Fail(
						OutError,
						TEXT("content_graph_read_niagara_emitter_data_missing"),
						OwnerId);
				}
				if (!TryMakeManualFieldState(
					MakeShared<FJsonValueBoolean>(Data->bLocalSpace),
					MakeShared<FJsonValueBoolean>(false),
					TEXT("niagara.emitter.local_space"),
					Data->bLocalSpace,
					Resources,
					AssetFieldState,
					OutError))
				{
					return false;
				}
				AssetField.FieldId = TEXT("local_space");
				AssetFieldSelectors.Add(TEXT("property_path"), TEXT("local_space"));
				AssetFieldSelectors.Add(
					TEXT("native_property_path"),
					TEXT("bLocalSpace"));
				AssetField.SemanticIdentity = SemanticIdentity(
					TEXT("uai.semantic.niagara.emitter.asset.property"),
					MoveTemp(AssetFieldSelectors),
					TEXT("property"));
				RevisionFacts.Add(
					FString::Printf(
						TEXT("local_space|%d"),
						Data->bLocalSpace ? 1 : 0));
			}
			else
			{
				const FVersionedNiagaraScriptData* Data =
					Script->GetLatestScriptData();
				if (!Data)
				{
					return Fail(
						OutError,
						TEXT("content_graph_read_niagara_script_data_missing"),
						OwnerId);
				}
				const FString Description = Data->Description.ToString();
				if (!TryMakeManualFieldState(
					MakeShared<FJsonValueString>(Description),
					MakeShared<FJsonValueString>(FString()),
					TEXT("niagara.script.description"),
					!Description.IsEmpty(),
					Resources,
					AssetFieldState,
					OutError))
				{
					return false;
				}
				AssetField.FieldId = TEXT("description");
				AssetFieldSelectors.Add(TEXT("property_path"), TEXT("description"));
				AssetFieldSelectors.Add(
					TEXT("native_property_path"),
					TEXT("Description"));
				AssetField.SemanticIdentity = SemanticIdentity(
					TEXT("uai.semantic.niagara.script.asset.property"),
					MoveTemp(AssetFieldSelectors),
					TEXT("property"));
				RevisionFacts.Add(TEXT("description|") + Description);
			}
			AssetField.OwnerId = OwnerId;
			AssetField.State = MoveTemp(AssetFieldState);
			OutCapture.Properties.Add(MoveTemp(AssetField));

			TArray<FResolvedEmitterOwner> EmitterOwners;
			FString NativeError;
			if (!Script
				&& !EnumerateEmitterOwners(Asset, EmitterOwners, NativeError))
			{
				return Fail(
					OutError,
					TEXT("content_graph_read_niagara_emitter_capture_failed"),
					OwnerId,
					NativeError);
			}
			for (int32 Index = 0; Index < EmitterOwners.Num(); ++Index)
			{
				const FResolvedEmitterOwner& Owner = EmitterOwners[Index];
				const FString EmitterHandleId = Owner.EmitterHandleId.IsValid()
					? GuidText(Owner.EmitterHandleId)
					: TEXT("standalone");
				const FString EmitterId = TEXT("emitter:") + EmitterHandleId;
				TMap<FString, FString> Selectors = AssetSelectors;
				Selectors.Add(TEXT("emitter_owner_id"), EmitterHandleId);
				if (Owner.EmitterHandleId.IsValid())
				{
					Selectors.Add(TEXT("emitter_handle_id"), EmitterHandleId);
				}
				if (!Owner.EmitterName.IsEmpty())
				{
					Selectors.Add(TEXT("emitter_name"), Owner.EmitterName);
				}
				TSharedRef<FJsonObject> EmitterState = MakeShared<FJsonObject>();
				EmitterState->SetStringField(
					TEXT("emitter_owner_id"),
					EmitterHandleId);
				EmitterState->SetStringField(
					TEXT("emitter_name"),
					Owner.EmitterName);
				if (Owner.EmitterHandleId.IsValid())
				{
					EmitterState->SetStringField(
						TEXT("emitter_handle_id"),
						EmitterHandleId);
				}
				FUeAgentCanonicalValuePtr EmitterValue;
				if (!TryCanonicalObject(
					EmitterState,
					Resources,
					EmitterValue,
					OutError))
				{
					return false;
				}
				FUeAgentContentGraphItem CapturedEmitter;
				CapturedEmitter.ItemId = EmitterId;
				CapturedEmitter.ItemKind = TEXT("niagara_emitter");
				CapturedEmitter.OwnerId = OwnerId;
				CapturedEmitter.ClassPath = Owner.Emitter
					? Owner.Emitter->GetClass()->GetPathName()
					: TEXT("niagara_emitter");
				CapturedEmitter.ContainerId = TEXT("asset:") + OwnerId;
				CapturedEmitter.Order = Index;
				CapturedEmitter.SemanticIdentity = SemanticIdentity(
					TEXT("uai.semantic.niagara.emitter_owner"),
					MoveTemp(Selectors),
					TEXT("item"));
				CapturedEmitter.State = EmitterValue;
				OutCapture.Items.Add(MoveTemp(CapturedEmitter));
				RevisionFacts.Add(EmitterId + TEXT("|") + Owner.EmitterName);
			}

			TArray<FResolvedStage> Stages;
			if (!EnumerateStages(Asset, Stages, NativeError))
			{
				return Fail(
					OutError,
					TEXT("content_graph_read_niagara_stage_capture_failed"),
					OwnerId,
					NativeError);
			}
			FMaterialNiagaraGraphNativeHook Hook;
			for (int32 StageIndex = 0; StageIndex < Stages.Num(); ++StageIndex)
			{
				const FResolvedStage& Stage = Stages[StageIndex];
				const FString EmitterHandleId = Stage.EmitterHandleId.IsValid()
					? GuidText(Stage.EmitterHandleId)
					: TEXT("system");
				const FString UsageId = Stage.UsageId.IsValid()
					? GuidText(Stage.UsageId)
					: TEXT("none");
				const FString StageId = TEXT("stage:")
					+ EmitterHandleId + TEXT(":") + Stage.StageKey;
				TMap<FString, FString> StageSelectors = AssetSelectors;
				StageSelectors.Add(TEXT("stage_owner_id"), EmitterHandleId);
				StageSelectors.Add(TEXT("usage"), ScriptUsageToString(Stage.Usage));
				StageSelectors.Add(TEXT("usage_id"), UsageId);
				if (Stage.EmitterHandleId.IsValid())
				{
					StageSelectors.Add(TEXT("emitter_handle_id"), EmitterHandleId);
				}
				if (!Stage.EmitterName.IsEmpty())
				{
					StageSelectors.Add(TEXT("emitter_name"), Stage.EmitterName);
				}

				FMaterialNiagaraGraphTopology Topology;
				FString TopologyRevision;
				FString Evidence;
				FMaterialNiagaraGraphAdapterError HookError;
				if (!Hook.TryCaptureNiagaraStack(
					Asset,
					Stage.EmitterHandleId,
					Stage.Usage,
					Stage.UsageId,
					Topology,
					TopologyRevision,
					Evidence,
					HookError))
				{
					return Fail(
						OutError,
						HookError.GetCode(),
						StageId,
						HookError.GetDetail());
				}
				TSharedRef<FJsonObject> StageState = MakeShared<FJsonObject>();
				StageState->SetStringField(
					TEXT("usage"),
					ScriptUsageToString(Stage.Usage));
				StageState->SetStringField(TEXT("usage_id"), UsageId);
				if (!Stage.EmitterName.IsEmpty())
				{
					StageState->SetStringField(
						TEXT("emitter_name"),
						Stage.EmitterName);
				}
				if (Stage.EmitterHandleId.IsValid())
				{
					StageState->SetStringField(
						TEXT("emitter_handle_id"),
						EmitterHandleId);
				}
				FUeAgentCanonicalValuePtr StageValue;
				if (!TryCanonicalObject(
					StageState,
					Resources,
					StageValue,
					OutError))
				{
					return false;
				}
				FUeAgentContentGraphContainer CapturedStage;
				CapturedStage.ContainerId = StageId;
				CapturedStage.ContainerKind = TEXT("niagara_stack");
				CapturedStage.OwnerId = OwnerId;
				CapturedStage.ParentContainerId = TEXT("asset:") + OwnerId;
				CapturedStage.Order = StageIndex;
				CapturedStage.SemanticIdentity = SemanticIdentity(
					TEXT("uai.semantic.niagara.stack_entry"),
					StageSelectors,
					TEXT("structure"));
				CapturedStage.State = StageValue;
				OutCapture.Containers.Add(MoveTemp(CapturedStage));
				RevisionFacts.Add(TopologyRevision);

				TArray<UNiagaraNodeFunctionCall*> NativeModules;
				if (Stage.OutputNode)
				{
					GetModules(*Stage.OutputNode, NativeModules);
				}
				TMap<FString, UNiagaraNodeFunctionCall*> ModulesByGuid;
				for (UNiagaraNodeFunctionCall* Module : NativeModules)
				{
					if (Module && Module->NodeGuid.IsValid())
					{
						ModulesByGuid.Add(GuidText(Module->NodeGuid), Module);
					}
				}
				for (int32 ModuleIndex = 0;
					ModuleIndex < Topology.GetItems().Num();
					++ModuleIndex)
				{
					const FMaterialNiagaraGraphItemIdentity& Item =
						Topology.GetItems()[ModuleIndex];
					UNiagaraNodeFunctionCall* Module =
						ModulesByGuid.FindRef(Item.GetNativeGuid());
					if (!Module)
					{
						return Fail(
							OutError,
							TEXT("content_graph_read_niagara_module_missing"),
							Item.GetItemKey(),
							StageId);
					}
					TMap<FString, FString> ModuleSelectors = StageSelectors;
					ModuleSelectors.Add(TEXT("module_guid"), Item.GetNativeGuid());
					const FString ScriptPath = GetModuleScriptPath(*Module);
					if (!ScriptPath.IsEmpty())
					{
						ModuleSelectors.Add(TEXT("script_asset_path"), ScriptPath);
					}
					TSharedRef<FJsonObject> ModuleState = MakeShared<FJsonObject>();
					ModuleState->SetNumberField(TEXT("module_index"), ModuleIndex);
					if (Cast<UNiagaraNodeAssignment>(Module))
					{
						ModuleState->SetStringField(
							TEXT("module_kind"),
							TEXT("assignment"));
						TSharedRef<FJsonObject> AssignmentTargets =
							MakeShared<FJsonObject>();
						if (!BuildAssignmentTargets(
							*Module,
							AssignmentTargets,
							NativeError))
						{
							return Fail(
								OutError,
								TEXT("content_graph_read_niagara_assignment_capture_failed"),
								Item.GetItemKey(),
								NativeError);
						}
						ModuleState->SetObjectField(
							TEXT("assignment_targets"),
							AssignmentTargets);
						FUeAgentContentGraphFieldState AssignmentState;
						if (!TryMakeManualFieldState(
							MakeShared<FJsonValueObject>(AssignmentTargets),
							MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()),
							TEXT("niagara.module.assignment_targets"),
							true,
							Resources,
							AssignmentState,
							OutError))
						{
							return false;
						}
						TMap<FString, FString> PropertySelectors = ModuleSelectors;
						PropertySelectors.Add(
							TEXT("property_path"),
							TEXT("assignment_targets"));
						FUeAgentContentGraphProperty Assignment;
						Assignment.OwnerId = Item.GetItemKey();
						Assignment.FieldId = TEXT("assignment_targets");
						Assignment.State = MoveTemp(AssignmentState);
						Assignment.SemanticIdentity = SemanticIdentity(
							TEXT("uai.semantic.niagara.module_property"),
							MoveTemp(PropertySelectors),
							TEXT("property"));
						OutCapture.Properties.Add(MoveTemp(Assignment));
					}
					else
					{
						ModuleState->SetStringField(
							TEXT("script_asset_path"),
							ScriptPath);
					}
					TSharedRef<FJsonObject> Inputs = MakeShared<FJsonObject>();
					if (!BuildInputOverrides(*Module, Inputs, NativeError))
					{
						return Fail(
							OutError,
							TEXT("content_graph_read_niagara_input_capture_failed"),
							Item.GetItemKey(),
							NativeError);
					}
					if (!Inputs->Values.IsEmpty())
					{
						ModuleState->SetObjectField(TEXT("inputs"), Inputs);
					}
					FUeAgentCanonicalValuePtr ModuleValue;
					if (!TryCanonicalObject(
						ModuleState,
						Resources,
						ModuleValue,
						OutError))
					{
						return false;
					}
					FUeAgentContentGraphItem CapturedModule;
					CapturedModule.ItemId = Item.GetItemKey();
					CapturedModule.ItemKind = TEXT("niagara_module");
					CapturedModule.OwnerId = StageId;
					CapturedModule.ClassPath = Module->GetClass()->GetPathName();
					CapturedModule.ContainerId = StageId;
					CapturedModule.Order = ModuleIndex;
					CapturedModule.SemanticIdentity = SemanticIdentity(
						TEXT("uai.semantic.niagara.module"),
						MoveTemp(ModuleSelectors),
						TEXT("item"));
					CapturedModule.State = ModuleValue;
					OutCapture.Items.Add(MoveTemp(CapturedModule));
					RevisionFacts.Add(
						StageId + TEXT("|") + Item.GetItemKey()
							+ TEXT("|") + FString::FromInt(ModuleIndex));
				}

				for (int32 OrderIndex = 0;
					OrderIndex < Topology.GetOrdering().Num();
					++OrderIndex)
				{
					const FMaterialNiagaraGraphOrderingIdentity& Ordering =
						Topology.GetOrdering()[OrderIndex];
					const FString RelationId = StageId + TEXT(":")
						+ Ordering.GetOrderingKey();
					TSharedRef<FJsonObject> OrderState = MakeShared<FJsonObject>();
					OrderState->SetStringField(
						TEXT("item_id"),
						Ordering.GetItemKey());
					OrderState->SetStringField(
						TEXT("predecessor_item_id"),
						Ordering.GetPredecessorItemKey());
					OrderState->SetNumberField(TEXT("order"), OrderIndex);
					FUeAgentCanonicalValuePtr OrderValue;
					if (!TryCanonicalObject(
						OrderState,
						Resources,
						OrderValue,
						OutError))
					{
						return false;
					}
					TMap<FString, FString> OrderSelectors = StageSelectors;
					OrderSelectors.Add(TEXT("ordering_key"), RelationId);
					FUeAgentContentGraphRelation CapturedOrder;
					CapturedOrder.RelationId = RelationId;
					CapturedOrder.RelationKind = TEXT("niagara_order");
					CapturedOrder.SourceItemId =
						Ordering.GetPredecessorItemKey().IsEmpty()
							? StageId
							: Ordering.GetPredecessorItemKey();
					CapturedOrder.TargetItemId = Ordering.GetItemKey();
					CapturedOrder.SemanticIdentity = SemanticIdentity(
						TEXT("uai.semantic.niagara.stack_order"),
						MoveTemp(OrderSelectors),
						TEXT("relation"));
					CapturedOrder.State = OrderValue;
					OutCapture.Relations.Add(MoveTemp(CapturedOrder));
				}
			}

			OutCapture.Revision = CaptureRevision(Asset, MoveTemp(RevisionFacts));
			return true;
		}
	}

	bool FUeAgentContentGraphReadProductionEnvironment::
		TryCaptureOwner(
			UObject& Owner,
			FUeAgentContentGraphCapture& OutCapture,
			FUeAgentContentGraphReadError& OutError)
	{
		OutCapture = {};
		OutError = {};
		if (!IsValid(&Owner))
		{
			return Fail(
				OutError,
				TEXT("content_graph_read_owner_invalid"));
		}
		const FString ResolvedDomain = DomainId(Owner);
		if (ResolvedDomain.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("content_graph_read_domain_unsupported"),
				Owner.GetPathName(),
				Owner.GetClass()->GetPathName());
		}
		OutCapture.Owner = &Owner;
		OutCapture.DomainId = ResolvedDomain;
		if (ResolvedDomain == TEXT("blueprint"))
		{
			return CaptureBlueprint(
				*CastChecked<UBlueprint>(&Owner),
				OutCapture,
				OutError);
		}
		if (ResolvedDomain == TEXT("niagara"))
		{
			return CaptureNiagara(
				Owner,
				OutCapture,
				OutError);
		}

		const FString OwnerId = Owner.GetPathName();
		const FString RootContainerId =
			TEXT("content_graph.container:") + OwnerId;
		OutCapture.Containers.Add({
			RootContainerId,
			TEXT("root"),
			OwnerId,
			FString(),
			0,
		});

		TArray<UObject*> Objects;
		GetObjectsWithOuter(&Owner, Objects, true);
		Objects.RemoveAll(
			[](const UObject* Object)
			{
				return !IsValid(Object)
					|| Object->HasAnyFlags(
						RF_Transient | RF_ClassDefaultObject);
			});
		Objects.Sort(
			[](const UObject& Left, const UObject& Right)
			{
				return Left.GetPathName() < Right.GetPathName();
			});

		TSet<FString> ItemIds;
		for (int32 Index = 0; Index < Objects.Num(); ++Index)
		{
			UObject* Object = Objects[Index];
			const FString ItemId = Object->GetPathName();
			ItemIds.Add(ItemId);
			OutCapture.Items.Add({
				ItemId,
				Object->GetClass()->GetName(),
				OwnerId,
				Object->GetClass()->GetPathName(),
				RootContainerId,
				Index,
			});
			OutCapture.Relations.Add({
				FString::Printf(
					TEXT("content_graph.relation:%04d"),
					Index),
				TEXT("membership"),
				RootContainerId,
				ItemId,
			});
		}

		FUeAgentResourceTracker Resources =
			MakeCanonicalResources();
		TArray<FString> GenericRevisionFacts;
		TArray<FString>* GenericRevisionFactsPtr =
			ResolvedDomain == TEXT("world_hlod")
				? &GenericRevisionFacts : nullptr;
		int32 PropertyCount = 0;
		if (!AddProperties(
				Owner,
				Resources,
				OutCapture.Properties,
				PropertyCount,
				OutError,
				GenericRevisionFactsPtr))
		{
			return false;
		}
		for (UObject* Object : Objects)
		{
			if (!AddProperties(
					*Object,
					Resources,
					OutCapture.Properties,
					PropertyCount,
					OutError,
					GenericRevisionFactsPtr))
			{
				return false;
			}
		}
		FString NativeMaterialRevision;
		if (ResolvedDomain == TEXT("material")
			&& !AugmentMaterialCapture(
				Owner,
				RootContainerId,
				Resources,
				OutCapture,
				NativeMaterialRevision,
				OutError))
		{
			return false;
		}
		FString NativeAiBehaviorRevision;
		if (ResolvedDomain == TEXT("ai_behavior")
			&& !AugmentAiBehaviorCapture(
				Owner,
				RootContainerId,
				OutCapture,
				NativeAiBehaviorRevision,
				OutError))
		{
			return false;
		}
		FString NativeAudioRevision;
		if (ResolvedDomain == TEXT("audio")
			&& !AugmentAudioCapture(
				Owner,
				RootContainerId,
				OutCapture,
				NativeAudioRevision,
				OutError))
		{
			return false;
		}
		FString NativeAnimationRigRevision;
		FString NativeTextureGraphRevision;
		if (ResolvedDomain == TEXT("texture_graph")
			&& !AugmentTextureGraphCapture(
				Owner,
				RootContainerId,
				Resources,
				OutCapture,
				NativeTextureGraphRevision,
				OutError))
		{
			return false;
		}
		if (ResolvedDomain == TEXT("animation_rig_graph")
			&& !AugmentAnimationRigCapture(
				Owner,
				RootContainerId,
				Resources,
				OutCapture,
				NativeAnimationRigRevision,
				OutError))
		{
			return false;
		}
		if (ResolvedDomain == TEXT("world_hlod"))
		{
			for (const FUeAgentContentGraphItem& Item : OutCapture.Items)
			{
				GenericRevisionFacts.Add(
					TEXT("item|") + Item.ItemId + TEXT("|") + Item.ClassPath);
			}
			OutCapture.Revision = CaptureRevision(
				Owner, MoveTemp(GenericRevisionFacts));
			return true;
		}
		OutCapture.Revision = FString::Printf(
			TEXT("%s:%s:%d:%d:%d%s%s%s%s%s%s%s%s%s%s"),
			*Owner.GetOutermost()->GetPersistentGuid().ToString(
				EGuidFormats::Digits),
			Owner.GetOutermost()->IsDirty()
				? TEXT("dirty")
				: TEXT("clean"),
			OutCapture.Containers.Num(),
			OutCapture.Items.Num(),
			PropertyCount,
			NativeMaterialRevision.IsEmpty() ? TEXT("") : TEXT(":"),
			*NativeMaterialRevision,
			NativeAiBehaviorRevision.IsEmpty() ? TEXT("") : TEXT(":"),
			*NativeAiBehaviorRevision,
			NativeAudioRevision.IsEmpty() ? TEXT("") : TEXT(":"),
			*NativeAudioRevision,
			NativeTextureGraphRevision.IsEmpty() ? TEXT("") : TEXT(":"),
			*NativeTextureGraphRevision,
			NativeAnimationRigRevision.IsEmpty() ? TEXT("") : TEXT(":"),
			*NativeAnimationRigRevision);
		return true;
	}

	bool FUeAgentContentGraphReadProductionEnvironment::TryCapture(
		const UE::UAI::Discovery::FUeAgentCandidateBinding& Target,
		const UE::UAI::ContextResource::FUeAgentExecutionContextRef&
			Context,
		FUeAgentContentGraphCapture& OutCapture,
		FUeAgentContentGraphReadError& OutError)
	{
		UObject* Owner = ResolveObject(Target.GetPublicRef());
		if (!Owner)
		{
			return Fail(
				OutError,
				TEXT("content_graph_read_target_unresolved"),
				Target.GetPublicRef(),
				Context.GetProjectInstanceId());
		}
		return TryCaptureOwner(
			*Owner,
			OutCapture,
			OutError);
	}
}
