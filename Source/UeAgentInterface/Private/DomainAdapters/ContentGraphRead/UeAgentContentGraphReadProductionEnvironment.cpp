#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/EditorProject/UeAgentSettingsAuthoring.h"
#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphTextureGraphNativeHook.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringAssetIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringReflectionDescriptors.h"
#include "Domains/Material/Authoring/UeAgentMaterialInstanceAuthoring.h"
#include "Domains/Material/Authoring/UeAgentMaterialFunctionInstanceAuthoring.h"
#include "Materials/MaterialFunctionInstance.h"
#include "DomainAdapters/UObjectDataAsset/UeAgentRecursiveDataAsset.h"

#include "Domains/BehaviorTree/Authoring/UeAgentBehaviorTreeNativeGraph.h"
#include "Domains/BehaviorTree/Authoring/UeAgentBehaviorTreeAuthoringState.h"
#include "Domains/Eqs/Authoring/UeAgentEqsNativeGraph.h"
#include "Domains/Eqs/Authoring/UeAgentEqsAuthoringState.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeAuthoringState.h"
#include "Domains/Blackboard/Authoring/UeAgentBlackboardAuthoringState.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringDocuments.h"
#include "Domains/SmartObject/Authoring/UeAgentSmartObjectAuthoringState.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringDescriptors.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringExport.h"
#include "Domains/Level/Authoring/UeAgentLevelPartitionReadScope.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringRegistration.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "EngineUtils.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringRegistration.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringState.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundSubmix.h"
#include "Sound/SoundMix.h"
#include "SubmixEffects/AudioMixerSubmixEffectEQ.h"
#include "MediaTexture.h"
#include "SparseVolumeTexture/SparseVolumeTexture.h"
#include "PaperFlipbook.h"
#include "PaperSprite.h"
#include "PaperSpriteAtlas.h"
#include "PaperTileSet.h"
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
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/HjsonWorkspace/UeAgentSemanticPublicReferences.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphStructureCapture.h"
#include "DomainAdapters/DataTableCurveTable/UeAgentDataTableAuthoringState.h"
#include "DomainAdapters/BlueprintUmgGraph/UeAgentBlueprintUmgGraphNativeHook.h"
#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphNativeHook.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCarrierRevision.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringIdentity.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringExport.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringDescriptors.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraStageModuleAuthoring.h"
#include "Domains/PCG/Authoring/UeAgentPCGAuthoringState.h"
#include "Domains/PCG/UeAgentPCGDomainBackend.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "GameFramework/Actor.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "Expressions/TG_Expression.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringState.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringRegistration.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundAssetKey.h"
#include "MetasoundAssetManager.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundSource.h"
#include "NiagaraEmitter.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "Internationalization/Text.h"
#include "Retargeter/IKRetargeter.h"
#include "ReferenceSkeleton.h"
#include "Rig/IKRigDefinition.h"
#include "Rigs/RigHierarchy.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundConcurrency.h"
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
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "WidgetBlueprint.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Engine/SpecularProfile.h"
#include "MediaPlayer.h"
#include "FileMediaSource.h"
#include "Utilities/UeAgentCurveJson.h"

namespace UE::UAI::DomainAdapters::ContentGraphRead
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::DomainAdapters::AnimationRigGraph;
	using namespace UE::UAI::DomainAdapters::BlueprintUmgGraph;
	using namespace UE::UAI::DomainAdapters::MaterialNiagaraGraph;
	using namespace UE::UAI::HjsonWorkspace;
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

		FUeAgentContentGraphReadError CaptureFact(const FString& Code, const FString& Reason,
			const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& SubjectId = FString())
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("content_capture"), FString(), SubjectId);
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.PropertyPath = Field;
			Diagnostic.ActualValue = MoveTemp(Actual);
			if (!Diagnostic.ActualValue.IsValid()) Diagnostic.ActualValue = MakeShared<FJsonValueString>(TEXT("<missing>"));
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			return Diagnostic;
		}

		void SupplementCaptureOwner(FUeAgentContentGraphReadError& Diagnostic, const FString& Owner, const FString& Field = FString(), const FString& Name = FString())
		{
			if (Diagnostic.IsEmpty()) return;
			if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Owner;
			if (Diagnostic.PropertyPath.IsEmpty()) Diagnostic.PropertyPath = Field;
			if (Diagnostic.CandidateName.IsEmpty()) Diagnostic.CandidateName = Name;
			if (Diagnostic.SourceKind.IsEmpty() && Diagnostic.Source.SourceFile.IsEmpty()) Diagnostic.SourceKind = TEXT("native_result");
			for (auto& Child : Diagnostic.Diagnostics) SupplementCaptureOwner(Child, Owner, Field, Name);
			if (Diagnostic.PublicCause)
			{
				auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Diagnostic.PublicCause);
				SupplementCaptureOwner(*Cause, Owner, Field, Name);
				Diagnostic.PublicCause = MoveTemp(Cause);
			}
		}

		void PrefixCaptureValuePath(FUeAgentContentGraphReadError& Diagnostic, const FString& Token)
		{
			if (Diagnostic.IsEmpty()) return;
			const FString Prefix = TEXT("/") + Token.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
			Diagnostic.PropertyPath = Prefix + (Diagnostic.PropertyPath.StartsWith(TEXT("/")) || Diagnostic.PropertyPath.IsEmpty()
				? Diagnostic.PropertyPath : TEXT("/") + Diagnostic.PropertyPath);
			if (Diagnostic.SourceKind.IsEmpty()) Diagnostic.SourceKind = TEXT("native_result");
			for (auto& Child : Diagnostic.Diagnostics) PrefixCaptureValuePath(Child, Token);
			if (Diagnostic.PublicCause)
			{
				auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Diagnostic.PublicCause);
				PrefixCaptureValuePath(*Cause, Token);
				Diagnostic.PublicCause = MoveTemp(Cause);
			}
		}

		bool Fail(FUeAgentContentGraphReadError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& SubjectId = FString())
		{
			if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = SubjectId;
			if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = TEXT("content_capture");
			OutError = MoveTemp(Diagnostic);
			return false;
		}

		FString DomainId(const UObject& Owner)
		{
			if (Owner.IsA<UWorld>()) return UeAgentLevelAuthoring::DomainId;
			if (Owner.GetClass()->GetPathName() == TEXT("/Script/PCG.PCGDataAsset")) return TEXT("pcg_data_asset");
			if (Owner.GetClass()->GetPathName() == TEXT("/Script/Engine.MeshDeformerCollection")) return TEXT("mesh_deformer_collection");
			if (Owner.GetClass()->GetPathName() == TEXT("/Script/OptimusCore.OptimusSource")) return TEXT("deformer_source");
			if (Owner.GetClass()->GetPathName() == TEXT("/Script/MLDeformerFramework.MLDeformerAsset")) return TEXT("ml_deformer");
			if (Owner.GetClass()->GetPathName() == TEXT("/Script/Engine.MaterialParameterCollection")) return TEXT("material_parameter_collection");
			if (Owner.GetClass()->GetPathName() == TEXT("/Script/Engine.PhysicsAsset")) return TEXT("physics_asset");
			if (Owner.GetClass()->GetPathName() == TEXT("/Script/Engine.StaticMesh")) return TEXT("static_mesh");
			if (Owner.GetClass()->GetPathName() == TEXT("/Script/Engine.SkeletalMesh")) return TEXT("skeletal_mesh");
			if (Owner.GetClass()->GetPathName() == TEXT("/Script/Engine.TextureRenderTarget2D")) return TEXT("render_target");
			if (Owner.GetClass()->GetPathName() == TEXT("/Script/Engine.RuntimeVirtualTexture")) return TEXT("runtime_virtual_texture");
			if (Owner.IsA<UStreamableSparseVolumeTexture>()) return TEXT("sparse_volume_texture");
			if (Owner.GetClass()->GetPathName() == TEXT("/Script/Engine.SubUVAnimation")) return TEXT("subuv_animation");
			if (Owner.GetClass()->GetPathName() == TEXT("/Script/Engine.TextureCollection")) return TEXT("texture_collection");
			if (Owner.GetClass()->GetPathName() == TEXT("/Script/Engine.Texture2D")) return TEXT("texture_2d");
			if (Owner.GetClass()->GetPathName() == TEXT("/Script/Engine.SoundWave")) return TEXT("sound_wave");
			if (Owner.GetClass()->GetPathName() == TEXT("/Script/Engine.Texture2DArray")) return TEXT("texture_array");
			if (Owner.GetClass()->GetPathName() == TEXT("/Script/Engine.TextureCubeArray")) return TEXT("texture_cube_array");
			if (Owner.GetClass()->GetPathName() == TEXT("/Script/Engine.TextureCube")) return TEXT("texture_cube");
			if (Owner.GetClass()->GetPathName() == TEXT("/Script/Engine.VolumeTexture")) return TEXT("volume_texture");
			if (Owner.GetClass()->GetPathName() == TEXT("/Script/PhysicsCore.PhysicalMaterial")) return TEXT("physical_material");
			if (Owner.IsA<UUeAgentRecursiveDataAsset>()) return TEXT("recursive_dataasset");
			if (Owner.IsA<UInputAction>()) return TEXT("input_action");
			if (Owner.IsA<UInputMappingContext>()) return TEXT("input_mapping_context");
			if (Owner.IsA<USpecularProfile>()) return TEXT("specular_profile");
			if (Owner.IsA<UMediaPlayer>()) return TEXT("media_player");
			if (Owner.IsA<UFileMediaSource>()) return TEXT("media_source");
			if (Owner.IsA<UMediaTexture>()) return TEXT("media_texture");
			if (Owner.IsA<UPaperFlipbook>()) return TEXT("paper_flipbook");
			if (Owner.IsA<UPaperSprite>()) return TEXT("paper_sprite");
			if (Owner.IsA<UPaperSpriteAtlas>()) return TEXT("paper_sprite_atlas");
			if (Owner.IsA<UPaperTileSet>()) return TEXT("paper_tileset");
			if (Owner.IsA<USoundAttenuation>()) return TEXT("sound_attenuation");
			if (Owner.IsA<USoundConcurrency>()) return TEXT("sound_concurrency");
			if (Owner.IsA<USoundClass>()) return TEXT("sound_class");
			if (Owner.IsA<USoundSubmix>()) return TEXT("sound_submix");
			if (Owner.IsA<USoundMix>()) return TEXT("sound_mix");
			if (Owner.IsA<USubmixEffectSubmixEQPreset>()) return TEXT("audio_effect_preset");
			if (const auto* Curve = UeAgentCurveJson::FindAssetCurveContract(&Owner)) return TEXT("curve_") + Curve->CurveKind;
			if (Owner.IsA<UDataTable>())
			{
				return TEXT("data_table");
			}
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
			if (Owner.IsA<UStaticMesh>())
			{
				return TEXT("modeling_geometry");
			}
			if (DomainAdapters::EditorProject::IsRegisteredSettingsObject(Owner)) return TEXT("project_settings");
			const auto* DataObjectProfile = UeAgentReflectionAuthoring::FindProfileByTypeId(TEXT("data_object"));
			if (DataObjectProfile && DataObjectProfile->AcceptsConcreteClass
				&& DataObjectProfile->AcceptsConcreteClass(Owner.GetClass())) return TEXT("data_object");
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
			if (!ObjectPath.Contains(TEXT(".")))
				ObjectPath = UeAgentAuthoringAssetIdentity::ToObjectPath(ObjectPath);
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
			return FUeAgentResourceTracker();
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
			{
				FString Lexeme;
				if (!StateSemantic::CanonicalJson::TryGetNumberLexeme(*JsonValue, Lexeme))
				{ return Fail(OutError, CaptureFact(TEXT("canonical_json_number_lexeme_invalid"), TEXT("Captured JSON number has no valid finite numeric lexeme."), TEXT("/"), MakeShared<FJsonValueString>(TEXT("invalid numeric lexeme")), MakeShared<FJsonValueString>(TEXT("finite JSON number")))); }
				return FUeAgentCanonicalValue::TryDecimal(Lexeme, Resources, OutValue, OutError);
			}
			case EJson::Array:
				{
					TArray<FUeAgentCanonicalValueRef> Items;
					const auto& JsonItems = JsonValue->AsArray();
					for (int32 Index = 0; Index < JsonItems.Num(); ++Index)
					{
						FUeAgentCanonicalValuePtr Canonical;
						if (!TryCanonicalValue(
							JsonItems[Index],
							Resources,
							Canonical,
							OutError))
						{
							PrefixCaptureValuePath(OutError, FString::FromInt(Index));
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
						return Fail(OutError, CaptureFact(TEXT("content_graph_read_json_object_missing"), TEXT("Captured JSON value declares an object but has no object payload."), TEXT("/"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
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
							PrefixCaptureValuePath(OutError, Key);
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
				return Fail(OutError, CaptureFact(TEXT("content_graph_read_json_kind_unsupported"), TEXT("Captured JSON value has an unsupported type."), TEXT("/"), MakeShared<FJsonValueNumber>(static_cast<int32>(JsonValue->Type)), MakeShared<FJsonValueString>(TEXT("null, boolean, string, number, array or object"))));
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

		void CollectProperties(UObject& Object, TArray<FProperty*>& Properties, const TSet<FName>* Excluded = nullptr)
		{
			const auto* ReflectionProfile = UeAgentReflectionAuthoring::FindProfileByTypeId(DomainId(Object));
			for (TFieldIterator<FProperty> It(
				Object.GetClass(),
				EFieldIterationFlags::IncludeSuper);
				It;
				++It)
			{
				FProperty* Property = *It;
				if (!Property
					|| (Excluded && Excluded->Contains(Property->GetFName()))
					|| (ReflectionProfile && ReflectionProfile->IncludeProperty && !ReflectionProfile->IncludeProperty(Property))
					|| (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)
						&& !UeAgentAuthoringReflection::IsEditableDeclaration(Property, Object.GetClass())
						&& !(ReflectionProfile && ReflectionProfile->AuthorizesProperty(Property)))
					|| Property->HasAnyPropertyFlags(
						CPF_Transient | CPF_Deprecated))
				{
					continue;
				}
				UE::UAI::Contract::FDiagnostic Unsupported;
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
		}

		bool AddProperties(
			UObject& Object, FUeAgentResourceTracker& Resources,
			TArray<FUeAgentContentGraphProperty>& OutProperties, int32& OutPropertyCount,
			FUeAgentContentGraphReadError& OutError, TArray<FString>* OutRevisionFacts = nullptr,
			const TSet<FName>* Excluded = nullptr)
		{
			UeAgentPropertyValueCodec::FOptions Options;
			Options.bCompareStoredValues = true;
			Options.bAllowPartialStructFields = false;
			TArray<FProperty*> Properties;
			CollectProperties(Object, Properties, Excluded);

			UObject* DefaultObject =
				Object.GetClass()->GetDefaultObject();
			for (FProperty* Property : Properties)
			{
				ON_SCOPE_EXIT { SupplementCaptureOwner(OutError, Object.GetPathName(), Property->GetName()); };
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
					UE::UAI::Contract::FDiagnostic JsonError;
					if (!CanonicalJson::TryToJsonValue(*Value, JsonValue, JsonError))
					{
						return Fail(OutError, JsonError, Property->GetName());
					}
					const TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
					Envelope->SetField(TEXT("value"), JsonValue);
					FString Canonical;
					if (!CommandContract::Json::TryCanonicalize(
							Envelope, Canonical, JsonError))
					{
						return Fail(OutError, JsonError, Property->GetName());
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
				return Fail(OutError, StateError);
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
				return Fail(OutError, StateError);
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
			ON_SCOPE_EXIT { SupplementCaptureOwner(OutError, Object.GetPathName(), ExternalFieldId); };
			FProperty* Property = FindFProperty<FProperty>(
				Object.GetClass(),
				NativePropertyName);
			if (!Property)
			{
				return Fail(OutError, CaptureFact(TEXT("content_graph_read_property_missing"), TEXT("The requested native property does not exist on the captured object."), NativePropertyName.ToString(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			}
			UE::UAI::Contract::FDiagnostic Unsupported;
			if (!UeAgentPropertyValueCodec::IsSupported(
				Property,
				Unsupported))
			{
				return Fail(OutError, Unsupported, Object.GetPathName());
			}

			UeAgentPropertyValueCodec::FOptions Options;
			Options.bCompareStoredValues = true;
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
				return Fail(OutError, StateError);
			}
			const bool bOverridden =
				!UeAgentPropertyValueCodec::AreSemanticallyEquivalent(
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
					return Fail(OutError, CaptureFact(TEXT("content_graph_read_animation_rig_hierarchy_missing"), TEXT("Control Rig Blueprint has no hierarchy to capture."), TEXT("/hierarchy"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
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
				return Fail(OutError, StructureError, Owner.GetPathName());
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
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!CommandContract::Json::TryCanonicalize(
						Captured.Value, Canonical, JsonError))
				{
					return Fail(OutError, JsonError, Captured.SemanticId);
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


		bool AugmentPcgCapture(
			UPCGGraph& Graph,
			FUeAgentResourceTracker& Resources,
			FUeAgentContentGraphCapture& OutCapture,
			FUeAgentContentGraphReadError& OutError)
		{
			UeAgentAuthoringState::FStateSnapshot Snapshot;
			UE::UAI::Contract::FDiagnostic CaptureError;
			if (!UeAgentPCGAuthoring::CaptureAssetExportSnapshot(Graph, Snapshot, CaptureError))
				return Fail(OutError, CaptureError, Graph.GetPathName());
			FUeAgentCanonicalSnapshotPtr Canonical;
			FAuthoringModelPtr Model;
			FWorkspaceError WorkspaceError;
			if (!TryBuildCanonicalAuthoringCapture(TEXT("ue.pcg_graph.asset"), TEXT("llm_authoring"),
					Snapshot, Resources, Canonical, Model, WorkspaceError))
				return Fail(OutError, WorkspaceError.GetDiagnostic(), Graph.GetPathName());
			const TMap<FString, UPCGNode*> NativeNodes = UeAgentPCG::BuildExportNodeMapById(&Graph);
			TSet<FString> CapturedNodes;
			for (const auto& Unit : Model->GetUnits())
			{
				if (Unit.GetUnitKind() != ESemanticUnitKind::StructureState || !Unit.GetValues().Contains(TEXT("nodes"))) continue;
				TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
				for (const auto& Field : Unit.GetValues())
				{
					TSharedPtr<FJsonValue> Value;
					if (!CanonicalJson::TryToJsonValue(*Field.Value, Value, CaptureError))
						return Fail(OutError, CaptureError, Unit.GetUnitId());
					Values->SetField(Field.Key, Value);
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetObjectField(TEXT("values"), Values);
				TSharedRef<FJsonObject> Visible = MakeShared<FJsonObject>();
				TSharedPtr<FJsonObject> Interfaces;
				if (!TryProjectGraphAuthoringVisibleRow(Unit.GetUnitId(), Row, Visible, Interfaces, WorkspaceError))
					return Fail(OutError, WorkspaceError.GetDiagnostic(), Unit.GetUnitId());
				const TSharedPtr<FJsonObject> SourceNodes = Values->GetObjectField(TEXT("nodes"));
				const TSharedPtr<FJsonObject> VisibleNodes = Visible->GetObjectField(TEXT("nodes"));
				const TSharedPtr<FJsonObject>* InterfaceNodes = nullptr;
				if (Interfaces) Interfaces->TryGetObjectField(TEXT("nodes"), InterfaceNodes);
				for (const auto& Entry : VisibleNodes->Values)
				{
					ON_SCOPE_EXIT { SupplementCaptureOwner(OutError, Graph.GetPathName(), TEXT("/nodes"), Entry.Key); };
					const TSharedPtr<FJsonObject> Source = SourceNodes->GetObjectField(Entry.Key);
					FString SourceId;
					Source->TryGetStringField(TEXT("id"), SourceId);
					UPCGNode* Node = NativeNodes.FindRef(SourceId);
					if (!Node) return Fail(OutError, CaptureFact(TEXT("content_graph_read_pcg_node_missing"), TEXT("Projected PCG node has no matching native node."), TEXT("/nodes/") + Entry.Key, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), SourceId));
					CapturedNodes.Add(SourceId);
					const TSharedRef<FJsonObject> State = Entry.Value->AsObject().ToSharedRef();
					State->SetStringField(TEXT("name"), Entry.Key);
					if (UPCGSettings* Settings = Node->GetSettings())
					{
						const FText Title = Settings->GetDefaultNodeTitle();
						const FString* SourceTitle = FTextInspector::GetSourceString(Title);
						State->SetStringField(TEXT("node_type"), SourceTitle ? *SourceTitle : Title.ToString());
					}
					const TSharedPtr<FJsonObject>* Interface = nullptr;
					if (InterfaceNodes && (*InterfaceNodes)->TryGetObjectField(Entry.Key, Interface))
						State->SetObjectField(TEXT("pin_interface"), *Interface);
					FUeAgentContentGraphItem* Item = OutCapture.Items.FindByPredicate(
						[Node](const FUeAgentContentGraphItem& Value) { return Value.ItemId == Node->GetPathName(); });
					if (!Item)
					{
						return Fail(OutError, CaptureFact(TEXT("content_graph_read_pcg_object_missing"), TEXT("Native PCG node is absent from the captured objects."), TEXT("/nodes"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Node->GetPathName()));
					}
					Item->ItemKind = TEXT("pcg_node");
					Item->SemanticIdentity = SemanticIdentity(TEXT("uai.semantic.pcg.node"),
						{{TEXT("public_name"), Entry.Key}, {TEXT("node_id"), SourceId}}, TEXT("item"));
					if (!TryCanonicalObject(State, Resources, Item->State, OutError)) return false;
				}
			}
			return CapturedNodes.Num() == NativeNodes.Num()
				|| Fail(OutError, CaptureFact(TEXT("content_graph_read_pcg_nodes_incomplete"), TEXT("PCG capture does not cover all native nodes."), TEXT("/nodes"), MakeShared<FJsonValueNumber>(CapturedNodes.Num()), MakeShared<FJsonValueNumber>(NativeNodes.Num())));
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
			auto* FunctionInstance = Cast<UMaterialFunctionInstance>(&Owner);
			auto* Instance = Cast<UMaterialInstanceConstant>(&Owner);
			if (FunctionInstance || Instance)
			{
				TSharedPtr<FJsonValue> Parameters;
				TArray<UeAgentMaterialAuthoring::FInstanceParameterDefault> Defaults;
				UE::UAI::Contract::FDiagnostic Error;
				if (FunctionInstance)
				{
					if (!UeAgentMaterialAuthoring::FunctionInstance::Read(*FunctionInstance, Parameters, Error, nullptr, &Defaults))
						return Fail(OutError, Error, Owner.GetPathName());
				}
				else
				{
					TArray<TSharedPtr<FJsonValue>> Values;
					if (!UeAgentMaterialAuthoring::CaptureInstanceParameters(*Instance, Values, Error, nullptr, &Defaults))
					{
						if (Error.TargetPath.IsEmpty()) Error.TargetPath = Owner.GetPathName();
						return Fail(OutError, Error, Owner.GetPathName());
					}
					Parameters = MakeShared<FJsonValueArray>(Values);
				}
				TArray<FString> RevisionFacts;
				for (int32 Index = 0; Index < Parameters->AsArray().Num(); ++Index)
				{
					const auto Row = Parameters->AsArray()[Index]->AsObject();
					const FString Kind = Row->GetStringField(TEXT("parameter_kind"));
					const FString Name = Row->GetStringField(TEXT("parameter_name"));
					const FString Association = Row->GetStringField(TEXT("association"));
					const FString ParameterIndex = FString::FromInt(Row->GetIntegerField(TEXT("parameter_index")));
					FUeAgentContentGraphParameter Captured;
					if (!Defaults.IsValidIndex(Index) || !Defaults[Index].Value.IsValid())
					{
						Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("content_graph_read_state_default_missing"),
							TEXT("The native material parameter has no inherited or declared default."), TEXT("content_capture"));
						Error.SourceKind = TEXT("native_result");
						Error.PropertyPath = TEXT("/parameters/") + Kind + TEXT(":") + Name + TEXT("/default");
						return Fail(OutError, Error, Owner.GetPathName());
					}
					const auto& NativeDefault = Defaults[Index];
					if (!TryMakeManualFieldState(Row->TryGetField(TEXT("value")), NativeDefault.Value,
						TEXT("material.parameter.") + Kind, Row->GetBoolField(TEXT("authored_override")),
						Resources, Captured.State, OutError)) return false;
					Captured.State.Default = FUeAgentDefaultValue(
						EUeAgentDefaultSourceKind::DomainNative,
						NativeDefault.SourceIdentity, NativeDefault.Provenance,
						Captured.State.Default.GetValue().GetValue());
					if (Kind == TEXT("texture") || Kind == TEXT("texture_collection")
						|| Kind == TEXT("runtime_virtual_texture") || Kind == TEXT("sparse_volume_texture"))
					{
						// A native resource reference can be null independently of its inherited value.
						const auto ValueKind = Captured.State.Value->GetKind();
						const auto DefaultKind = Captured.State.Default.GetValue().GetValue()->GetKind();
						const auto KindWhenPresent = ValueKind == EUeAgentValueKind::Null ? DefaultKind : ValueKind;
						Captured.State.Type = FUeAgentCanonicalType(TEXT("material.parameter.") + Kind,
							KindWhenPresent, true, ContainerKind(KindWhenPresent));
					}
					Captured.ParameterId = Kind + TEXT(":") + Name + TEXT(":") + Association + TEXT(":") + ParameterIndex;
					Captured.OwnerId = Owner.GetPathName();
					Captured.SemanticIdentity = SemanticIdentity(TEXT("uai.semantic.material.parameter"),
						{{TEXT("name"), Name}, {TEXT("association"), Association}, {TEXT("index"), ParameterIndex}, {TEXT("type"), Kind}}, TEXT("parameter"));
					OutCapture.Parameters.Add(MoveTemp(Captured));
					FString Fact;
					if (!UE::UAI::CommandContract::Json::TryCanonicalize(Row, Fact, Error))
						return Fail(OutError, Error, Owner.GetPathName());
					RevisionFacts.Add(MoveTemp(Fact));
					const auto DefaultFact = MakeShared<FJsonObject>();
					DefaultFact->SetStringField(TEXT("parameter"), Kind + TEXT(":") + Name + TEXT(":") + Association + TEXT(":") + ParameterIndex);
					DefaultFact->SetField(TEXT("default"), NativeDefault.Value);
					DefaultFact->SetStringField(TEXT("source"), NativeDefault.SourceIdentity);
					DefaultFact->SetStringField(TEXT("provenance"), NativeDefault.Provenance);
					if (!UE::UAI::CommandContract::Json::TryCanonicalize(DefaultFact, Fact, Error))
						return Fail(OutError, Error, Owner.GetPathName());
					RevisionFacts.Add(MoveTemp(Fact));
				}
				OutNativeRevision = CaptureRevision(Owner, MoveTemp(RevisionFacts));
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
				if (NativeError.TargetPath.IsEmpty()) NativeError.TargetPath = Owner.GetPathName();
				return Fail(OutError, NativeError, Owner.GetPathName());
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

			// Consume the same capture and composition as ordinary HJSON export.
			// Rebuilding aliases from topology keys would give repeated classes a
			// different ordering from composition's semantic unit identities.
			UeAgentMaterialAuthoring::FMaterialAuthoringCapture Authoring;
			UE::UAI::Contract::FDiagnostic AuthoringError;
			if (!UeAgentMaterialAuthoring::TryCaptureMaterialAuthoringState(
				Owner, TEXT("llm_authoring"), Authoring, AuthoringError))
			{
				return Fail(OutError, AuthoringError, Owner.GetPathName());
			}
			TMap<FString, TSharedPtr<FJsonObject>> InterfacesById;
			for (const auto& Entry : Authoring.Snapshot.CopyEntries())
			{
				if (Entry.SemanticId != UeAgentMaterialAuthoring::ExportGraphSemanticId
					|| !Entry.Value.IsValid() || Entry.Value->Type != EJson::Object) continue;
				const TSharedPtr<FJsonObject>* Nodes = nullptr;
				if (!Entry.Value->AsObject()->TryGetObjectField(TEXT("nodes"), Nodes)) continue;
				for (const auto& Node : (*Nodes)->Values)
				{
					const TSharedPtr<FJsonObject>* Interface = nullptr;
					if (Node.Value.IsValid() && Node.Value->Type == EJson::Object
						&& Node.Value->AsObject()->TryGetObjectField(TEXT("pin_interface"), Interface))
						InterfacesById.Add(Node.Key, *Interface);
				}
			}
			TMap<FString, TSharedPtr<FJsonObject>> StatesById;
			TMap<FString, FString> NamesByItemKey;
			for (const auto& Unit : Authoring.Model->GetUnits())
			{
				if (Unit.GetUnitKind() != ESemanticUnitKind::StructureState) continue;
				const auto* Nodes = Unit.GetValues().Find(TEXT("nodes"));
				if (!Nodes || (*Nodes)->GetKind() != EUeAgentValueKind::Object) continue;
				for (const auto& Node : (*Nodes)->GetObjectItems())
				{
					ON_SCOPE_EXIT { SupplementCaptureOwner(OutError, Owner.GetPathName(), TEXT("/nodes"), Node.Key); };
					TSharedPtr<FJsonValue> Json;
					if (!CanonicalJson::TryToJsonValue(*Node.Value, Json, AuthoringError))
						return Fail(OutError, AuthoringError, Node.Key);
					if (!Json.IsValid() || Json->Type != EJson::Object)
						return Fail(OutError, CaptureFact(TEXT("content_graph_read_material_node_invalid"), TEXT("Captured material node must be an object."), TEXT("/nodes/") + Node.Key, MakeShared<FJsonValueString>(Json.IsValid() ? TEXT("non-object") : TEXT("missing")), MakeShared<FJsonValueString>(TEXT("object"))));
					TSharedPtr<FJsonObject> State = Json->AsObject();
					FString Id, Type;
					if (!State->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
						return Fail(OutError, CaptureFact(TEXT("content_graph_read_material_node_identity_missing"), TEXT("Captured material node requires a nonempty id."), TEXT("/nodes/") + Node.Key + TEXT("/id"), State->TryGetField(TEXT("id")), MakeShared<FJsonValueString>(TEXT("nonempty string"))));
					State->TryGetStringField(TEXT("name"), Type);
					State->SetStringField(TEXT("node_type"), Type);
					State->SetStringField(TEXT("name"), Node.Key);
					const TSharedPtr<FJsonObject>* Candidate = nullptr;
					FString Function;
					if (State->TryGetObjectField(TEXT("candidate_context"), Candidate)
						&& (*Candidate)->TryGetStringField(TEXT("material_function_path"), Function))
						State->SetStringField(TEXT("material_function"), Function);
					State->RemoveField(TEXT("candidate_context"));
					State->RemoveField(TEXT("id"));
					State->RemoveField(TEXT("operation"));
					if (const auto* Interface = InterfacesById.Find(Id))
						State->SetObjectField(TEXT("pin_interface"), *Interface);
					StatesById.Add(Id, MoveTemp(State));
				}
			}

			for (int32 Index = 0; Index < Topology.GetItems().Num(); ++Index)
			{
				const FMaterialNiagaraGraphItemIdentity& Item =
					Topology.GetItems()[Index];
				UMaterialExpression* Expression =
					ExpressionsByGuid.FindRef(Item.GetNativeGuid());
				const FString SourceId = Expression ? Item.GetNativeGuid() : Item.GetStableBuiltinRole();
				const auto* State = StatesById.Find(SourceId);
				// The native hook's synthetic root is not a node in a Material Function.
				if (!State && !Expression && Cast<UMaterialFunctionInterface>(&Owner)) continue;
				if (!State)
				{
					auto Diagnostic = CaptureFact(TEXT("content_graph_read_material_node_state_missing"),
						TEXT("Native material expression has no matching authoring state."), TEXT("/nodes/") + FString::FromInt(Index),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), SourceId);
					if (Expression) Diagnostic.CandidateName = Expression->GetName();
					return Fail(OutError, MoveTemp(Diagnostic));
				}
				const FString PublicName = (*State)->GetStringField(TEXT("name"));
				NamesByItemKey.Add(Item.GetItemKey(), PublicName);
				FUeAgentContentGraphItem Captured;
				Captured.ItemId = Item.GetItemKey();
				Captured.ItemKind = TEXT("material_expression");
				Captured.OwnerId = Owner.GetPathName();
				Captured.ClassPath = Expression
					? Expression->GetClass()->GetPathName()
					: Item.GetStableBuiltinRole();
				Captured.ContainerId = RootContainerId;
				Captured.Order = Index;
				if (!TryCanonicalObject(State->ToSharedRef(), Resources, Captured.State, OutError)) return false;
				TMap<FString, FString> Selectors = {{TEXT("public_name"), PublicName}};
				if (!Item.GetNativeGuid().IsEmpty()) Selectors.Add(TEXT("native_guid"), Item.GetNativeGuid());
				else Selectors.Add(TEXT("builtin_role"), Item.GetStableBuiltinRole());
				Captured.SemanticIdentity = SemanticIdentity(
					TEXT("uai.semantic.material.expression"),
					MoveTemp(Selectors),
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
				const FString PublicName = NamesByItemKey.FindRef(Pin.GetOwnerItemKey())
					+ TEXT(".") + Direction + TEXT(".")
					+ (Pin.GetNativeIndex() == INDEX_NONE ? Pin.GetNativeName() : FString::FromInt(Pin.GetNativeIndex()));
				auto State = MakeShared<FJsonObject>();
				State->SetStringField(TEXT("name"), PublicName);
				State->SetStringField(TEXT("node"), NamesByItemKey.FindRef(Pin.GetOwnerItemKey()));
				State->SetStringField(TEXT("pin_name"), Pin.GetNativeName());
				State->SetStringField(TEXT("direction"), Direction);
				if (Pin.GetNativeIndex() != INDEX_NONE) State->SetNumberField(TEXT("index"), Pin.GetNativeIndex());
				if (!TryCanonicalObject(State, Resources, Captured.State, OutError)) return false;
				Captured.SemanticIdentity = SemanticIdentity(
					TEXT("uai.semantic.material.endpoint"),
					{{TEXT("name"), Pin.GetNativeName()},
						{TEXT("public_name"), PublicName},
						{TEXT("index"), FString::FromInt(Pin.GetNativeIndex())},
						{TEXT("direction"), Direction}},
					TEXT("endpoint"));
				OutCapture.Items.Add(MoveTemp(Captured));
				OutCapture.Relations.Add({
					TEXT("material.endpoint:") + Pin.GetPinKey(),
					TEXT("membership"),
					Pin.GetOwnerItemKey(),
					Pin.GetPinKey(),
				});
			}
			for (const FMaterialNiagaraGraphLinkIdentity& Link : Topology.GetLinks())
			{
				OutCapture.Relations.Add({
					Link.GetLinkKey(),
					TEXT("connection"),
					Link.GetFromPinKey(),
					Link.GetToPinKey(),
				});
			}
			for (const FMaterialNiagaraGraphOrderingIdentity& Ordering
				: Topology.GetOrdering())
			{
				OutCapture.Relations.Add({
					Ordering.GetOrderingKey(),
					TEXT("order"),
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
			FUeAgentResourceTracker& Resources,
			FUeAgentContentGraphCapture& OutCapture,
			FString& OutNativeRevision,
			FUeAgentContentGraphReadError& OutError)
		{
			OutNativeRevision.Reset();
			TArray<FString> RevisionFacts;
			const auto AssetKind = Cast<USoundCue>(&Owner)
				? UeAgentAudioAuthoring::EAssetKind::SoundCue
				: UeAgentAudioAuthoring::EAssetKind::MetaSound;
			TSharedPtr<FJsonObject> DocumentState;
			UE::UAI::Contract::FDiagnostic DocumentError;
			if (!UeAgentAudioAuthoring::BuildDocument(AssetKind, Owner, DocumentState, DocumentError))
				return Fail(OutError, MoveTemp(DocumentError), Owner.GetPathName());
			// Share the native authoring capture's aliases and node values. Internal
			// source ids bind topology below; they are not the public node names.
			TMap<FString, TSharedPtr<FJsonObject>> NodeStates;
			const TSharedPtr<FJsonObject>* Nodes = nullptr;
			if (DocumentState.IsValid() && DocumentState->TryGetObjectField(TEXT("nodes"), Nodes))
			{
				for (const auto& Node : (*Nodes)->Values)
				{
					if (!Node.Value.IsValid() || Node.Value->Type != EJson::Object) continue;
					FString SourceId;
					if (!Node.Value->AsObject()->TryGetStringField(TEXT("id"), SourceId)) continue;
					auto State = MakeShared<FJsonObject>();
					State->Values = Node.Value->AsObject()->Values;
					State->RemoveField(TEXT("id"));
					State->SetStringField(TEXT("name"), Node.Key);
					NodeStates.Add(SourceId, MoveTemp(State));
				}
			}
			auto AddItem = [&] (
				const FString& Id,
				const FString& Kind,
				const FString& ItemOwner,
				const FString& Type,
				const FString& Container,
				const int32 Order,
				const TSharedPtr<FJsonObject>& State) -> bool
			{
				if (Id.IsEmpty() || OutCapture.Items.ContainsByPredicate(
					[&Id](const FUeAgentContentGraphItem& Item)
					{
						return Item.ItemId == Id;
					}))
				{
					return true;
				}
				if (!State.IsValid())
					return Fail(OutError, CaptureFact(TEXT("content_graph_read_audio_node_state_missing"),
						TEXT("Native audio node has no matching authoring state."), TEXT("/nodes"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Id));
				FUeAgentContentGraphItem Item;
				Item.ItemId = Id;
				Item.ItemKind = Kind;
				Item.OwnerId = ItemOwner;
				Item.ClassPath = Type;
				Item.ContainerId = Container;
				Item.Order = Order;
				if (!TryCanonicalObject(State.ToSharedRef(), Resources, Item.State, OutError)) return false;
				Item.SemanticIdentity = SemanticIdentity(
					TEXT("uai.semantic.audio_graph.item"),
					{{TEXT("identity"), Id}, {TEXT("public_name"), State->GetStringField(TEXT("name"))}}, TEXT("item"));
				OutCapture.Items.Add(MoveTemp(Item));
				RevisionFacts.Add(Id + TEXT("|") + Kind + TEXT("|")
					+ ItemOwner + TEXT("|") + Type);
				return true;
			};
			auto AddRelation = [&] (
				const FString& Id,
				const FString& Kind,
				const FString& Source,
				const FString& Target,
				const TSharedRef<FJsonObject>& State) -> bool
			{
				if (Id.IsEmpty() || OutCapture.Relations.ContainsByPredicate(
					[&Id](const FUeAgentContentGraphRelation& Relation)
					{
						return Relation.RelationId == Id;
					}))
				{
					return true;
				}
				FUeAgentContentGraphRelation Relation;
				Relation.RelationId = Id;
				Relation.RelationKind = Kind;
				Relation.SourceItemId = Source;
				Relation.TargetItemId = Target;
				if (!TryCanonicalObject(State, Resources, Relation.State, OutError)) return false;
				Relation.SemanticIdentity = SemanticIdentity(
					TEXT("uai.semantic.audio_graph.relation"),
					{{TEXT("identity"), Id}}, TEXT("relation"));
				OutCapture.Relations.Add(MoveTemp(Relation));
				RevisionFacts.Add(Id + TEXT("|") + Kind + TEXT("|")
					+ Source + TEXT("|") + Target);
				return true;
			};
			auto PublicNodeName = [&NodeStates](const FString& SourceId)
			{
				const auto State = NodeStates.FindRef(SourceId);
				return State ? State->GetStringField(TEXT("name")) : FString();
			};

			if (USoundCue* Cue = Cast<USoundCue>(&Owner))
			{
				const FString GraphId = TEXT("sound_cue:graph");
				OutCapture.Containers.Add({GraphId, TEXT("sound_cue_graph"),
					Owner.GetPathName(), RootContainerId,
					OutCapture.Containers.Num()});
				if (const auto OutputState = NodeStates.FindRef(TEXT("sound_cue_root")))
				{
					if (!AddItem(TEXT("sound_cue:output"), TEXT("sound_cue_node"), Owner.GetPathName(),
						OutputState->GetStringField(TEXT("class_path")), GraphId, INDEX_NONE, OutputState)) return false;
				}
				for (int32 Index = 0; Index < Cue->AllNodes.Num(); ++Index)
				{
					USoundNode* Node = Cue->AllNodes[Index];
					if (!Node) continue;
					const FString NodeId = TEXT("sound_node:") + Node->GetPathName();
					if (!AddItem(NodeId, TEXT("sound_cue_node"), Owner.GetPathName(),
						Node->GetClass()->GetPathName(), GraphId, Index,
						NodeStates.FindRef(Node->GetName()))) return false;
					for (int32 ChildIndex = 0;
						ChildIndex < Node->ChildNodes.Num(); ++ChildIndex)
					{
						USoundNode* Child = Node->ChildNodes[ChildIndex];
						if (!Child) continue;
						const FString ChildId = TEXT("sound_node:") + Child->GetPathName();
						auto State = MakeShared<FJsonObject>();
						State->SetStringField(TEXT("kind"), TEXT("connection"));
						State->SetStringField(TEXT("from"), PublicNodeName(Node->GetName()));
						State->SetStringField(TEXT("to"), PublicNodeName(Child->GetName()));
						State->SetNumberField(TEXT("child_index"), ChildIndex);
						if (!AddRelation(FString::Printf(TEXT("sound_child:%s:%d:%s"),
							*NodeId, ChildIndex, *ChildId),
							TEXT("connection"), NodeId, ChildId, State)) return false;
					}
				}
				if (Cue->FirstNode)
				{
					auto State = MakeShared<FJsonObject>();
					State->SetStringField(TEXT("kind"), TEXT("membership"));
					State->SetStringField(TEXT("from"), Owner.GetName());
					State->SetStringField(TEXT("to"), PublicNodeName(Cue->FirstNode->GetName()));
					if (!AddRelation(TEXT("sound_root:") + Cue->FirstNode->GetPathName(),
						TEXT("membership"), RootContainerId,
						TEXT("sound_node:") + Cue->FirstNode->GetPathName(), State)) return false;
				}
			}
			else if (UMetaSoundSource* Source = Cast<UMetaSoundSource>(&Owner))
			{
				const IMetaSoundDocumentInterface* Interface =
					Cast<IMetaSoundDocumentInterface>(Source);
				if (!Interface)
				{
					return Fail(OutError, CaptureFact(TEXT("content_graph_read_metasound_document_missing"), TEXT("MetaSound source does not expose a document interface."), TEXT("/document"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
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
				TMap<FGuid, const FMetasoundFrontendClass*> Dependencies;
				TMap<FString, FTopLevelAssetPath> ReferencePaths;
				for (const FMetasoundFrontendClass& Dependency : Document.Dependencies)
				{
					const FString Name = Dependency.Metadata.GetClassName().ToString();
					DependencyNames.Add(Dependency.ID, Name);
					Dependencies.Add(Dependency.ID, &Dependency);
					if (FMetaSoundAssetKey::IsValidType(Dependency.Metadata.GetType()))
					{
						const FMetaSoundAssetKey Key(Dependency.Metadata);
						ReferencePaths.Add(Key.ToString(), Metasound::Frontend::IMetaSoundAssetManager::GetChecked().FindAssetPath(Key));
					}
					auto State = MakeShared<FJsonObject>();
					const FString DisplayName = Dependency.Metadata.GetDisplayName().ToString();
					State->SetStringField(TEXT("name"), DisplayName.IsEmpty() ? Name : DisplayName);
					State->SetStringField(TEXT("class_name"), Name);
					State->SetNumberField(TEXT("major"), Dependency.Metadata.GetVersion().Major);
					State->SetNumberField(TEXT("minor"), Dependency.Metadata.GetVersion().Minor);
					if (!AddItem(TEXT("metasound_dependency:")
						+ Dependency.ID.ToString(EGuidFormats::DigitsWithHyphensLower),
						TEXT("metasound_dependency"), Owner.GetPathName(), Name,
						GraphId, OutCapture.Items.Num(), State)) return false;
				}
				for (const FString& Reference : Source->GetReferencedAssetClassKeys())
				{
					auto State = MakeShared<FJsonObject>();
					const auto AssetPath = ReferencePaths.FindRef(Reference);
					State->SetStringField(TEXT("name"), AssetPath.IsNull() ? Reference : AssetPath.GetAssetName().ToString());
					if (!AssetPath.IsNull()) State->SetStringField(TEXT("asset"), AssetPath.ToString());
					if (!AddItem(TEXT("audio_reference:") + Reference,
						TEXT("external_audio_reference"), Owner.GetPathName(),
						Reference, GraphId, OutCapture.Items.Num(), State)) return false;
				}
				const FMetasoundFrontendClassInterface& GraphInterface =
					Document.RootGraph.GetDefaultInterface();
				auto AddEndpoint = [&] (
					const FMetasoundFrontendClassVertex& Vertex,
					const FString& Direction,
					const int32 Order) -> bool
				{
					auto State = MakeShared<FJsonObject>();
					State->SetStringField(TEXT("name"), Vertex.Name.ToString());
					State->SetStringField(TEXT("direction"), Direction);
					State->SetStringField(TEXT("type"), Vertex.TypeName.ToString());
					return AddItem(FString::Printf(TEXT("metasound_endpoint:%s:%s:%s"),
						*Direction, *Vertex.Name.ToString(), *Vertex.TypeName.ToString()),
						TEXT("metasound_endpoint"), Owner.GetPathName(),
						Vertex.TypeName.ToString(), GraphId, Order, State);
				};
				for (int32 Index = 0; Index < GraphInterface.Inputs.Num(); ++Index)
				{
					if (!AddEndpoint(GraphInterface.Inputs[Index], TEXT("input"), Index)) return false;
				}
				for (int32 Index = 0; Index < GraphInterface.Outputs.Num(); ++Index)
				{
					if (!AddEndpoint(GraphInterface.Outputs[Index], TEXT("output"), Index)) return false;
				}
				for (int32 Index = 0; Index < Graph.Nodes.Num(); ++Index)
				{
					const FMetasoundFrontendNode& Node = Graph.Nodes[Index];
					const FString NodeId = TEXT("metasound_node:")
						+ Node.GetID().ToString(EGuidFormats::DigitsWithHyphensLower);
					auto State = NodeStates.FindRef(Node.GetID().ToString(EGuidFormats::DigitsWithHyphensLower));
					const auto* Dependency = Dependencies.FindRef(Node.ClassID);
					// Context-only native nodes have no ordinary authoring record.
					// Preserve their native state rather than dropping them from readback.
					if (!State && Dependency && Dependency->Metadata.GetType() != EMetasoundFrontendClassType::External
						&& Dependency->Metadata.GetType() != EMetasoundFrontendClassType::Graph)
					{
						State = MakeShared<FJsonObject>();
						State->SetStringField(TEXT("name"), Node.Name.ToString());
						State->SetStringField(TEXT("class_name"), DependencyNames.FindRef(Node.ClassID));
						NodeStates.Add(Node.GetID().ToString(EGuidFormats::DigitsWithHyphensLower), State);
					}
					if (!AddItem(NodeId, TEXT("metasound_node"), Owner.GetPathName(),
						DependencyNames.FindRef(Node.ClassID), GraphId, Index, State)) return false;
					for (int32 InputIndex = 0;
						InputIndex < Node.Interface.Inputs.Num(); ++InputIndex)
					{
						const FMetasoundFrontendVertex& Input =
							Node.Interface.Inputs[InputIndex];
						auto ParameterState = MakeShared<FJsonObject>();
						ParameterState->SetStringField(TEXT("name"), Input.Name.ToString());
						ParameterState->SetStringField(TEXT("node"), State->GetStringField(TEXT("name")));
						ParameterState->SetStringField(TEXT("type"), Input.TypeName.ToString());
						if (!AddItem(FString::Printf(TEXT("metasound_parameter:%s:%s"),
							*NodeId, *Input.VertexID.ToString(
								EGuidFormats::DigitsWithHyphensLower)),
							TEXT("metasound_parameter"), NodeId,
							Input.TypeName.ToString(), GraphId, InputIndex, ParameterState)) return false;
					}
				}
				for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
				{
					const FString SourceId = TEXT("metasound_node:")
						+ Edge.FromNodeID.ToString(EGuidFormats::DigitsWithHyphensLower);
					const FString TargetId = TEXT("metasound_node:")
						+ Edge.ToNodeID.ToString(EGuidFormats::DigitsWithHyphensLower);
					auto State = MakeShared<FJsonObject>();
					State->SetStringField(TEXT("kind"), TEXT("connection"));
					State->SetStringField(TEXT("from"), PublicNodeName(Edge.FromNodeID.ToString(EGuidFormats::DigitsWithHyphensLower)));
					State->SetStringField(TEXT("to"), PublicNodeName(Edge.ToNodeID.ToString(EGuidFormats::DigitsWithHyphensLower)));
					for (const auto& Node : Graph.Nodes)
					{
						if (Node.GetID() == Edge.FromNodeID)
							for (const auto& Pin : Node.Interface.Outputs)
								if (Pin.VertexID == Edge.FromVertexID) State->SetStringField(TEXT("from_pin"), Pin.Name.ToString());
						if (Node.GetID() == Edge.ToNodeID)
							for (const auto& Pin : Node.Interface.Inputs)
								if (Pin.VertexID == Edge.ToVertexID) State->SetStringField(TEXT("to_pin"), Pin.Name.ToString());
					}
					if (!AddRelation(FString::Printf(TEXT("metasound_link:%s:%s:%s:%s"),
						*SourceId, *Edge.FromVertexID.ToString(
							EGuidFormats::DigitsWithHyphensLower), *TargetId,
						*Edge.ToVertexID.ToString(EGuidFormats::DigitsWithHyphensLower)),
						TEXT("connection"), SourceId, TargetId, State)) return false;
				}
			}
			else
			{
				return Fail(OutError, CaptureFact(TEXT("content_graph_read_audio_target_unsupported"), TEXT("Audio graph capture supports SoundCue and MetaSoundSource."), TEXT("/class"), MakeShared<FJsonValueString>(Owner.GetClass()->GetPathName()), MakeShared<FJsonValueString>(TEXT("SoundCue or MetaSoundSource"))));
			}
			RevisionFacts.Add(TEXT("compile_settled|true"));
			OutNativeRevision = CaptureRevision(Owner, MoveTemp(RevisionFacts));
			return true;
		}

		bool AugmentTextureGraphCapture(
			UObject& Owner,
			const FString& RootContainerId,
			FUeAgentResourceTracker& Resources,
			FUeAgentContentGraphCapture& OutCapture,
			FString& OutNativeRevision,
			FUeAgentContentGraphReadError& OutError,
			const TMap<int32, TSet<FString>>* FieldsByNode = nullptr,
			bool bIncludeTopology = false)
		{
			OutNativeRevision.Reset();
			UTextureGraph* TextureGraph = Cast<UTextureGraph>(&Owner);
			UTG_Graph* Graph = TextureGraph ? TextureGraph->Graph() : nullptr;
			if (!TextureGraph || !Graph)
			{
				return Fail(OutError, CaptureFact(TEXT("content_graph_read_texture_graph_missing"), TEXT("TextureGraph capture requires a TextureGraph asset with a graph."), TEXT("/graph"), MakeShared<FJsonValueString>(!TextureGraph ? Owner.GetClass()->GetPathName() : TEXT("graph missing")), MakeShared<FJsonValueString>(TEXT("TextureGraph with graph"))));
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
					TEXT("uai.semantic.texture_graph.graph"),
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
					if (FieldsByNode && !bIncludeTopology && !FieldsByNode->Contains(Index)) return;
					const FString Id = NodeId(Index);
					const FString ClassPath =
						Node->GetExpression()->GetClass()->GetPathName();
					// All editable fields not represented by compact parameters use the
					// same FProperty descriptor and native expression owner.
					TArray<FProperty*> ExpressionProperties;
					if (!FieldsByNode || FieldsByNode->Contains(Index))
						CollectProperties(*Node->GetExpression(), ExpressionProperties);
					for (FProperty* Property : ExpressionProperties)
					{
						if (!Property || !DomainAdapters::AudioMediaGraph::FAudioMediaGraphTextureGraphNativeHook::UsesReflectedExpressionProperty(
							*Node->GetExpression(), *Property)) continue;
						if (FieldsByNode && !FieldsByNode->FindChecked(Index).Contains(Property->GetName())) continue;
						FUeAgentContentGraphProperty Captured;
						Captured.OwnerId = Node->GetExpression()->GetPathName();
						Captured.FieldId = Property->GetName();
						UeAgentAuthoringState::FReadRequest Request;
						UeAgentAuthoringTargetLocator::FResolveResult TargetResult;
						if (!UeAgentAuthoringState::BuildPropertyReadRequest(Node->GetExpression(), Captured.FieldId,
							TEXT("core.property"), Request, TargetResult))
						{ bCaptureValid = Fail(OutError, TargetResult.Diagnostic, Captured.FieldId); return; }
						// TG's native Details eligibility is evaluated against the current expression.
						UeAgentAuthoringState::FCaptureResult PropertyCapture;
						if (!UeAgentAuthoringState::CaptureSnapshot({Request}, PropertyCapture))
						{ bCaptureValid = Fail(OutError, PropertyCapture.Diagnostic, Captured.FieldId); return; }
						UeAgentAuthoringState::FStateEntry Descriptor;
						if (!PropertyCapture.Snapshot.CopyEntry(0, Descriptor))
						{ bCaptureValid = Fail(OutError, CaptureFact(TEXT("texture_graph_setting_capture_missing"), TEXT("TextureGraph setting capture returned no state entry."), Captured.FieldId, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true))); return; }
						const auto Metadata = MakeShared<FJsonObject>(); Metadata->SetObjectField(TEXT("type"), Descriptor.Type.ToJsonObject());
						if (Descriptor.CandidatesHeader.IsValid()) Metadata->SetField(TEXT("candidates_header"), Descriptor.CandidatesHeader);
						if (Descriptor.Candidates.IsValid()) Metadata->SetField(TEXT("candidates"), Descriptor.Candidates);
						if (Descriptor.ValueDefinition.IsValid()) Metadata->SetField(TEXT("value_definition"), Descriptor.ValueDefinition);
						if (Descriptor.ComparisonValueDefinition.IsValid()) Metadata->SetField(TEXT("comparison_value_definition"), Descriptor.ComparisonValueDefinition);
						UObject* DefaultExpression = Node->GetExpression()->GetClass()->GetDefaultObject();
						UeAgentPropertyValueCodec::FOptions CodecOptions;
						const auto DefaultEncoded = UeAgentPropertyValueCodec::Encode(Property,
							Property->ContainerPtrToValuePtr<void>(DefaultExpression), Node->GetExpression(), CodecOptions);
						FUeAgentCanonicalValuePtr DefaultValue; FUeAgentStateSemanticError StateError;
						if (!TryCanonicalValue(Descriptor.Value, Resources, Captured.State.Value, StateError)
							|| !TryCanonicalValue(DefaultEncoded, Resources, DefaultValue, StateError))
						{ bCaptureValid = Fail(OutError, StateError); return; }
						Captured.State.Type = FUeAgentCanonicalType(Descriptor.Type.Kind, Captured.State.Value->GetKind(), false,
							ContainerKind(Captured.State.Value->GetKind()));
						Captured.State.Default = FUeAgentDefaultValue(EUeAgentDefaultSourceKind::ClassDefaultObject,
							DefaultExpression->GetPathName(), TEXT("reflection_class_default"), DefaultValue.ToSharedRef());
						auto StoredOptions = CodecOptions; StoredOptions.bCompareStoredValues = true;
						const bool bOverridden = !UeAgentPropertyValueCodec::AreSemanticallyEquivalent(Property,
							Property->ContainerPtrToValuePtr<void>(Node->GetExpression()), Property->ContainerPtrToValuePtr<void>(DefaultExpression),
							Node->GetExpression(), StoredOptions);
						Captured.State.AuthoredPresence = bOverridden ? EUeAgentAuthoredPresence::Present : EUeAgentAuthoredPresence::Absent;
						Captured.State.OverrideState = bOverridden ? EUeAgentOverrideState::Overridden : EUeAgentOverrideState::Inherited;
						if (!TryCanonicalObject(Metadata, Resources, Captured.State.AuthoringDescriptor, OutError))
						{ bCaptureValid = false; return; }
						Captured.State.AuthoringDetails = FUeAgentStateDetails(Descriptor.DisplayName, Descriptor.Tooltip,
							Descriptor.CategoryPath, Descriptor.bEditable, Descriptor.bAvailable ? TEXT("available") : TEXT("unavailable"), Descriptor.Constraints);
						OutCapture.Properties.RemoveAll([&](const auto& Existing) {
							return Existing.OwnerId == Captured.OwnerId && Existing.FieldId == Captured.FieldId; });
						Captured.OwnerId = Id;
						Captured.SemanticIdentity = SemanticIdentity(TEXT("uai.semantic.texture_graph.setting"),
							{{TEXT("asset_path"), Owner.GetPathName()}, {TEXT("owner_item_id"), Id},
							 {TEXT("node_index"), FString::FromInt(Index)}, {TEXT("property_path"), Captured.FieldId},
							 {TEXT("expression_path"), Node->GetExpression()->GetPathName()}}, TEXT("property"));
						OutCapture.Properties.Add(MoveTemp(Captured));
					}

					FString NodeName =
						Node->GetExpression()->GetTitleName().ToString();
					if (NodeName.IsEmpty())
					{
						NodeName = Node->GetExpression()->GetClass()->GetName();
					}
					FUeAgentContentGraphItem Item{
						Id,
						TEXT("texture_graph_node"),
						Owner.GetPathName(),
						ClassPath,
						GraphContainerId,
						Index,
						SemanticIdentity(
							TEXT("uai.semantic.texture_graph.node"),
							{{TEXT("asset_path"), Owner.GetPathName()},
							 {TEXT("node_index"), FString::FromInt(Index)},
							 {TEXT("item_id"), Id},
							 {TEXT("expression_path"),
								Node->GetExpression()->GetPathName()}},
							TEXT("item")),
					};
					TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
					State->SetStringField(TEXT("id"), Id);
					State->SetStringField(TEXT("name"), NodeName);
					State->SetStringField(
						TEXT("node_type"),
						Node->GetExpression()->GetClass()->GetName());
					if (FieldsByNode && !bIncludeTopology) {
						bCaptureValid = TryCanonicalObject(State, Resources, Item.State, OutError);
						if (bCaptureValid) OutCapture.Items.Add(MoveTemp(Item));
						RevisionFacts.Add(Id + TEXT("|") + ClassPath);
						return;
					}
					TArray<TSharedPtr<FJsonValue>> InterfacePins;
					Graph->ForEachPins(
						[&InterfacePins, Index](
							const UTG_Pin* Pin,
							const uint32,
							const uint32)
						{
							if (!Pin || Pin->GetId().NodeIdx() != Index)
							{
								return;
							}
							TSharedRef<FJsonObject> InterfacePin =
								MakeShared<FJsonObject>();
							InterfacePin->SetStringField(
								TEXT("name"), Pin->GetArgumentName().ToString());
							InterfacePin->SetStringField(
								TEXT("direction"),
								Pin->IsInput() ? TEXT("input") : TEXT("output"));
							InterfacePin->SetStringField(
								TEXT("type"), Pin->GetArgumentCPPTypeName().ToString());
							InterfacePins.Add(
								MakeShared<FJsonValueObject>(InterfacePin));
						});
					TSharedRef<FJsonObject> PinInterface = MakeShared<FJsonObject>();
					PinInterface->SetArrayField(TEXT("pins"), MoveTemp(InterfacePins));
					State->SetObjectField(TEXT("pin_interface"), PinInterface);
					// Keep editable input defaults typed and compact in the visible
					// graph node state.  Full pin topology remains in pin_interface.
					TSharedRef<FJsonObject> Inputs = MakeShared<FJsonObject>();
					Graph->ForEachPins(
						[&Inputs, Index](const UTG_Pin* InputPin, const uint32, const uint32)
						{
							if (!InputPin || InputPin->GetId().NodeIdx() != Index
								|| !InputPin->IsInput()) return;
							const bool bCompact = DomainAdapters::AudioMediaGraph::FAudioMediaGraphTextureGraphNativeHook::UsesCompactParameterValue(*InputPin);
							if (!InputPin->IsParam() && !bCompact) return;
							const FString PinName = InputPin->GetArgumentName().ToString();
							if (PinName.IsEmpty()) return;
							TSharedRef<FJsonObject> Input = MakeShared<FJsonObject>();
							if (InputPin->HasAliasName())
								Input->SetStringField(TEXT("alias"), InputPin->GetAliasName().ToString());
							if (bCompact && InputPin->IsArgBool())
							{
								bool Value = false;
								if (InputPin->GetValue(Value)) Input->SetBoolField(TEXT("value"), Value);
							}
							else if (bCompact && InputPin->IsArgColor())
							{
								FLinearColor Value;
								if (InputPin->GetValue(Value)) Input->SetArrayField(TEXT("value"), {
									MakeShared<FJsonValueNumber>(Value.R), MakeShared<FJsonValueNumber>(Value.G),
									MakeShared<FJsonValueNumber>(Value.B), MakeShared<FJsonValueNumber>(Value.A)});
							}
							else if (bCompact && InputPin->IsArgVector())
							{
								FVector4f Value;
								if (InputPin->GetValue(Value)) Input->SetArrayField(TEXT("value"), {
									MakeShared<FJsonValueNumber>(Value.X), MakeShared<FJsonValueNumber>(Value.Y),
									MakeShared<FJsonValueNumber>(Value.Z), MakeShared<FJsonValueNumber>(Value.W)});
							}
							else if (bCompact && InputPin->IsArgScalar())
							{
								double Value = 0.0;
								if (DomainAdapters::AudioMediaGraph::FAudioMediaGraphTextureGraphNativeHook::TryReadPinNumber(*InputPin, Value))
									Input->SetNumberField(TEXT("value"), Value);
							}
							else if (bCompact && InputPin->IsArgString())
							{
								FString Value;
								if (InputPin->GetValue(Value)) Input->SetStringField(TEXT("value"), Value);
							}
							if (Input->HasField(TEXT("value")) || Input->HasField(TEXT("alias")))
								Inputs->SetObjectField(PinName, Input);
						});
					if (!Inputs->Values.IsEmpty())
					{
						State->SetObjectField(TEXT("inputs"), Inputs);
					}
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

			if (!FieldsByNode || bIncludeTopology) Graph->ForEachPins(
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
							{{TEXT("asset_path"), Owner.GetPathName()},
							 {TEXT("node_index"), FString::FromInt(NodeIndex)},
							 {TEXT("pin_index"), FString::FromInt(PinIndex)},
							 {TEXT("item_id"), Id},
							 {TEXT("owner_item_id"), ParentId},
							 {TEXT("name"), ArgumentName},
							 {TEXT("direction"), Direction}},
							TEXT("item")),
					};
					TSharedRef<FJsonObject> PinState = MakeShared<FJsonObject>();
					PinState->SetStringField(TEXT("id"), Id);
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
						TEXT("membership"),
						ParentId,
						Id,
						SemanticIdentity(
							TEXT("uai.semantic.texture_graph.node_pin"),
							{{TEXT("asset_path"), Owner.GetPathName()},
							 {TEXT("pin_id"), Id}},
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
							{{TEXT("asset_path"), Owner.GetPathName()},
							 {TEXT("pin_id"), Id},
							 {TEXT("owner_item_id"), Id},
							 {TEXT("cpp_type"), CppType}},
							TEXT("property"));
						// A TG_Setting has one writable owner: its reflected expression property.
						if (!Pin->IsSetting()) OutCapture.Properties.Add(MoveTemp(Property));
						if (Pin->IsParam())
						{
							FUeAgentContentGraphParameter Parameter;
							Parameter.ParameterId = Id;
							Parameter.OwnerId = ParentId;
							Parameter.State = State;
							Parameter.SemanticIdentity = SemanticIdentity(
								TEXT("uai.semantic.texture_graph.parameter"),
								{{TEXT("asset_path"), Owner.GetPathName()},
								 {TEXT("pin_id"), Id},
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
					const UTG_Pin* TargetPin = PinsByIdentity.FindRef(TargetId);
					if (!TargetPin || !Pin->GetNodePtr() || !TargetPin->GetNodePtr())
					{
						continue;
					}
					const FString RelationId = TEXT("texture_graph.edge:")
						+ Pair.Key + TEXT("->") + TargetId;
					FUeAgentContentGraphRelation EdgeRelation{
						RelationId,
						TEXT("connection"),
						Pair.Key,
						TargetId,
						SemanticIdentity(
							TEXT("uai.semantic.texture_graph.edge"),
							{{TEXT("asset_path"), Owner.GetPathName()},
							 {TEXT("source_pin"), Pair.Key},
							 {TEXT("target_pin"), TargetId}},
							TEXT("relation")),
					};
					TSharedRef<FJsonObject> From = MakeShared<FJsonObject>();
					From->SetStringField(
						TEXT("node"),
						NodeId(Pin->GetNodePtr()->GetId().NodeIdx()));
					From->SetStringField(
						TEXT("pin"), Pin->GetArgumentName().ToString());
					TSharedRef<FJsonObject> To = MakeShared<FJsonObject>();
					To->SetStringField(
						TEXT("node"),
						NodeId(TargetPin->GetNodePtr()->GetId().NodeIdx()));
					To->SetStringField(
						TEXT("pin"), TargetPin->GetArgumentName().ToString());
					TSharedRef<FJsonObject> EdgeState = MakeShared<FJsonObject>();
					EdgeState->SetObjectField(TEXT("from"), From);
					EdgeState->SetObjectField(TEXT("to"), To);
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

		TArray<TSharedPtr<FJsonValue>> ReadMeshComponentGeometry(AActor& Actor)
		{
			TArray<UActorComponent*> Components;
			Actor.GetComponents(Components);
			Components.Sort([](const UActorComponent& A, const UActorComponent& B) { return A.GetName() < B.GetName(); });
			TArray<TSharedPtr<FJsonValue>> Rows;
			auto Vector = [](const FVector& V) {
				return TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z)};
			};
			auto TransformValue = [&Vector](const FTransform& Transform) {
				auto Value = MakeShared<FJsonObject>();
				Value->SetArrayField(TEXT("translation"), Vector(Transform.GetLocation()));
				const FRotator R = Transform.Rotator();
				Value->SetArrayField(TEXT("rotation"), Vector(FVector(R.Pitch, R.Yaw, R.Roll)));
				Value->SetArrayField(TEXT("scale"), Vector(Transform.GetScale3D()));
				return Value;
			};
			for (UActorComponent* Component : Components)
			{
				if (!IsValid(Component)) continue;
				const auto* Instances = Cast<UInstancedStaticMeshComponent>(Component);
				const auto* Spline = Cast<USplineMeshComponent>(Component);
				if (!Instances && !Spline) continue;
				auto Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), Component->GetName());
				Row->SetStringField(TEXT("type"), Component->GetClass()->GetPathName());
				Row->SetStringField(TEXT("space"), TEXT("world"));
				Row->SetStringField(TEXT("unit"), TEXT("centimeter"));
				const UStaticMeshComponent* Mesh = Instances ? static_cast<const UStaticMeshComponent*>(Instances) : Spline;
				if (Mesh->GetStaticMesh()) Row->SetStringField(TEXT("mesh"), Mesh->GetStaticMesh()->GetPathName());
				if (Instances) {
					TArray<TSharedPtr<FJsonValue>> Values;
					for (int32 Index = 0; Index < Instances->GetInstanceCount(); ++Index) {
						FTransform Transform;
						if (!Instances->GetInstanceTransform(Index, Transform, true)) continue;
						auto Value = TransformValue(Transform);
						Value->SetNumberField(TEXT("index"), Index);
						Values.Add(MakeShared<FJsonValueObject>(Value));
					}
					Row->SetArrayField(TEXT("instances"), MoveTemp(Values));
				}
				if (Spline) {
					const FTransform Transform = Spline->GetComponentTransform();
					Row->SetObjectField(TEXT("world_transform"), TransformValue(Transform));
					Row->SetArrayField(TEXT("start"), Vector(Transform.TransformPosition(Spline->GetStartPosition())));
					Row->SetArrayField(TEXT("end"), Vector(Transform.TransformPosition(Spline->GetEndPosition())));
					Row->SetArrayField(TEXT("start_tangent"), Vector(Transform.TransformVector(Spline->GetStartTangent())));
					Row->SetArrayField(TEXT("end_tangent"), Vector(Transform.TransformVector(Spline->GetEndTangent())));
					const FVector2D StartOffset = Spline->GetStartOffset(), EndOffset = Spline->GetEndOffset();
					auto Offsets = MakeShared<FJsonObject>();
					Offsets->SetStringField(TEXT("space"), TEXT("spline_cross_section"));
					Offsets->SetArrayField(TEXT("start"), {MakeShared<FJsonValueNumber>(StartOffset.X), MakeShared<FJsonValueNumber>(StartOffset.Y)});
					Offsets->SetArrayField(TEXT("end"), {MakeShared<FJsonValueNumber>(EndOffset.X), MakeShared<FJsonValueNumber>(EndOffset.Y)});
					Row->SetObjectField(TEXT("offsets"), Offsets);
				}
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			return Rows;
		}

		bool AppendLevelProperties(const FUeAgentCanonicalSnapshot& Snapshot,
			FUeAgentContentGraphCapture& OutCapture, FUeAgentContentGraphReadError& OutError)
		{
			using namespace UeAgentLevelAuthoring;
			TSet<FString> ActorOwners;
			TMap<FString, TSet<FString>> ComponentOwners;
			for (const auto& Entry : Snapshot.GetEntries()) {
				const auto& Identity = Entry.GetIdentity();
				if (Identity.GetSemanticId() == ActorPropertySemanticId) ActorOwners.Add(Identity.GetSelector(TEXT("actor_guid")));
				else if (Identity.GetSemanticId() == ComponentPropertySemanticId)
					ComponentOwners.FindOrAdd(Identity.GetSelector(TEXT("owner_actor_guid"))).Add(Identity.GetSelector(TEXT("component_name")));
			}
			if (ActorOwners.IsEmpty() && ComponentOwners.IsEmpty()) return true;
			TMap<FString, FString> ActorIds;
			TMap<FString, TMap<FString, FString>> ComponentIds;
			for (const auto& Item : OutCapture.Items) {
				if (Item.ItemKind == TEXT("actor")) {
					const FString Guid = Item.SemanticIdentity.Selectors.FindRef(TEXT("actor_guid"));
					if (ActorOwners.Contains(Guid)) ActorIds.Add(Guid, Item.ItemId);
				} else if (Item.ItemKind == TEXT("component")) {
					const FString Guid = Item.SemanticIdentity.Selectors.FindRef(TEXT("owner_actor_guid"));
					const FString Name = Item.SemanticIdentity.Selectors.FindRef(TEXT("component_name"));
					const auto* Needed = ComponentOwners.Find(Guid);
					if (Needed && Needed->Contains(Name)) ComponentIds.FindOrAdd(Guid).Add(Name, Item.ItemId);
				}
			}
			for (const auto& Entry : Snapshot.GetEntries()) {
				const auto& Identity = Entry.GetIdentity();
				const FString& SemanticId = Identity.GetSemanticId();
				if (SemanticId != ActorPropertySemanticId && SemanticId != ComponentPropertySemanticId) continue;
				const bool bActorProperty = SemanticId == ActorPropertySemanticId;
				const FString ActorGuid = Identity.GetSelector(bActorProperty ? TEXT("actor_guid") : TEXT("owner_actor_guid"));
				const auto* Components = ComponentIds.Find(ActorGuid);
				const FString* OwnerId = bActorProperty ? ActorIds.Find(ActorGuid)
					: (Components ? Components->Find(Identity.GetSelector(TEXT("component_name"))) : nullptr);
				if (!OwnerId) return Fail(OutError, CaptureFact(TEXT("level_read_property_owner_missing"), TEXT("Captured property has no matching actor or component owner."), TEXT("/actors"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Identity.GetCanonicalKey()));
				FUeAgentContentGraphProperty Property;
				Property.OwnerId = *OwnerId; Property.FieldId = Identity.GetSelector(TEXT("property_path"));
				Property.SemanticIdentity = SemanticIdentity(SemanticId, Identity.GetSelectors(), TEXT("property"));
				Property.State.AuthoringDescriptor = Entry.GetAuthoringDescriptor(); Property.State.AuthoringDetails = Entry.GetDetails();
				Property.State.Value = Entry.GetValue(); Property.State.Type = Entry.GetType(); Property.State.Default = Entry.GetDefault();
				Property.State.AuthoredPresence = Entry.GetAuthoredPresence(); Property.State.OverrideState = Entry.GetOverrideState();
				OutCapture.Properties.Add(MoveTemp(Property));
			}
			return true;
		}

		bool CaptureLevel(UWorld& World, FUeAgentContentGraphCapture& OutCapture,
			FUeAgentContentGraphReadError& OutError, const TArray<UeAgentLevelAuthoring::FLevelActorCaptureScope>* Scope = nullptr,
			bool bIncludeScopedGeometry = false, bool bIncludeAuthoringMetadata = true)
		{
			using namespace UeAgentLevelAuthoring;
			FLevelAuthoringCapture Capture;
			UE::UAI::Contract::FDiagnostic Error;
			if (!UeAgentLevelAuthoring::RegisterAuthoring(Error)
				|| !(Scope ? TryCaptureLevelScopedNativeState(World, *Scope, Capture, Error, TEXT("llm_authoring"))
					: TryCaptureLevelNativeState(World, Capture, Error, TEXT("llm_authoring"))))
				return Fail(OutError, Error, World.GetPathName());
			auto Resources = MakeSemanticWorkspaceResourceTracker();
			FWorkspaceError ProjectionError;
			if (!TryBuildCanonicalAuthoringSnapshot(TEXT("ue.level.asset"), TEXT("llm_authoring"), Capture.Snapshot,
				Resources, Capture.CanonicalSnapshot, ProjectionError, bIncludeAuthoringMetadata && Scope == nullptr, Scope == nullptr)) return Fail(OutError, ProjectionError.GetDiagnostic(), World.GetPathName());
			OutCapture.CanonicalRevision = Capture.CanonicalSnapshot->GetRevision();
			OutCapture.Revision = Capture.CanonicalSnapshot->GetRevision().GetGeneration();
			const FString RootId = TEXT("level.actors:") + World.GetPathName();
			TMap<FString, FString> ActorNames, ActorIds;
			// The original composition owner allocates the same namespace, but
			// no complete Model/graph/JSON aggregate is produced just for two fields.
			if (!TryVisitSnapshotKeyedMemberNames(TEXT("ue.level.asset"), *Capture.CanonicalSnapshot, ActorStateSemanticId, TEXT("actors"),
				[&](const FString& Alias, const FString&, const FString& Guid, const FString&, const FString&) { ActorNames.Add(Guid, Alias); },
				ProjectionError)) return Fail(OutError, ProjectionError.GetDiagnostic(), World.GetPathName());
			for (const auto& Entry : Capture.CanonicalSnapshot->GetEntries())
			{
				const auto& Identity = Entry.GetIdentity();
				if (Identity.GetSemanticId() == ActorStateSemanticId)
					ActorIds.Add(Identity.GetSelector(TEXT("actor_guid")), Identity.GetCanonicalKey());
			}
			for (const auto& Entry : Capture.CanonicalSnapshot->GetEntries())
			{
				const auto& Identity = Entry.GetIdentity();
				const FString& SemanticId = Identity.GetSemanticId();
				const bool bActor = SemanticId == ActorStateSemanticId;
				const bool bComponent = SemanticId == ComponentStateSemanticId;
				if (SemanticId == WorldTargetSemanticId || bActor || bComponent)
				{
					if (Entry.GetValue()->GetKind() != EUeAgentValueKind::Object)
						return Fail(OutError, CaptureFact(TEXT("level_read_state_not_object"), TEXT("Native Level state must be an object."),
							TEXT("/state"), nullptr, MakeShared<FJsonValueString>(TEXT("object")), Identity.GetCanonicalKey()));
					auto Fields = Entry.GetValue()->GetObjectItems();
					FUeAgentStateSemanticError StateError;
					const auto SetValue = [&](const FString& Key, FUeAgentCanonicalValueRef Value) {
						if (auto* Existing = Fields.FindByPredicate([&](const auto& Field) { return Field.Key == Key; })) Existing->Value = MoveTemp(Value);
						else Fields.Emplace(Key, MoveTemp(Value));
					};
					const auto SetText = [&](const FString& Key, const FString& Text) {
						FUeAgentCanonicalValuePtr Value;
						if (!FUeAgentCanonicalValue::TryString(Text, Resources, Value, StateError)) return false;
						SetValue(Key, Value.ToSharedRef()); return true;
					};
					if (SemanticId == WorldTargetSemanticId)
					{
						FUeAgentContentGraphContainer Root;
						Root.ContainerId = RootId; Root.ContainerKind = TEXT("actors"); Root.OwnerId = World.GetPathName();
						Root.SemanticIdentity = SemanticIdentity(SemanticId, Identity.GetSelectors(), TEXT("container"));
						if (!SetText(TEXT("name"), TEXT("actors"))
							|| !FUeAgentCanonicalValue::TryObject(MoveTemp(Fields), Resources, Root.State, StateError)) return Fail(OutError, MoveTemp(StateError));
						OutCapture.Containers.Add(MoveTemp(Root));
						continue;
					}
					const FString ActorGuid = Identity.GetSelector(bActor ? TEXT("actor_guid") : TEXT("owner_actor_guid"));
					const FString* ActorName = ActorNames.Find(ActorGuid);
					const FString* ActorId = ActorIds.Find(ActorGuid);
					if (!ActorName || !ActorId)
						return Fail(OutError, CaptureFact(TEXT("level_read_actor_projection_missing"), TEXT("Captured actor has no matching public name or canonical actor entry."), TEXT("/actors"), MakeShared<FJsonValueString>(!ActorName ? TEXT("public name missing") : TEXT("actor entry missing")), MakeShared<FJsonValueString>(TEXT("matching actor name and entry")), Identity.GetCanonicalKey()));
					const FString ComponentsId = TEXT("level.components:") + ActorGuid;
					const FString ComponentsName = TEXT("actors/") + *ActorName + TEXT("/components");
					FUeAgentContentGraphItem Item;
					Item.ItemId = Identity.GetCanonicalKey(); Item.ItemKind = bActor ? TEXT("actor") : TEXT("component");
					Item.OwnerId = bActor ? RootId : *ActorId; Item.ContainerId = bActor ? RootId : ComponentsId;
					Item.ClassPath = Identity.GetSelector(bActor ? TEXT("actor_class") : TEXT("component_class"));
					Item.Order = OutCapture.Items.Num();
					Item.SemanticIdentity = SemanticIdentity(SemanticId, Identity.GetSelectors(), TEXT("item"));
					const FString ItemName = bActor ? *ActorName : Identity.GetSelector(TEXT("component_name"));
					if (!SetText(TEXT("name"), ItemName) || !SetText(TEXT("type"), Item.ClassPath)
						|| !SetText(TEXT("kind"), Item.ItemKind) || !SetText(TEXT("container"), bActor ? TEXT("actors") : ComponentsName))
						return Fail(OutError, MoveTemp(StateError));
					if (bActor)
					{
						// Public lookup for the existing parent_managed_id attachment contract.
						// This reports ownership already present on the Actor; it never assigns it.
						if (!SetText(TEXT("object_ref"), Identity.GetSelector(TEXT("object_path")))
							|| !SetText(TEXT("managed_id"), Identity.GetSelector(TEXT("managed_id")))) return Fail(OutError, MoveTemp(StateError));
						if (AActor* Actor = (!Scope || bIncludeScopedGeometry) ? FindObject<AActor>(nullptr, *Identity.GetSelector(TEXT("object_path"))) : nullptr) {
							FUeAgentCanonicalValuePtr Geometry;
							if (!TryCanonicalValue(MakeShared<FJsonValueArray>(ReadMeshComponentGeometry(*Actor)), Resources, Geometry, StateError)) return Fail(OutError, MoveTemp(StateError));
							SetValue(TEXT("geometry_components"), Geometry.ToSharedRef());
						}
						FUeAgentContentGraphContainer Components;
						Components.ContainerId = ComponentsId; Components.ContainerKind = TEXT("components");
						Components.OwnerId = Item.ItemId; Components.ParentContainerId = RootId;
						Components.SemanticIdentity = SemanticIdentity(SemanticId, Identity.GetSelectors(), TEXT("container"));
						const auto ContainerState = MakeShared<FJsonObject>();
						ContainerState->SetStringField(TEXT("name"), ComponentsName);
						ContainerState->SetStringField(TEXT("actor"), *ActorName);
						if (!TryCanonicalObject(ContainerState, Resources, Components.State, OutError)) return false;
						OutCapture.Containers.Add(MoveTemp(Components));
					}
					else if (!SetText(TEXT("actor"), *ActorName)) return Fail(OutError, MoveTemp(StateError));
					if (!FUeAgentCanonicalValue::TryObject(MoveTemp(Fields), Resources, Item.State, StateError)) return Fail(OutError, MoveTemp(StateError));
					// Only full/compound/relation capture consumes ownership edges.
					// Single-facet topology already carries its required item owners.
					if (!Scope) {
					FUeAgentContentGraphRelation Relation;
					Relation.RelationId = TEXT("level.ownership:") + Item.ItemId;
					Relation.RelationKind = bActor ? TEXT("membership") : TEXT("parent");
					Relation.SourceItemId = bActor ? RootId : Item.ItemId;
					Relation.TargetItemId = bActor ? Item.ItemId : *ActorId;
					Relation.SemanticIdentity = SemanticIdentity(SemanticId, Identity.GetSelectors(), TEXT("relation"));
					const auto RelationState = MakeShared<FJsonObject>();
					RelationState->SetStringField(TEXT("kind"), Relation.RelationKind);
					RelationState->SetStringField(TEXT("source"), bActor ? TEXT("actors") : ItemName);
					RelationState->SetStringField(TEXT("target"), *ActorName);
					RelationState->SetStringField(TEXT("container"), bActor ? TEXT("actors") : ComponentsName);
					if (!TryCanonicalObject(RelationState, Resources, Relation.State, OutError)) return false;
					OutCapture.Relations.Add(MoveTemp(Relation));
					}
					OutCapture.Items.Add(MoveTemp(Item));
				}

			}
			return AppendLevelProperties(*Capture.CanonicalSnapshot, OutCapture, OutError);
		}

		bool CaptureSmartObject(USmartObjectDefinition& Definition, FUeAgentContentGraphCapture& OutCapture,
			FUeAgentContentGraphReadError& OutError)
		{
			const FString OwnerId = Definition.GetPathName(), ContainerId = TEXT("smart_object:definition");
			UE::UAI::Contract::FDiagnostic Error;
			if (!UeAgentReflectionAuthoring::RegisterAuthoring(Error)) return Fail(OutError, Error, OwnerId);
			const auto* Profile = UeAgentReflectionAuthoring::FindProfileByTypeId(TEXT("smart_object"));
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!Profile)
				return Fail(OutError, CaptureFact(TEXT("smart_object_authoring_profile_missing"),
					TEXT("No authoring profile is registered for smart_object capture."), TEXT("/profile"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			if (!UeAgentReflectionAuthoring::BuildAssetExportSnapshotReadRequests(*Profile, Definition, Requests, Error))
				return Fail(OutError, Error, OwnerId);
			if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
				return Fail(OutError, Capture.Diagnostic, OwnerId);
			auto Resources = MakeSemanticWorkspaceResourceTracker();
			FUeAgentCanonicalSnapshotPtr Snapshot;
			FAuthoringModelPtr Model;
			FWorkspaceError WorkspaceError;
			if (!TryBuildCanonicalAuthoringCapture(ResolveTargetCompositionKind(Profile->TargetKind, Profile->TypeId),
				TEXT("llm_authoring"), Capture.Snapshot, Resources, Snapshot, Model, WorkspaceError))
				return Fail(OutError, WorkspaceError.GetDiagnostic(), OwnerId);
			OutCapture.CanonicalRevision = Snapshot->GetRevision(); OutCapture.Revision = Snapshot->GetRevision().GetGeneration();
			FKeyedItemProjectionContract Contract;
			if (!TryGetKeyedItemProjectionContract(TEXT("smart_object_items"), Contract)) return Fail(OutError, CaptureFact(TEXT("smart_object_projection_contract_missing"), TEXT("No keyed-item projection contract is registered for smart_object_items."), TEXT("/smart_object_items"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			TSet<FString> ItemIds;
			for (const auto& Unit : Model->GetUnits())
			{
				if (Unit.GetSemanticTypeRef() != UeAgentSmartObjectAuthoring::DefinitionSemanticId) continue;
				const auto* Source = Unit.GetValues().Find(TEXT("smart_object_items"));
				FUeAgentCanonicalValuePtr Visible;
				TSharedPtr<FJsonValue> VisibleJson, SourceJson;
				if (!Source)
					return Fail(OutError, CaptureFact(TEXT("smart_object_read_projection_failed"),
						TEXT("Captured smart_object document is missing smart_object_items."), TEXT("/smart_object_items"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Unit.GetUnitId()));
				if (!TryProjectKeyedItemVisibleValue(Unit.GetUnitId(), Source->Get(), Contract, Resources, Visible, WorkspaceError))
					return Fail(OutError, WorkspaceError.GetDiagnostic(), OwnerId);
				if (!CanonicalJson::TryToJsonValue(*Visible, VisibleJson, Error))
					return Fail(OutError, Error, OwnerId);
				if (!CanonicalJson::TryToJsonValue(Source->Get(), SourceJson, Error))
					return Fail(OutError, Error, OwnerId);
				FUeAgentContentGraphContainer Container;
				Container.ContainerId = ContainerId; Container.ContainerKind = TEXT("smart_object"); Container.OwnerId = OwnerId;
				Container.SemanticIdentity = SemanticIdentity(UeAgentSmartObjectAuthoring::DefinitionSemanticId,
					{{TEXT("asset_path"), OwnerId}, {TEXT("graph_name"), TEXT("main")}}, TEXT("graph"));
				const auto RootState = MakeShared<FJsonObject>(); RootState->SetStringField(TEXT("name"), Definition.GetName());
				if (!TryCanonicalObject(RootState, Resources, Container.State, OutError)) return false;
				OutCapture.Containers.Add(MoveTemp(Container));
				const auto Rows = VisibleJson->AsObject(), Sources = SourceJson->AsObject();
				TMap<FString, FString> IdByName;
				for (const auto& Row : Rows->Values) IdByName.Add(Row.Key, Sources->GetObjectField(Row.Key)->GetStringField(TEXT("id")));
				for (const auto& Row : Rows->Values)
				{
					const auto State = Row.Value->AsObject().ToSharedRef();
					const auto Native = Sources->GetObjectField(Row.Key);
					const FString Id = IdByName.FindChecked(Row.Key);
					FString Parent; State->TryGetStringField(TEXT("parent"), Parent);
					FUeAgentContentGraphItem Item;
					Item.ItemId = Id; Item.OwnerId = Parent.IsEmpty() ? ContainerId : IdByName.FindRef(Parent); Item.ContainerId = ContainerId;
					Item.ItemKind = TEXT("smart_object_") + Native->GetStringField(TEXT("kind"));
					Native->TryGetStringField(TEXT("class_path"), Item.ClassPath); Item.Order = Native->GetIntegerField(TEXT("index"));
					Item.SemanticIdentity = SemanticIdentity(UeAgentSmartObjectAuthoring::ItemSemanticId,
						{{TEXT("asset_path"), OwnerId}, {TEXT("graph_name"), TEXT("main")}, {TEXT("node_id"), Id}, {TEXT("public_name"), Row.Key}}, TEXT("item"));
					State->SetStringField(TEXT("name"), Row.Key);
					if (!TryCanonicalObject(State, Resources, Item.State, OutError)) return false;
					ItemIds.Add(Id); OutCapture.Items.Add(MoveTemp(Item));
				}
			}
			for (const auto& Entry : Snapshot->GetEntries())
			{
				const auto& Identity = Entry.GetIdentity();
				if (Identity.GetSemanticId() != UeAgentSmartObjectAuthoring::ItemPropertySemanticId) continue;
				const FString Id = Identity.GetSelector(TEXT("node_id"));
				if (!ItemIds.Contains(Id)) return Fail(OutError, CaptureFact(TEXT("smart_object_read_property_owner_missing"), TEXT("SmartObject property refers to an item absent from the captured definition."), Identity.GetSelector(TEXT("property_path")), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Id));
				FUeAgentContentGraphProperty Property;
				Property.OwnerId = Id; Property.FieldId = Identity.GetSelector(TEXT("property_path"));
				Property.State.Value = Entry.GetValue(); Property.State.Type = Entry.GetType(); Property.State.Default = Entry.GetDefault();
				Property.State.AuthoredPresence = Entry.GetAuthoredPresence(); Property.State.OverrideState = Entry.GetOverrideState();
				Property.SemanticIdentity = SemanticIdentity(Identity.GetSemanticId(),
					{{TEXT("asset_path"), OwnerId}, {TEXT("graph_name"), TEXT("main")}, {TEXT("node_id"), Id}, {TEXT("property_path"), Property.FieldId}}, TEXT("property"));
				OutCapture.Properties.Add(MoveTemp(Property));
			}
			return true;
		}

		bool CaptureBlackboard(UBlackboardData& Blackboard, FUeAgentContentGraphCapture& OutCapture,
			FUeAgentContentGraphReadError& OutError)
		{
			const FString OwnerId = Blackboard.GetPathName(), ContainerId = TEXT("blackboard:keys");
			UE::UAI::Contract::FDiagnostic Error;
			if (!UeAgentReflectionAuthoring::RegisterAuthoring(Error)) return Fail(OutError, Error, OwnerId);
			const auto* Profile = UeAgentReflectionAuthoring::FindProfileByTypeId(TEXT("blackboard"));
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!Profile)
				return Fail(OutError, CaptureFact(TEXT("blackboard_authoring_profile_missing"),
					TEXT("No authoring profile is registered for blackboard capture."), TEXT("/profile"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			if (!UeAgentReflectionAuthoring::BuildAssetExportSnapshotReadRequests(*Profile, Blackboard, Requests, Error))
				return Fail(OutError, Error, OwnerId);
			if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
				return Fail(OutError, Capture.Diagnostic, OwnerId);
			auto Resources = MakeSemanticWorkspaceResourceTracker();
			FUeAgentCanonicalSnapshotPtr Snapshot;
			FAuthoringModelPtr Model;
			FWorkspaceError WorkspaceError;
			if (!TryBuildCanonicalAuthoringCapture(ResolveTargetCompositionKind(Profile->TargetKind, Profile->TypeId),
				TEXT("llm_authoring"), Capture.Snapshot, Resources, Snapshot, Model, WorkspaceError))
				return Fail(OutError, WorkspaceError.GetDiagnostic(), OwnerId);
			OutCapture.CanonicalRevision = Snapshot->GetRevision();
			OutCapture.Revision = Snapshot->GetRevision().GetGeneration();
			FKeyedItemProjectionContract Contract;
			if (!TryGetKeyedItemProjectionContract(TEXT("blackboard_keys"), Contract)) return Fail(OutError, CaptureFact(TEXT("blackboard_projection_contract_missing"), TEXT("No keyed-item projection contract is registered for blackboard_keys."), TEXT("/blackboard_keys"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			TMap<FString, FString> ItemIds;
			for (const auto& Unit : Model->GetUnits())
			{
				if (Unit.GetSemanticTypeRef() != UeAgentBlackboardAuthoring::DocumentSemanticId) continue;
				const auto* Source = Unit.GetValues().Find(TEXT("blackboard_keys"));
				FUeAgentCanonicalValuePtr Visible;
				TSharedPtr<FJsonValue> VisibleJson;
				if (!Source)
					return Fail(OutError, CaptureFact(TEXT("blackboard_read_projection_failed"),
						TEXT("Captured blackboard document is missing blackboard_keys."), TEXT("/blackboard_keys"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Unit.GetUnitId()));
				if (!TryProjectKeyedItemVisibleValue(Unit.GetUnitId(), Source->Get(), Contract, Resources, Visible, WorkspaceError))
					return Fail(OutError, WorkspaceError.GetDiagnostic(), OwnerId);
				if (!CanonicalJson::TryToJsonValue(*Visible, VisibleJson, Error))
					return Fail(OutError, Error, OwnerId);
				FUeAgentContentGraphContainer Container;
				Container.ContainerId = ContainerId; Container.ContainerKind = TEXT("blackboard"); Container.OwnerId = OwnerId;
				Container.SemanticIdentity = SemanticIdentity(UeAgentBlackboardAuthoring::DocumentSemanticId,
					{{TEXT("asset_path"), OwnerId}}, TEXT("blackboard.document"));
				const auto RootState = MakeShared<FJsonObject>();
				RootState->SetStringField(TEXT("name"), Blackboard.GetName());
				for (const auto& Field : Unit.GetValues())
					if (Field.Key != TEXT("blackboard_keys"))
					{
						TSharedPtr<FJsonValue> Value;
						if (!CanonicalJson::TryToJsonValue(Field.Value.Get(), Value, Error)) return Fail(OutError, Error);
						RootState->SetField(Field.Key, Value);
					}
				if (!TryCanonicalObject(RootState, Resources, Container.State, OutError)) return false;
				OutCapture.Containers.Add(MoveTemp(Container));
				int32 Index = 0;
				for (const FBlackboardEntry& Entry : Blackboard.Keys)
				{
					const FString Name = Entry.EntryName.ToString();
					const TSharedPtr<FJsonObject>* State = nullptr;
					if (!VisibleJson->AsObject()->TryGetObjectField(Name, State)) return Fail(OutError, CaptureFact(TEXT("blackboard_read_key_missing"), TEXT("Blackboard key has no projected key state."), TEXT("/keys"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Name));
					FUeAgentContentGraphItem Item;
					Item.ItemId = TEXT("item:blackboard_entry:") + Name + TEXT(":") + Entry.KeyType->GetClass()->GetPathName() + TEXT(":local");
					Item.ItemKind = TEXT("blackboard_entry"); Item.OwnerId = OwnerId; Item.ContainerId = ContainerId;
					Item.ClassPath = Entry.KeyType->GetClass()->GetPathName(); Item.Order = Index++;
					Item.SemanticIdentity = SemanticIdentity(UeAgentBlackboardAuthoring::KeySemanticId,
						{{TEXT("asset_path"), OwnerId}, {TEXT("key_name"), Name}}, TEXT("blackboard.key"));
					(*State)->SetStringField(TEXT("name"), Name);
					if (!TryCanonicalObject((*State).ToSharedRef(), Resources, Item.State, OutError)) return false;
					ItemIds.Add(Name, Item.ItemId); OutCapture.Items.Add(MoveTemp(Item));
				}
#if WITH_EDITORONLY_DATA
				for (const FBlackboardEntry& Entry : Blackboard.ParentKeys)
				{
					if (!Entry.KeyType) continue;
					FUeAgentContentGraphItem Item;
					const FString Name = Entry.EntryName.ToString();
					Item.ItemId = TEXT("item:blackboard_entry:") + Name + TEXT(":") + Entry.KeyType->GetClass()->GetPathName() + TEXT(":inherited");
					Item.ItemKind = TEXT("blackboard_entry"); Item.OwnerId = OwnerId; Item.ContainerId = ContainerId;
					Item.ClassPath = Entry.KeyType->GetClass()->GetPathName(); Item.Order = Index++;
					Item.SemanticIdentity = SemanticIdentity(TEXT("uai.semantic.blackboard.inherited_key"),
						{{TEXT("asset_path"), OwnerId}, {TEXT("key_name"), Name}}, TEXT("item"));
					const auto* InheritedState = RootState->GetArrayField(TEXT("inherited_keys")).FindByPredicate(
						[&Name](const auto& Value) { return Value->AsObject()->GetStringField(TEXT("name")) == Name; });
					if (!InheritedState)
						return Fail(OutError, CaptureFact(TEXT("blackboard_read_inherited_key_missing"), TEXT("Inherited Blackboard key has no captured state."), TEXT("/inherited_keys/") + Name, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
					if (!TryCanonicalObject((*InheritedState)->AsObject().ToSharedRef(), Resources, Item.State, OutError)) return false;
					OutCapture.Items.Add(MoveTemp(Item));
				}
#endif
			}
			for (const auto& Entry : Snapshot->GetEntries())
			{
				const auto& Identity = Entry.GetIdentity();
				if (Identity.GetSemanticId() != UeAgentBlackboardAuthoring::KeyPropertySemanticId) continue;
				const FString Name = Identity.GetSelector(TEXT("key_name"));
				const FString* Id = ItemIds.Find(Name);
				if (!Id) return Fail(OutError, CaptureFact(TEXT("blackboard_read_property_owner_missing"), TEXT("Blackboard property has no captured local key owner."), TEXT("/keys/") + Name, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
				FUeAgentContentGraphProperty Property;
				Property.OwnerId = *Id; Property.FieldId = Identity.GetSelector(TEXT("property_path"));
				Property.State.Value = Entry.GetValue(); Property.State.Type = Entry.GetType();
				Property.State.Default = Entry.GetDefault();
				Property.State.AuthoredPresence = Entry.GetAuthoredPresence(); Property.State.OverrideState = Entry.GetOverrideState();
				Property.SemanticIdentity = SemanticIdentity(Identity.GetSemanticId(),
					{{TEXT("asset_path"), OwnerId}, {TEXT("key_name"), Name}, {TEXT("property_path"), Property.FieldId}}, TEXT("property"));
				OutCapture.Properties.Add(MoveTemp(Property));
			}
			return true;
		}

		bool CaptureStateTree(UStateTree& Tree, FUeAgentContentGraphCapture& OutCapture,
			FUeAgentContentGraphReadError& OutError)
		{
			const FString OwnerId = Tree.GetPathName(), TreeId = TEXT("state_tree:tree");
			UE::UAI::Contract::FDiagnostic Error;
			if (!UeAgentReflectionAuthoring::RegisterAuthoring(Error)) return Fail(OutError, Error, OwnerId);
			const auto* Profile = UeAgentReflectionAuthoring::FindProfileByTypeId(TEXT("state_tree"));
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!Profile)
				return Fail(OutError, CaptureFact(TEXT("state_tree_authoring_profile_missing"),
					TEXT("No authoring profile is registered for state_tree capture."), TEXT("/profile"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			if (!UeAgentReflectionAuthoring::BuildAssetExportSnapshotReadRequests(*Profile, Tree, Requests, Error))
				return Fail(OutError, Error, OwnerId);
			if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
				return Fail(OutError, Capture.Diagnostic, OwnerId);
			auto Resources = MakeSemanticWorkspaceResourceTracker();
			FUeAgentCanonicalSnapshotPtr Snapshot;
			FAuthoringModelPtr Model;
			FWorkspaceError WorkspaceError;
			if (!TryBuildCanonicalAuthoringCapture(ResolveTargetCompositionKind(Profile->TargetKind, Profile->TypeId),
				TEXT("llm_authoring"), Capture.Snapshot, Resources, Snapshot, Model, WorkspaceError))
				return Fail(OutError, WorkspaceError.GetDiagnostic(), OwnerId);
			OutCapture.CanonicalRevision = Snapshot->GetRevision(); OutCapture.Revision = TEXT("observed");
			FKeyedItemProjectionContract Contract;
			if (!TryGetKeyedItemProjectionContract(TEXT("tree_items"), Contract)) return Fail(OutError, CaptureFact(TEXT("state_tree_projection_contract_missing"), TEXT("No keyed-item projection contract is registered for tree_items."), TEXT("/tree_items"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			TSet<FString> ItemIds;
			int32 PropertyCount = 0;
			if (!AddProperties(Tree, Resources, OutCapture.Properties, PropertyCount, OutError)) return false;
			for (const auto& Unit : Model->GetUnits())
			{
				if (Unit.GetSemanticTypeRef() != UeAgentStateTreeAuthoring::TreeSemanticId) continue;
				const auto* Source = Unit.GetValues().Find(TEXT("tree_items"));
				FUeAgentCanonicalValuePtr Visible;
				TSharedPtr<FJsonValue> VisibleJson, SourceJson;
				if (!Source)
					return Fail(OutError, CaptureFact(TEXT("state_tree_read_projection_failed"),
						TEXT("Captured state_tree document is missing tree_items."), TEXT("/tree_items"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Unit.GetUnitId()));
				if (!TryProjectKeyedItemVisibleValue(Unit.GetUnitId(), Source->Get(), Contract, Resources, Visible, WorkspaceError))
					return Fail(OutError, WorkspaceError.GetDiagnostic(), OwnerId);
				if (!CanonicalJson::TryToJsonValue(*Visible, VisibleJson, Error))
					return Fail(OutError, Error, OwnerId);
				if (!CanonicalJson::TryToJsonValue(Source->Get(), SourceJson, Error))
					return Fail(OutError, Error, OwnerId);
				FUeAgentContentGraphContainer Container;
				Container.ContainerId = TreeId; Container.ContainerKind = TEXT("state_tree"); Container.OwnerId = OwnerId;
				Container.SemanticIdentity = SemanticIdentity(UeAgentStateTreeAuthoring::TreeSemanticId,
					{{TEXT("asset_path"), OwnerId}, {TEXT("graph_name"), TEXT("main")}}, TEXT("graph"));
				const auto TreeState = MakeShared<FJsonObject>();
				for (const auto& Field : Unit.GetValues())
					if (Field.Key != TEXT("tree_items"))
					{
						TSharedPtr<FJsonValue> Value;
						if (!CanonicalJson::TryToJsonValue(Field.Value.Get(), Value, Error)) return Fail(OutError, Error);
						TreeState->SetField(Field.Key, Value);
					}
				if (!TryCanonicalObject(TreeState, Resources, Container.State, OutError)) return false;
				OutCapture.Containers.Add(MoveTemp(Container));
				const auto Sources = SourceJson->AsObject(), Rows = VisibleJson->AsObject();
				TMap<FString, FString> IdByName;
				for (const auto& Row : Rows->Values) IdByName.Add(Row.Key, Sources->GetObjectField(Row.Key)->GetStringField(TEXT("id")));
				auto AddReference = [&](const FString& Id, const FString& Kind, const FString& From, const FString& To,
					const TSharedRef<FJsonObject>& State) {
					FUeAgentContentGraphRelation Relation;
					Relation.RelationId = Id; Relation.RelationKind = Kind;
					Relation.SourceItemId = From; Relation.TargetItemId = To;
					Relation.SemanticIdentity = SemanticIdentity(TEXT("uai.semantic.state_tree.reference"),
						{{TEXT("asset_path"), OwnerId}, {TEXT("identity"), Id}}, TEXT("relation"));
					if (!TryCanonicalObject(State, Resources, Relation.State, OutError)) return false;
					OutCapture.Relations.Add(MoveTemp(Relation)); return true;
				};
				for (const auto& Row : Rows->Values)
				{
					const auto State = Row.Value->AsObject().ToSharedRef();
					const auto Native = Sources->GetObjectField(Row.Key);
					State->SetStringField(TEXT("name"), Row.Key);
					const FString Id = IdByName.FindChecked(Row.Key);
					FString Parent; State->TryGetStringField(TEXT("parent"), Parent);
					FUeAgentContentGraphItem Item;
					Item.ItemId = Id; Item.ContainerId = TreeId; Item.OwnerId = Parent.IsEmpty() ? TreeId : IdByName.FindChecked(Parent);
					Item.ItemKind = TEXT("state_tree_") + Native->GetStringField(TEXT("kind"));
					Native->TryGetStringField(TEXT("class_path"), Item.ClassPath);
					FString NodeType;
					if (Native->TryGetStringField(TEXT("node_type"), NodeType) && !NodeType.IsEmpty()) Item.ClassPath = NodeType;
					Item.Order = Native->GetIntegerField(TEXT("index"));
					Item.SemanticIdentity = SemanticIdentity(UeAgentStateTreeAuthoring::ItemSemanticId,
						{{TEXT("asset_path"), OwnerId}, {TEXT("graph_name"), TEXT("main")}, {TEXT("node_id"), Id}, {TEXT("public_name"), Row.Key}}, TEXT("item"));
					if (!TryCanonicalObject(State, Resources, Item.State, OutError)) return false;
					ItemIds.Add(Id); OutCapture.Items.Add(MoveTemp(Item));
					if (!Parent.IsEmpty())
					{
						const auto Reference = MakeShared<FJsonObject>();
						Reference->SetStringField(TEXT("from"), Parent); Reference->SetStringField(TEXT("to"), Row.Key);
						if (!AddReference(TEXT("parent:") + Id, TEXT("state_tree_parent"), IdByName.FindChecked(Parent), Id, Reference)) return false;
					}
					for (const TCHAR* Field : {TEXT("target"), TEXT("linked_state")})
					{
						FString To, From = Row.Key;
						if (!State->TryGetStringField(Field, To) || To.IsEmpty()) continue;
						if (State->HasField(TEXT("source"))) From = State->GetStringField(TEXT("source"));
						if (!AddReference(FString(Field) + TEXT(":") + Id, TEXT("state_tree_reference"), IdByName.FindChecked(From), IdByName.FindChecked(To), State)) return false;
					}
				}
			}
			for (const auto& Entry : Snapshot->GetEntries())
			{
				const auto& Identity = Entry.GetIdentity();
				if (Identity.GetSemanticId() != UeAgentStateTreeAuthoring::ItemPropertySemanticId) continue;
				const FString Id = Identity.GetSelector(TEXT("node_id"));
				if (!ItemIds.Contains(Id)) return Fail(OutError, CaptureFact(TEXT("state_tree_read_property_owner_missing"), TEXT("StateTree property refers to an item absent from the captured tree."), Identity.GetSelector(TEXT("property_path")), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Id));
				FUeAgentContentGraphProperty Property;
				Property.OwnerId = Id; Property.FieldId = Identity.GetSelector(TEXT("property_path"));
				Property.State.Value = Entry.GetValue(); Property.State.Type = Entry.GetType();
				Property.State.Default = Entry.GetDefault();
				Property.State.AuthoredPresence = Entry.GetAuthoredPresence(); Property.State.OverrideState = Entry.GetOverrideState();
				Property.SemanticIdentity = SemanticIdentity(Identity.GetSemanticId(),
					{{TEXT("asset_path"), OwnerId}, {TEXT("node_id"), Id}, {TEXT("property_path"), Property.FieldId}}, TEXT("property"));
				OutCapture.Properties.Add(MoveTemp(Property));
			}
			return true;
		}

		struct FAiGraphQueryDomain
		{
			const TCHAR* ProfileId;
			const TCHAR* GraphSemanticId;
			const TCHAR* NodeSemanticId;
			const TCHAR* RelationSemanticId;
			const TCHAR* ContainerKind;
			const TCHAR* NodeKind;
			const TCHAR* ChildKind;
			const TCHAR* ConnectionKind;
			const TCHAR* AttachmentKind;
		};

		struct FAiGraphQueryNode
		{
			const UEdGraphNode* Node = nullptr;
			const UObject* PropertyObject = nullptr;
			FString QueryItemId;
		};

		using FAiGraphQueryBindings = TMap<FString, TMap<FString, FAiGraphQueryNode>>;

		// Both query and workspace use the canonical authoring graph projection.
		// Native bindings only supply property objects; they do not define public rows.
		bool CaptureProjectedAiGraphs(UObject& Owner, const FAiGraphQueryDomain& Domain,
			FAiGraphQueryBindings& NativeGraphs, FUeAgentContentGraphCapture& OutCapture,
			FUeAgentContentGraphReadError& OutError)
		{
			const FString OwnerId = Owner.GetPathName();
			UE::UAI::Contract::FDiagnostic Error;
			if (!UeAgentReflectionAuthoring::RegisterAuthoring(Error)) return Fail(OutError, Error, OwnerId);
			const auto* Profile = UeAgentReflectionAuthoring::FindProfileByTypeId(Domain.ProfileId);
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!Profile)
				return Fail(OutError, CaptureFact(TEXT("ai_graph_authoring_profile_missing"),
					TEXT("No authoring profile is registered for the graph capture."), TEXT("/profile"),
					MakeShared<FJsonValueString>(Domain.ProfileId), MakeShared<FJsonValueString>(TEXT("registered profile"))));
			if (!UeAgentReflectionAuthoring::BuildAssetExportSnapshotReadRequests(*Profile, Owner, Requests, Error))
				return Fail(OutError, Error, OwnerId);
			if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
				return Fail(OutError, Capture.Diagnostic, OwnerId);
			auto Resources = MakeSemanticWorkspaceResourceTracker();
			FUeAgentCanonicalSnapshotPtr Snapshot;
			FAuthoringModelPtr Model;
			FWorkspaceError WorkspaceError;
			if (!TryBuildCanonicalAuthoringCapture(ResolveTargetCompositionKind(Profile->TargetKind, Profile->TypeId),
				TEXT("llm_authoring"), Capture.Snapshot, Resources, Snapshot, Model, WorkspaceError))
				return Fail(OutError, WorkspaceError.GetDiagnostic(), OwnerId);
			OutCapture.CanonicalRevision = Snapshot->GetRevision();
			OutCapture.Revision = TEXT("observed");
			FSemanticReferenceProjection PublicReferences;
			if (!TryBuildSemanticReferenceProjection(*Model, PublicReferences, WorkspaceError))
				return Fail(OutError, WorkspaceError.GetDiagnostic(), OwnerId);
			TMap<FString, FString> ItemByDeclaration;
			for (const auto& Entry : Snapshot->GetEntries())
			{
				const auto& Identity = Entry.GetIdentity();
				if (Identity.GetSemanticId() != Domain.NodeSemanticId) continue;
				const FString& GraphName = Identity.GetSelector(TEXT("graph_name"));
				const FString& NodeId = Identity.GetSelector(TEXT("node_id"));
				const FString& Declaration = Identity.GetSelector(TEXT("item_id"));
				auto* Graph = NativeGraphs.Find(GraphName);
				FAiGraphQueryNode* Binding = Graph ? Graph->Find(NodeId) : nullptr;
				if (!Binding || Declaration.IsEmpty())
					return Fail(OutError, CaptureFact(TEXT("ai_graph_read_node_binding_missing"),
						TEXT("The captured node declaration must identify a node in its native graph."), TEXT("/nodes"),
						MakeShared<FJsonValueString>(Declaration), MakeShared<FJsonValueString>(TEXT("native graph node declaration")), NodeId));
				// Main graph item ids remain stable. Owned instances use their original
				// declaration because their native GUIDs are local to each bound graph.
				Binding->QueryItemId = GraphName == TEXT("main") ? NodeId : Declaration;
				ItemByDeclaration.Add(Declaration, Binding->QueryItemId);
			}
			TSet<FString> CapturedGraphs;
			FUeAgentResourceTracker PropertyResources = MakeCanonicalResources();
			int32 PropertyCount = 0;
			if (!AddProperties(Owner, PropertyResources, OutCapture.Properties, PropertyCount, OutError)) return false;
			for (const auto& Unit : Model->GetUnits())
			{
				if (Unit.GetSemanticTypeRef() != Domain.GraphSemanticId) continue;
				UeAgentAuthoringIdentity::FCanonicalIdentity GraphIdentity;
				if (!UeAgentAuthoringIdentity::ParseCanonicalKey(Unit.GetTargetIdentityKey(), GraphIdentity, Error))
					return Fail(OutError, Error, OwnerId);
				const FString GraphName = GraphIdentity.GetSelector(TEXT("graph_name"));
				const auto* NativeById = NativeGraphs.Find(GraphName);
				if (!NativeById || CapturedGraphs.Contains(GraphName))
					return Fail(OutError, CaptureFact(TEXT("ai_graph_read_graph_binding_invalid"),
						TEXT("The projected graph must have one matching native graph."), TEXT("/graph_name"),
						MakeShared<FJsonValueString>(GraphName), MakeShared<FJsonValueString>(TEXT("unique native graph name"))));
				CapturedGraphs.Add(GraphName);
				TSharedPtr<FJsonObject> Row;
				if (!TryBuildSemanticModelUnitRow(Unit, Row, WorkspaceError, &PublicReferences.PublicRefs))
					return Fail(OutError, WorkspaceError.GetDiagnostic(), OwnerId);
				const auto Values = Row->GetObjectField(TEXT("values"));
				auto Visible = MakeShared<FJsonObject>();
				TSharedPtr<FJsonObject> Interfaces;
				if (!TryProjectGraphAuthoringVisibleRow(Unit.GetUnitId(), Row.ToSharedRef(), Visible, Interfaces, WorkspaceError))
					return Fail(OutError, WorkspaceError.GetDiagnostic(), OwnerId);
				const FString MainGraphId = FString(Domain.ProfileId) + TEXT(":graph");
				const FString GraphId = GraphName == TEXT("main") ? MainGraphId : MainGraphId + TEXT(":") + GraphName;
				const FString OwnerDeclaration = GraphIdentity.GetSelector(TEXT("owner_item_id"));
				if (!OwnerDeclaration.IsEmpty() && !ItemByDeclaration.Contains(OwnerDeclaration))
					return Fail(OutError, CaptureFact(TEXT("ai_graph_read_owner_missing"),
						TEXT("The owned graph refers to an item absent from the captured authoring snapshot."), TEXT("/owner_item_id"),
						MakeShared<FJsonValueString>(OwnerDeclaration), MakeShared<FJsonValueString>(TEXT("captured item declaration"))));
				FUeAgentContentGraphContainer Container;
				Container.ContainerId = GraphId;
				Container.ContainerKind = Domain.ContainerKind;
				Container.OwnerId = OwnerDeclaration.IsEmpty() ? OwnerId : ItemByDeclaration.FindChecked(OwnerDeclaration);
				Container.ParentContainerId = OwnerDeclaration.IsEmpty() ? FString() : MainGraphId;
				Container.Order = OutCapture.Containers.Num();
				Container.SemanticIdentity = SemanticIdentity(Domain.GraphSemanticId,
					{{TEXT("asset_path"), OwnerId}, {TEXT("graph_name"), GraphName}}, TEXT("graph"));
				const auto GraphState = Visible->GetObjectField(TEXT("graph")).ToSharedRef();
				FString OwnerReference;
				if (Visible->TryGetStringField(TEXT("owner"), OwnerReference) && !OwnerReference.IsEmpty())
				{
					GraphState->SetStringField(TEXT("owner"), OwnerReference);
					FString Name;
					if (!GraphState->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
						GraphState->SetStringField(TEXT("name"), OwnerReference);
				}
				if (!TryCanonicalObject(GraphState, Resources, Container.State, OutError)) return false;
				OutCapture.Containers.Add(MoveTemp(Container));
				const auto Sources = Values->GetObjectField(TEXT("nodes"));
				const auto Rows = Visible->GetObjectField(TEXT("nodes"));
				TMap<FString, FString> IdByName;
				for (const auto& Entry : Rows->Values)
				{
					const TSharedPtr<FJsonObject>* Source = nullptr;
					FString Id;
					if (!Sources->TryGetObjectField(Entry.Key, Source))
						return Fail(OutError, CaptureFact(TEXT("ai_graph_read_node_binding_missing"), TEXT("Projected graph node has no captured source record."), TEXT("/nodes"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Entry.Key));
					if (!(*Source)->TryGetStringField(TEXT("id"), Id))
						return Fail(OutError, CaptureFact(TEXT("ai_graph_read_node_binding_missing"), TEXT("Captured graph node requires a string id."), TEXT("/nodes/") + Entry.Key + TEXT("/id"), (*Source)->TryGetField(TEXT("id")), MakeShared<FJsonValueString>(TEXT("string id"))));
					const FAiGraphQueryNode* Binding = NativeById->Find(Id);
					if (!Binding || Binding->QueryItemId.IsEmpty())
						return Fail(OutError, CaptureFact(TEXT("ai_graph_read_node_binding_missing"), TEXT("Projected graph node id has no matching native node."), TEXT("/nodes"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Id));
					IdByName.Add(Entry.Key, Id);
				}
				const auto QueryIdByName = [&](const FString& Name) -> const FString& {
					return NativeById->FindChecked(IdByName.FindChecked(Name)).QueryItemId;
				};
				auto AddRelationState = [&](const FString& Id, const FString& Kind, const FString& FromId,
					const FString& ToId, const TSharedRef<FJsonObject>& State) {
					FUeAgentContentGraphRelation Relation;
					Relation.RelationId = Id; Relation.RelationKind = Kind;
					Relation.SourceItemId = FromId; Relation.TargetItemId = ToId;
					Relation.SemanticIdentity = SemanticIdentity(Domain.RelationSemanticId,
						{{TEXT("asset_path"), OwnerId}, {TEXT("graph_name"), GraphName}, {TEXT("identity"), Id}}, TEXT("relation"));
					if (!TryCanonicalObject(State, Resources, Relation.State, OutError)) return false;
					OutCapture.Relations.Add(MoveTemp(Relation)); return true;
				};
				for (const auto& Entry : Rows->Values)
				{
					const FString Id = IdByName.FindChecked(Entry.Key);
					const auto State = Entry.Value->AsObject().ToSharedRef();
					State->SetStringField(TEXT("name"), Entry.Key);
					if (Interfaces)
					{
						const TSharedPtr<FJsonObject>* Interface = nullptr;
						if (Interfaces->GetObjectField(TEXT("nodes"))->TryGetObjectField(Entry.Key, Interface)) State->SetObjectField(TEXT("pin_interface"), *Interface);
					}
					FString Parent;
					State->TryGetStringField(TEXT("parent"), Parent);
					if (!Parent.IsEmpty() && !IdByName.Contains(Parent))
						return Fail(OutError, CaptureFact(TEXT("ai_graph_read_parent_missing"), TEXT("The node parent is absent from its projected graph."),
							TEXT("/nodes/") + Entry.Key + TEXT("/parent"), MakeShared<FJsonValueString>(Parent), MakeShared<FJsonValueString>(TEXT("existing node name"))));
					const auto& Binding = NativeById->FindChecked(Id);
					const UObject* Payload = Binding.PropertyObject;
					FUeAgentContentGraphItem Item;
					Item.ItemId = Binding.QueryItemId; Item.ContainerId = GraphId;
					Item.OwnerId = Parent.IsEmpty() ? GraphId : QueryIdByName(Parent);
					Item.ItemKind = Parent.IsEmpty() ? Domain.NodeKind : Domain.ChildKind;
					Item.ClassPath = (Payload ? Payload : Binding.Node)->GetClass()->GetPathName();
					Item.Order = OutCapture.Items.Num();
					Item.SemanticIdentity = SemanticIdentity(Domain.NodeSemanticId,
						{{TEXT("asset_path"), OwnerId}, {TEXT("graph_name"), GraphName}, {TEXT("public_name"), Entry.Key}, {TEXT("node_id"), Id}}, TEXT("item"));
					if (!TryCanonicalObject(State, Resources, Item.State, OutError)) return false;
					OutCapture.Items.Add(MoveTemp(Item));
					if (Payload)
					{
						const int32 FirstProperty = OutCapture.Properties.Num();
						if (!AddProperties(*const_cast<UObject*>(Payload), PropertyResources, OutCapture.Properties, PropertyCount, OutError)) return false;
						for (int32 Index = FirstProperty; Index < OutCapture.Properties.Num(); ++Index) OutCapture.Properties[Index].OwnerId = Binding.QueryItemId;
					}
					if (!Parent.IsEmpty())
					{
						const auto Relation = MakeShared<FJsonObject>();
						Relation->SetStringField(TEXT("from"), Parent); Relation->SetStringField(TEXT("to"), Entry.Key);
						if (!AddRelationState(TEXT("attachment:") + Binding.QueryItemId, Domain.AttachmentKind, QueryIdByName(Parent), Binding.QueryItemId, Relation)) return false;
					}
				}
				for (const auto& Edge : Visible->GetArrayField(TEXT("connections")))
				{
					const auto State = Edge->AsObject().ToSharedRef();
					FString From = State->GetStringField(TEXT("from")), To = State->GetStringField(TEXT("to"));
					int32 Separator;
					if (!From.FindLastChar(TEXT('.'), Separator)) return Fail(OutError, CaptureFact(TEXT("ai_graph_read_endpoint_invalid"), TEXT("Graph connection endpoint must contain a node and pin separated by a dot."), TEXT("/connections/from"), MakeShared<FJsonValueString>(From), MakeShared<FJsonValueString>(TEXT("node.pin"))));
					From.LeftInline(Separator);
					if (!To.FindLastChar(TEXT('.'), Separator)) return Fail(OutError, CaptureFact(TEXT("ai_graph_read_endpoint_invalid"), TEXT("Graph connection endpoint must contain a node and pin separated by a dot."), TEXT("/connections/to"), MakeShared<FJsonValueString>(To), MakeShared<FJsonValueString>(TEXT("node.pin"))));
					To.LeftInline(Separator);
					if (!IdByName.Contains(From) || !IdByName.Contains(To)) return Fail(OutError, CaptureFact(TEXT("ai_graph_read_endpoint_missing"), TEXT("Graph connection references a node absent from the projected graph."), TEXT("/connections"), MakeShared<FJsonValueString>(!IdByName.Contains(From) ? From : To), MakeShared<FJsonValueString>(TEXT("existing node name"))));
					if (!AddRelationState(TEXT("edge:") + FString::FromInt(OutCapture.Relations.Num()), Domain.ConnectionKind,
						QueryIdByName(From), QueryIdByName(To), State)) return false;
				}
				if (IdByName.Num() != NativeById->Num())
					return Fail(OutError, CaptureFact(TEXT("ai_graph_read_nodes_incomplete"), TEXT("Graph capture does not cover all native nodes."),
						TEXT("/nodes"), MakeShared<FJsonValueNumber>(IdByName.Num()), MakeShared<FJsonValueNumber>(NativeById->Num()), GraphName));
			}
			return CapturedGraphs.Num() == NativeGraphs.Num() || Fail(OutError, CaptureFact(TEXT("ai_graph_read_graphs_incomplete"),
				TEXT("Graph capture does not cover all native graphs."), TEXT("/graphs"),
				MakeShared<FJsonValueNumber>(CapturedGraphs.Num()), MakeShared<FJsonValueNumber>(NativeGraphs.Num())));
		}

		bool CaptureEqs(UEnvQuery& Query, FUeAgentContentGraphCapture& OutCapture, FUeAgentContentGraphReadError& OutError)
		{
			UeAgentEqsAuthoring::FGraphCapture Native;
			UE::UAI::Contract::FDiagnostic Error;
			if (!UeAgentEqsAuthoring::CaptureGraph(Query, Native, Error)) return Fail(OutError, Error, Query.GetPathName());
			FAiGraphQueryBindings Bindings;
			auto& Nodes = Bindings.Add(TEXT("main"));
			for (const auto& Node : Native.Nodes)
				Nodes.Add(Node.Id, {Node.NativeNode, UeAgentEqsAuthoring::GetNodePropertyObject(*Node.NativeNode)});
			const FAiGraphQueryDomain Domain{TEXT("eqs"), UeAgentEqsAuthoring::GraphSemanticId,
				UeAgentEqsAuthoring::NodeSemanticId, TEXT("uai.semantic.eqs.relation"), TEXT("eqs_graph"),
				TEXT("eqs_node"), TEXT("eqs_test"), TEXT("eqs_connection"), TEXT("eqs_test_attachment")};
			return CaptureProjectedAiGraphs(Query, Domain, Bindings, OutCapture, OutError);
		}

		bool CaptureBehaviorTree(UBehaviorTree& Tree, FUeAgentContentGraphCapture& OutCapture, FUeAgentContentGraphReadError& OutError)
		{
			UeAgentBehaviorTreeAuthoring::FGraphCapture Native;
			UE::UAI::Contract::FDiagnostic Error;
			if (!UeAgentBehaviorTreeAuthoring::CaptureGraph(Tree, Native, Error)) return Fail(OutError, Error, Tree.GetPathName());
			FAiGraphQueryBindings Bindings;
			auto AddGraph = [&](const FString& Name, const UeAgentBehaviorTreeAuthoring::FGraphContents& Contents) {
				auto& Nodes = Bindings.Add(Name);
				for (const auto& Node : Contents.Nodes)
					Nodes.Add(Node.Id, {Node.NativeNode, UeAgentBehaviorTreeAuthoring::GetNodePropertyObject(*Node.NativeNode)});
			};
			AddGraph(TEXT("main"), Native);
			for (const auto& Graph : Native.OwnedGraphs) AddGraph(Graph.Id, Graph);
			const FAiGraphQueryDomain Domain{TEXT("behavior_tree"), UeAgentBehaviorTreeAuthoring::GraphSemanticId,
				UeAgentBehaviorTreeAuthoring::NodeSemanticId, TEXT("uai.semantic.ai_behavior.relation"), TEXT("behavior_tree_graph"),
				TEXT("behavior_node"), TEXT("behavior_subnode"), TEXT("connection"), TEXT("parent")};
			if (!CaptureProjectedAiGraphs(Tree, Domain, Bindings, OutCapture, OutError)) return false;
			if (Tree.BlackboardAsset)
			{
				FUeAgentContentGraphItem Binding;
				Binding.ItemId = TEXT("binding:blackboard_asset");
				Binding.ItemKind = TEXT("blackboard_binding");
				Binding.OwnerId = Tree.GetPathName();
				Binding.ClassPath = Tree.BlackboardAsset->GetClass()->GetPathName();
				Binding.ContainerId = TEXT("behavior_tree:graph");
				Binding.Order = OutCapture.Items.Num();
				Binding.SemanticIdentity = SemanticIdentity(TEXT("uai.semantic.ai_behavior.blackboard_binding"),
					{{TEXT("identity"), Binding.ItemId}}, TEXT("item"));
				auto Resources = MakeCanonicalResources();
				const auto State = MakeShared<FJsonObject>();
				State->SetStringField(TEXT("name"), TEXT("Blackboard"));
				State->SetStringField(TEXT("blackboard"), Tree.BlackboardAsset->GetPathName());
				if (!TryCanonicalObject(State, Resources, Binding.State, OutError)) return false;
				OutCapture.Items.Add(MoveTemp(Binding));
			}
			return true;
		}

		bool CaptureDataTable(
			UDataTable& Table,
			FUeAgentContentGraphCapture& OutCapture,
			FUeAgentContentGraphReadError& OutError)
		{
			using namespace DomainAdapters::DataTableCurveTable;
			const FString OwnerId = Table.GetPathName();
			UE::UAI::Contract::FDiagnostic Error;
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!RegisterDataTableAuthoringState(Error)
				|| !BuildDataTableAuthoringReadRequests(Table, OwnerId, Requests, Error))
				return Fail(OutError, Error, OwnerId);
			if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
				return Fail(OutError, Capture.Diagnostic, OwnerId);
			auto Resources = MakeSemanticWorkspaceResourceTracker();
			FUeAgentCanonicalSnapshotPtr Snapshot;
			FAuthoringModelPtr Model;
			FWorkspaceError WorkspaceError;
			if (!TryBuildCanonicalAuthoringCapture(TEXT("ue.data_table.asset"), TEXT("llm_authoring"),
				Capture.Snapshot, Resources, Snapshot, Model, WorkspaceError))
				return Fail(OutError, WorkspaceError.GetDiagnostic(), OwnerId);
			OutCapture.CanonicalRevision = Snapshot->GetRevision();
			OutCapture.Revision = TEXT("observed");
			FUeAgentResourceTracker PropertyResources = MakeCanonicalResources();
			int32 PropertyCount = 0;
			if (!AddProperties(Table, PropertyResources, OutCapture.Properties, PropertyCount, OutError)) return false;
			for (const auto& Entry : Capture.Snapshot.CopyEntries())
			{
				if (Entry.SemanticId != TEXT("data_table.rows")) continue;
				const auto State = Entry.Value->AsObject();
				FUeAgentContentGraphContainer Container;
				Container.ContainerId = TEXT("rows:") + OwnerId;
				Container.ContainerKind = TEXT("data_table_rows");
				Container.OwnerId = OwnerId;
				Container.SemanticIdentity = SemanticIdentity(Entry.SemanticId,
					{{TEXT("asset_path"), OwnerId}}, TEXT("row_schema"));
				if (!TryCanonicalObject(State->GetObjectField(TEXT("row_schema")).ToSharedRef(), Resources, Container.State, OutError))
					return false;
				OutCapture.Containers.Add(Container);
				const auto Rows = State->GetObjectField(TEXT("rows"));
				TArray<FString> Names;
				Rows->Values.GetKeys(Names);
				Names.Sort();
				for (const FString& Name : Names)
				{
					ON_SCOPE_EXIT { SupplementCaptureOwner(OutError, OwnerId, TEXT("/rows/") + Name.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")), Name); };
					UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
					const TMap<FString, FString> Selectors{{TEXT("asset_path"), OwnerId}, {TEXT("row_name"), Name}};
					if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(TEXT("data_table.authoring.identity"),
						TEXT("data_table"), Selectors, Identity, Error))
						return Fail(OutError, Error, Name);
					FUeAgentContentGraphItem Item;
					Item.ItemId = Identity.CanonicalKey;
					Item.ItemKind = TEXT("data_table_row");
					Item.OwnerId = OwnerId;
					Item.ClassPath = Table.GetRowStruct()->GetPathName();
					Item.ContainerId = Container.ContainerId;
					Item.Order = OutCapture.Items.Num();
					Item.SemanticIdentity = SemanticIdentity(Entry.SemanticId, Selectors, TEXT("row"));
					if (!TryCanonicalObject(Rows->GetObjectField(Name).ToSharedRef(), Resources, Item.State, OutError)) return false;
					OutCapture.Items.Add(MoveTemp(Item));
				}
			}
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
				const TArray<UEdGraph*>& Graphs) -> bool
			{
				for (UEdGraph* Graph : Graphs)
				{
					if (!Graph)
					{
						continue;
					}
					const FString GraphKind = UeAgentBlueprintAuthoring::ExportGraphKind(Blueprint, Graph);
					const FString GraphGuid = GuidText(Graph->GraphGuid);
					const FString GraphId =
						UeAgentBlueprintAuthoring::GraphIdentity(*Graph);
					// Named graph containers share the authored name with HJSON and
					// candidate lookup; node occurrence aliases do not name graphs.
					const FString GraphName = Graph->GetName();
					TMap<FString, FString> GraphSelectors = AssetSelectors;
					GraphSelectors.Add(TEXT("graph_guid"), GraphGuid);
					GraphSelectors.Add(TEXT("graph_kind"), GraphKind);
					GraphSelectors.Add(TEXT("graph_name"), Graph->GetName());
					GraphSelectors.Add(TEXT("public_name"), GraphName);
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
						return Fail(OutError, HookError);
					}
					TSharedRef<FJsonObject> GraphState = MakeShared<FJsonObject>();
					GraphState->SetStringField(TEXT("name"), GraphName);
					GraphState->SetStringField(TEXT("graph_kind"), GraphKind);
					UeAgentBlueprintAuthoring::AppendGraphDisplayState(Blueprint, *Graph, *GraphState);
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
					TArray<FGraphAuthoringNodeAliasInput> NodeAliasInputs;
					for (const FBlueprintUmgGraphItemIdentity& Item : Topology.GetItems())
					{
						UEdGraphNode* Node = NodesByGuid.FindRef(Item.GetNativeGuid());
						const FString SemanticName = Node
							? UeAgentBlueprintAuthoring::NodeSemanticName(*Node)
							: (Item.GetStableBuiltinRole().IsEmpty()
								? TEXT("builtin")
								: Item.GetStableBuiltinRole());
						NodeAliasInputs.Add({Item.GetItemKey(), SemanticName,
							Node ? UeAgentBlueprintAuthoring::NodeReference(Node) : Item.GetItemKey()});
					}
					FGraphAuthoringAliasProjection NodeAliases;
					FWorkspaceError NodeAliasError;
					if (!TryBuildGraphAuthoringAliasProjection(
						NodeAliasInputs, NodeAliases, NodeAliasError))
					{
						return Fail(OutError, NodeAliasError.GetDiagnostic(), GraphId);
					}
					for (int32 Index = 0; Index < Topology.GetItems().Num(); ++Index)
					{
						const FBlueprintUmgGraphItemIdentity& Item =
							Topology.GetItems()[Index];
						UEdGraphNode* Node = NodesByGuid.FindRef(Item.GetNativeGuid());
						const FString* NodeAlias =
							NodeAliases.AliasBySourceId.Find(Item.GetItemKey());
						if (!NodeAlias)
						{
							auto Actual = MakeShared<FJsonObject>();
							Actual->SetStringField(TEXT("graph"), GraphSelectors.FindRef(TEXT("graph_name")));
							Actual->SetNumberField(TEXT("node_index"), Index);
							Actual->SetBoolField(TEXT("alias_present"), false);
							return Fail(OutError, CaptureFact(TEXT("content_graph_read_blueprint_node_alias_missing"),
								TEXT("Blueprint graph item has no projected public node name."), TEXT("/nodes"),
								MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueBoolean>(true), Item.GetItemKey()));
						}
						TMap<FString, FString> NodeSelectors = GraphSelectors;
						NodeSelectors.Add(TEXT("node_key"), Item.GetItemKey());
						NodeSelectors.Add(TEXT("public_name"), *NodeAlias);
						if (!Item.GetNativeGuid().IsEmpty())
						{
							NodeSelectors.Add(TEXT("node_guid"), Item.GetNativeGuid());
						}
						NodeSelectors.Add(
							TEXT("node_kind"),
							Node ? Node->GetClass()->GetName() : TEXT("builtin"));
						TSharedRef<FJsonObject> NodeState = MakeShared<FJsonObject>();
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
							AppendGraphNodeSemanticState(*Node, *NodeState,
								AssetSelectors.FindRef(TEXT("asset_path")));
						}
						else
						{
							NodeState->SetStringField(TEXT("name"), Item.GetStableBuiltinRole().IsEmpty()
								? TEXT("builtin") : Item.GetStableBuiltinRole());
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
					TMap<FString, int32> NextPinOccurrences;
					TMap<FString, TArray<FGraphAuthoringPinAliasInput>> PinInputsByNode;
					for (const FBlueprintUmgGraphPinIdentity& Pin : Topology.GetPins())
					{
						PinsByKey.Add(Pin.GetPinKey(), Pin);
						const FString OccurrenceKey = Pin.GetOwnerItemKey()
							+ TEXT("|") + Pin.GetNativeName().ToLower();
						PinInputsByNode.FindOrAdd(Pin.GetOwnerItemKey()).Add({
							Pin.GetPinKey(), Pin.GetNativeName(),
							NextPinOccurrences.FindOrAdd(OccurrenceKey)++,
							Pin.GetDirection() == EBlueprintUmgGraphPinDirection::Output
								? EGraphAuthoringEndpointDirection::Output
								: EGraphAuthoringEndpointDirection::Input});
					}
					TMap<FString, FString> PublicPinByKey;
					for (const auto& NodePins : PinInputsByNode)
					{
						FGraphAuthoringPinAliasProjection PinAliases;
						FWorkspaceError PinError;
						if (!TryBuildGraphAuthoringPinAliasProjection(NodePins.Value, PinAliases, PinError))
							return Fail(OutError, PinError.GetDiagnostic(), GraphId);
						PublicPinByKey.Append(PinAliases.AliasBySourceId);
					}
					for (const FBlueprintUmgGraphLinkIdentity& Link : Topology.GetLinks())
					{
						const FBlueprintUmgGraphPinIdentity* From =
							PinsByKey.Find(Link.GetFromPinKey());
						const FBlueprintUmgGraphPinIdentity* To =
							PinsByKey.Find(Link.GetToPinKey());
						if (!From || !To)
						{
							return Fail(OutError, CaptureFact(TEXT("content_graph_read_blueprint_edge_endpoint_missing"), TEXT("Blueprint graph link references a pin absent from the captured topology."), TEXT("/connections"), MakeShared<FJsonValueString>(!From ? TEXT("source pin missing") : TEXT("target pin missing")), MakeShared<FJsonValueString>(TEXT("source and target pins present")), Link.GetLinkKey()));
						}
						const FString* FromAlias = NodeAliases.AliasBySourceId.Find(
							From->GetOwnerItemKey());
						const FString* ToAlias = NodeAliases.AliasBySourceId.Find(
							To->GetOwnerItemKey());
						if (!FromAlias || !ToAlias)
						{
							return Fail(OutError, CaptureFact(TEXT("content_graph_read_blueprint_edge_node_alias_missing"), TEXT("Blueprint graph link endpoint has no projected public node name."), TEXT("/connections"), MakeShared<FJsonValueString>(!FromAlias ? TEXT("source node name missing") : TEXT("target node name missing")), MakeShared<FJsonValueString>(TEXT("source and target node names present")), Link.GetLinkKey()));
						}
						FString FromEndpoint;
						FString ToEndpoint;
						FWorkspaceError EndpointError;
						if (!TryFormatGraphAuthoringEndpoint(
							*FromAlias,
							PublicPinByKey.FindRef(From->GetPinKey()), 0,
							EGraphAuthoringEndpointDirection::Output,
							FromEndpoint,
							EndpointError)
							|| !TryFormatGraphAuthoringEndpoint(
								*ToAlias,
								PublicPinByKey.FindRef(To->GetPinKey()), 0,
								EGraphAuthoringEndpointDirection::Input,
								ToEndpoint,
								EndpointError))
						{
							return Fail(OutError, EndpointError.GetDiagnostic(), Link.GetLinkKey());
						}
						TSharedRef<FJsonObject> EdgeState = MakeShared<FJsonObject>();
						EdgeState->SetStringField(TEXT("from"), FromEndpoint);
						EdgeState->SetStringField(TEXT("to"), ToEndpoint);
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
						CapturedEdge.RelationKind = TEXT("connection");
						CapturedEdge.SourceItemId = From->GetOwnerItemKey();
						CapturedEdge.TargetItemId = To->GetOwnerItemKey();
						CapturedEdge.SourceEndpoint = {From->GetPinKey(), FromEndpoint};
						CapturedEdge.TargetEndpoint = {To->GetPinKey(), ToEndpoint};
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

			TArray<UEdGraph*> ExportGraphs;
			UeAgentBlueprintAuthoring::GatherExportGraphs(Blueprint, ExportGraphs);
			if (!AppendGraphs(ExportGraphs))
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
					return Fail(OutError, HookError);
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
					return Fail(OutError, HookError);
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
					CapturedRelation.RelationKind = TEXT("parent");
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

			// Topology identities and positions do not describe node semantics.
			// Read the same native values used by Blueprint/UMG/AnimBP authoring:
			// graph signatures, node properties and editable pin defaults included.
			// Keep UI/diagnostic metadata out of the revision and reuse the existing
			// canonicalizer rather than maintaining a second node serializer.
			UE::UAI::Contract::FDiagnostic SemanticError;
			TArray<UeAgentAuthoringState::FReadRequest> SemanticRequests;
			if (!UeAgentBlueprintAuthoring::RegisterAuthoring(SemanticError)
				|| !UeAgentBlueprintAuthoring::AppendReusableBlueprintExportSnapshotReadRequests(
					Blueprint, SemanticRequests, SemanticError))
			{
				return Fail(OutError, SemanticError, OwnerId);
			}
			UeAgentAuthoringState::FCaptureResult SemanticCapture;
			if (!UeAgentAuthoringState::CaptureSnapshot(SemanticRequests, SemanticCapture))
			{
				return Fail(OutError, SemanticCapture.Diagnostic, OwnerId);
			}
			for (const auto& Entry : SemanticCapture.Snapshot.CopyEntries())
			{
				const auto Fact = MakeShared<FJsonObject>();
				Fact->SetStringField(TEXT("semantic"), Entry.SemanticId);
				Fact->SetStringField(TEXT("identity"), Entry.TargetIdentity.CanonicalKey);
				Fact->SetBoolField(TEXT("available"), Entry.bAvailable);
				if (Entry.Value.IsValid()) Fact->SetField(TEXT("value"), Entry.Value);
				if (Entry.ValueDefinition.IsValid()) Fact->SetField(TEXT("definition"), Entry.ValueDefinition);
				RevisionFacts.Add(UeAgentAuthoringState::MakeStableCanonicalValueKey(
					MakeShared<FJsonValueObject>(Fact)));
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
				return Fail(OutError, CaptureFact(TEXT("content_graph_read_niagara_target_invalid"), TEXT("Niagara graph capture requires a system, emitter or script asset."), TEXT("/class"), MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName()), MakeShared<FJsonValueString>(TEXT("NiagaraSystem, NiagaraEmitter or NiagaraScript"))));
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
					return Fail(OutError, CaptureFact(TEXT("content_graph_read_niagara_emitter_data_missing"), TEXT("Niagara emitter has no latest version data."), TEXT("/emitter_data"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
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
					return Fail(OutError, CaptureFact(TEXT("content_graph_read_niagara_script_data_missing"), TEXT("Niagara script has no latest version data."), TEXT("/script_data"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
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
			UE::UAI::Contract::FDiagnostic NativeError;
			if (!Script
				&& !EnumerateEmitterOwners(Asset, EmitterOwners, NativeError))
			{
				return Fail(OutError, NativeError, OwnerId);
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
				return Fail(OutError, NativeError, OwnerId);
			}
			FMaterialNiagaraGraphNativeHook Hook;
			for (int32 StageIndex = 0; StageIndex < Stages.Num(); ++StageIndex)
			{
				const FResolvedStage& Stage = Stages[StageIndex];
				const FString EmitterHandleId = GetStageOwnerId(Stage);
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
					if (HookError.TargetPath.IsEmpty()) HookError.TargetPath = OwnerId;
					if (HookError.CandidateName.IsEmpty()) HookError.CandidateName = Stage.EmitterName.IsEmpty()
						? ScriptUsageToString(Stage.Usage) : Stage.EmitterName + TEXT(": ") + ScriptUsageToString(Stage.Usage);
					return Fail(OutError, HookError, StageId);
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
					if (!GetModules(*Stage.OutputNode, NativeModules, OutError)) return false;
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
						auto Diagnostic = CaptureFact(TEXT("content_graph_read_niagara_module_missing"),
							TEXT("Captured Niagara stage item has no matching native module."),
							TEXT("/stages/") + FString::FromInt(StageIndex) + TEXT("/modules/") + FString::FromInt(ModuleIndex),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Item.GetItemKey());
						Diagnostic.InternalContext = StageId + TEXT(":") + Item.GetNativeGuid();
						return Fail(OutError, MoveTemp(Diagnostic));
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
							return Fail(OutError, NativeError, Item.GetItemKey());
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
						Assignment.OwnerId = StageId + TEXT(":") + Item.GetItemKey();
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
						return Fail(OutError, NativeError, Item.GetItemKey());
					}
					if (!Inputs->Values.IsEmpty())
					{
						ModuleState->SetObjectField(TEXT("inputs"), Inputs);
					}
					ModuleState->SetObjectField(TEXT("input_schema"), UeAgentNiagaraStageModuleAuthoring::BuildModuleInputSchema(*Module, Stage.Asset));
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
					CapturedModule.ItemId = StageId + TEXT(":") + Item.GetItemKey();
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
						StageId + TEXT(":") + Ordering.GetItemKey());
					OrderState->SetStringField(
						TEXT("predecessor_item_id"),
						Ordering.GetPredecessorItemKey().IsEmpty()
							? FString() : StageId + TEXT(":") + Ordering.GetPredecessorItemKey());
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
					CapturedOrder.RelationKind = TEXT("order");
					CapturedOrder.SourceItemId =
						Ordering.GetPredecessorItemKey().IsEmpty()
							? StageId
							: StageId + TEXT(":") + Ordering.GetPredecessorItemKey();
					CapturedOrder.TargetItemId = StageId + TEXT(":") + Ordering.GetItemKey();
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

	bool FUeAgentContentGraphReadProductionEnvironment::TryCaptureBlueprintRevision(
		UBlueprint& Blueprint,
		FString& OutRevision,
		FUeAgentContentGraphReadError& OutError)
	{
		OutRevision.Reset();
		OutError = {};
		FUeAgentContentGraphCapture Capture;
		Capture.Owner = &Blueprint;
		Capture.DomainId = DomainId(Blueprint);
		if (!CaptureBlueprint(Blueprint, Capture, OutError)) return false;
		OutRevision = MoveTemp(Capture.Revision);
		return true;
	}

	bool FUeAgentContentGraphReadProductionEnvironment::TryCaptureTextureGraphSettings(UObject& Owner,
		const TMap<int32, TSet<FString>>& FieldsByNode, FUeAgentContentGraphCapture& OutCapture,
		FUeAgentContentGraphReadError& OutError, bool bIncludeTopology)
	{
		OutCapture = {}; OutError = {};
		OutCapture.Owner = &Owner;
		OutCapture.DomainId = TEXT("texture_graph");
		const FString Root = TEXT("content_graph.container:") + Owner.GetPathName();
		OutCapture.Containers.Add({Root, TEXT("root"), Owner.GetPathName(), FString(), 0});
		auto Resources = MakeCanonicalResources();
		if (!AugmentTextureGraphCapture(Owner, Root, Resources, OutCapture, OutCapture.Revision, OutError, &FieldsByNode, bIncludeTopology)) return false;
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=texture_setting_current nodes=%d properties=%d topology=%s complete_asset_values=false"),
			OutCapture.Items.Num(), OutCapture.Properties.Num(), bIncludeTopology ? TEXT("true") : TEXT("false"));
		return true;
	}

	static bool CaptureOwnerValues(
			UObject& Owner,
			FUeAgentContentGraphCapture& OutCapture,
			FUeAgentContentGraphReadError& OutError, bool bForQuery)
	{
		OutCapture = {};
		OutError = {};
		if (!IsValid(&Owner))
		{
			return Fail(OutError, CaptureFact(TEXT("content_graph_read_owner_invalid"), TEXT("Content capture owner is invalid or pending destruction."), TEXT("/owner"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
		}
		ON_SCOPE_EXIT { SupplementCaptureOwner(OutError, Owner.GetPathName()); };
		const FString ResolvedDomain = DomainId(Owner);
		if (ResolvedDomain.IsEmpty())
		{
			return Fail(OutError, CaptureFact(TEXT("content_graph_read_domain_unsupported"), TEXT("Content graph capture does not support this asset class."), TEXT("/class"), MakeShared<FJsonValueString>(Owner.GetClass()->GetPathName()), MakeShared<FJsonValueString>(TEXT("supported content graph asset class"))));
		}
		OutCapture.Owner = &Owner;
		OutCapture.DomainId = ResolvedDomain;
		if (UWorld* World = Cast<UWorld>(&Owner)) return CaptureLevel(*World, OutCapture, OutError, nullptr, false, !bForQuery);
		if (UBehaviorTree* Tree = Cast<UBehaviorTree>(&Owner)) return CaptureBehaviorTree(*Tree, OutCapture, OutError);
		if (UEnvQuery* Query = Cast<UEnvQuery>(&Owner)) return CaptureEqs(*Query, OutCapture, OutError);
		if (UStateTree* Tree = Cast<UStateTree>(&Owner)) return CaptureStateTree(*Tree, OutCapture, OutError);
		if (UBlackboardData* Blackboard = Cast<UBlackboardData>(&Owner)) return CaptureBlackboard(*Blackboard, OutCapture, OutError);
		if (USmartObjectDefinition* Definition = Cast<USmartObjectDefinition>(&Owner)) return CaptureSmartObject(*Definition, OutCapture, OutError);
		if (ResolvedDomain == TEXT("data_table"))
		{
			return CaptureDataTable(*CastChecked<UDataTable>(&Owner), OutCapture, OutError);
		}
		if (DomainAdapters::ModelingGeometryEffect::
				IsModelingGeometryCarrier(Owner))
		{
			TOptional<FUeAgentStateRevision> CarrierRevision;
			ExecutionMutation::FExecutionMutationError CarrierError;
			if (!DomainAdapters::ModelingGeometryEffect::TryReadModelingGeometryCarrierRevision(
				Owner.GetPathName(), Owner, CarrierRevision, CarrierError))
				return Fail(OutError, CarrierError, Owner.GetPathName());
			if (!CarrierRevision.IsSet())
				return Fail(OutError, CaptureFact(TEXT("content_graph_read_carrier_revision_failed"), TEXT("Modeling geometry capture completed without a carrier revision."), TEXT("/revision"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			OutCapture.CanonicalRevision = CarrierRevision;
		}
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
		const bool bGenericRootProperties = ResolvedDomain == TEXT("world_hlod")
			|| ResolvedDomain == TEXT("project_settings")
			|| ResolvedDomain == TEXT("material_parameter_collection")
			|| ResolvedDomain == TEXT("pcg_data_asset")
			|| ResolvedDomain == TEXT("recursive_dataasset")
			|| ResolvedDomain == TEXT("input_action") || ResolvedDomain == TEXT("specular_profile")
			|| ResolvedDomain == TEXT("media_player")
			|| ResolvedDomain == TEXT("media_source")
			|| ResolvedDomain == TEXT("media_texture")
			|| ResolvedDomain == TEXT("paper_flipbook")
			|| ResolvedDomain == TEXT("paper_sprite")
			|| ResolvedDomain == TEXT("paper_sprite_atlas")
			|| ResolvedDomain == TEXT("paper_tileset")
			|| ResolvedDomain == TEXT("sound_attenuation") || ResolvedDomain == TEXT("sound_concurrency")
			|| ResolvedDomain == TEXT("sound_class")
			|| ResolvedDomain == TEXT("sound_submix")
			|| ResolvedDomain == TEXT("sound_mix")
			|| ResolvedDomain == TEXT("audio_effect_preset")
			|| ResolvedDomain == TEXT("input_mapping_context")
			|| UeAgentCurveJson::FindAssetCurveContract(&Owner);
		TArray<FString>* GenericRevisionFactsPtr =
			bGenericRootProperties
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
		if (UPCGGraph* PcgGraph = Cast<UPCGGraph>(&Owner))
		{
			if (!AugmentPcgCapture(*PcgGraph, Resources, OutCapture, OutError)) return false;
		}
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
		FString NativeAudioRevision;
		if (ResolvedDomain == TEXT("audio")
			&& !AugmentAudioCapture(
				Owner,
				RootContainerId,
				Resources,
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
		if (bGenericRootProperties)
		{
			if (UeAgentCurveJson::FindAssetCurveContract(&Owner))
			{
				FString CurveState; UE::UAI::Contract::FDiagnostic CurveError;
				if (!CommandContract::Json::TryCanonicalize(
					UeAgentCurveJson::BuildCurveAssetJson(&Owner, true), CurveState, CurveError))
					return Fail(OutError, CurveError);
				GenericRevisionFacts.Add(TEXT("curve|") + CurveState);
			}
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
			TEXT("%s:%s:%d:%d:%d%s%s%s%s%s%s%s%s"),
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
			NativeAudioRevision.IsEmpty() ? TEXT("") : TEXT(":"),
			*NativeAudioRevision,
			NativeTextureGraphRevision.IsEmpty() ? TEXT("") : TEXT(":"),
			*NativeTextureGraphRevision,
			NativeAnimationRigRevision.IsEmpty() ? TEXT("") : TEXT(":"),
			*NativeAnimationRigRevision);
		return true;
	}

	bool FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(UObject& Owner,
		FUeAgentContentGraphCapture& OutCapture, FUeAgentContentGraphReadError& OutError)
	{
		return CaptureOwnerValues(Owner, OutCapture, OutError, false);
	}

	static bool CaptureLevelReadTopology(FUeAgentContentGraphReadProductionEnvironment& Environment,
		const UE::UAI::Discovery::FUeAgentCandidateBinding& Target,
		const FUeAgentExecutionContextRef& Context, FUeAgentContentGraphCapture& OutCapture,
		bool& bDeferredProperties, FUeAgentContentGraphReadError& OutError, bool bIncludeGeometry)
	{
		bDeferredProperties = false;
		UWorld* World = Cast<UWorld>(ResolveObject(Target.GetPublicRef()));
		if (!World) return Environment.TryCapture(Target, Context, OutCapture, OutError);
		OutCapture = {}; OutError = {};
		OutCapture.Owner = World; OutCapture.DomainId = DomainId(*World);
		// Keep the original loaded descriptor references through a later selected
		// getter, without using complete property capture to keep cells alive.
		if (World->IsPartitionedWorld()) OutCapture.NativeReadScope = MakeShared<UeAgentLevelAuthoring::FLevelPartitionReadScope>(*World);
		TArray<UeAgentLevelAuthoring::FLevelActorCaptureScope> Scope;
		for (TActorIterator<AActor> It(World); It; ++It) {
			auto& ActorScope = Scope.AddDefaulted_GetRef();
			ActorScope.ActorGuid = It->GetActorGuid();
			ActorScope.ManagedId = UeAgentLevelAuthoring::GetManagedActorId(**It);
			ActorScope.bActorState = ActorScope.bAllComponents = true;
			ActorScope.bComponentProperties = false;
		}
		if (!CaptureLevel(*World, OutCapture, OutError, &Scope, bIncludeGeometry)) return false;
		bDeferredProperties = true;
		UE_LOG(LogTemp, Display, TEXT("UAI read timing phase=level_read_topology actors=%d items=%d property_values=%d include_geometry=%s complete_asset_properties=false"),
			Scope.Num(), OutCapture.Items.Num(), OutCapture.Properties.Num(), bIncludeGeometry ? TEXT("true") : TEXT("false"));
		return true;
	}

	bool FUeAgentContentGraphReadProductionEnvironment::TryCapturePropertyTopology(
		const UE::UAI::Discovery::FUeAgentCandidateBinding& Target,
		const FUeAgentExecutionContextRef& Context, FUeAgentContentGraphCapture& OutCapture,
		bool& bDeferredProperties, FUeAgentContentGraphReadError& OutError)
	{
		return CaptureLevelReadTopology(*this, Target, Context, OutCapture, bDeferredProperties, OutError, false);
	}
	bool FUeAgentContentGraphReadProductionEnvironment::TryCaptureItemTopology(
		const UE::UAI::Discovery::FUeAgentCandidateBinding& Target,
		const FUeAgentExecutionContextRef& Context, FUeAgentContentGraphCapture& OutCapture, FUeAgentContentGraphReadError& OutError, bool bIncludeGeometry)
	{
		bool bDeferredProperties = false;
		return CaptureLevelReadTopology(*this, Target, Context, OutCapture, bDeferredProperties, OutError, bIncludeGeometry);
	}

	bool FUeAgentContentGraphReadProductionEnvironment::TryCaptureSelectedProperties(
		FUeAgentContentGraphCapture& Capture, const TSet<FString>& Owners, const TSet<FString>& Fields,
		FUeAgentContentGraphReadError& OutError)
	{
		UWorld* World = Cast<UWorld>(Capture.Owner);
		if (!World) return true;
		using namespace UeAgentLevelAuthoring;
		TArray<FLevelActorCaptureScope> Scope;
		for (const auto& Item : Capture.Items) {
			if (!Owners.Contains(Item.ItemId)) continue;
			const bool bActor = Item.ItemKind == TEXT("actor");
			if (!bActor && Item.ItemKind != TEXT("component")) continue;
			FGuid Guid;
			FGuid::Parse(Item.SemanticIdentity.Selectors.FindRef(bActor ? TEXT("actor_guid") : TEXT("owner_actor_guid")), Guid);
			auto* ActorScope = Scope.FindByPredicate([&](const auto& Existing) { return Existing.ActorGuid == Guid; });
			if (!ActorScope) {
				ActorScope = &Scope.AddDefaulted_GetRef();
				ActorScope->ActorGuid = Guid; ActorScope->PropertyNames = Fields;
			}
			if (bActor) ActorScope->bActorProperties = true;
			else ActorScope->ComponentNames.Add(Item.SemanticIdentity.Selectors.FindRef(TEXT("component_name")));
		}
		FLevelAuthoringCapture Selected;
		UE::UAI::Contract::FDiagnostic Error;
		if (!TryCaptureLevelScopedNativeState(*World, Scope, Selected, Error, TEXT("llm_authoring")))
			return Fail(OutError, Error, World->GetPathName());
		auto Resources = MakeSemanticWorkspaceResourceTracker(); FWorkspaceError ProjectionError;
		if (!TryBuildCanonicalAuthoringSnapshot(TEXT("ue.level.asset"), TEXT("llm_authoring"), Selected.Snapshot,
			Resources, Selected.CanonicalSnapshot, ProjectionError, false)) return Fail(OutError, ProjectionError.GetDiagnostic(), World->GetPathName());
		// This is a revision of the actual selected observation, never a native
		// identity or a promise that unrequested fields were observed.
		Capture.CanonicalRevision = Selected.CanonicalSnapshot->GetRevision();
		Capture.Revision = Selected.CanonicalSnapshot->GetRevision().GetGeneration();
		if (!AppendLevelProperties(*Selected.CanonicalSnapshot, Capture, OutError)) return false;
		UE_LOG(LogTemp, Display, TEXT("UAI read timing phase=level_selected_properties owners=%d fields=%d native_requests=%d returned_properties=%d complete_asset_properties=false"),
			Owners.Num(), Fields.Num(), Selected.Snapshot.Num(), Capture.Properties.Num());
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
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("content_graph_read_target_unresolved"),
				TEXT("The bound target reference could not be resolved to a loaded object for capture."), TEXT("content_capture"), Context.GetProjectInstanceId(), Target.GetPublicRef());
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.TargetPath = Target.GetPublicRef();
			Diagnostic.PropertyPath = TEXT("/target");
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(Target.GetPublicRef());
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("resolvable object reference"));
			return Fail(OutError, Diagnostic);
		}
		return CaptureOwnerValues(
			*Owner,
			OutCapture,
			OutError, true);
	}
}
