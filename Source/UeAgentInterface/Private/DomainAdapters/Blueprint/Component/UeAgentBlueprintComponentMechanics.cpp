#include "DomainAdapters/Blueprint/Component/UeAgentBlueprintComponentMechanics.h"

#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "DomainAdapters/BlueprintUmgGraph/UeAgentBlueprintUmgGraphNativeHook.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::DomainAdapters::Blueprint::Component
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::DomainAdapters::BlueprintUmgGraph;
	using namespace UE::UAI::HjsonWorkspace;

	namespace
	{
		const FString BlueprintType = TEXT("/Script/Engine.Blueprint");
		const FString SceneComponentType =
			TEXT("/Script/Engine.SceneComponent");
		const FString PointLightComponentType =
			TEXT("/Script/Engine.PointLightComponent");
		const FString StaticMeshComponentType =
			TEXT("/Script/Engine.StaticMeshComponent");
		const FString BoxComponentType =
			TEXT("/Script/Engine.BoxComponent");
		const FString SchemaId = TEXT("uai.blueprint.component.edit/1");
		const FString ComponentContract =
			TEXT("uai.init.blueprint.scs_component/1");

		struct FPlannedComponent final
		{
			FString Id;
			FString ComponentClass;
			FString CandidateName;
			FString BindingRevision;
			FString ParentId;
			int32 Order = 0;
			int32 Depth = 0;
			bool bHasRelativeLocation = false;
			FVector RelativeLocation = FVector::ZeroVector;
			bool bHasIntensity = false;
			double Intensity = 0.0;
			UClass* Class = nullptr;
		};

		struct FPreparedImport final
		{
			FString Target;
			FString ExpectedRevision;
			FString InitialRevision;
			UBlueprint* Blueprint = nullptr;
			TArray<FPlannedComponent> Components;
		};

		bool Fail(
			FUeAgentBlueprintComponentMechanicsError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		FString Sha256(const FString& Text)
		{
			FTCHARToUTF8 Utf8(*Text);
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(
				reinterpret_cast<const unsigned char*>(Utf8.Get()),
				static_cast<size_t>(Utf8.Length()),
				Digest);
			FString Result(TEXT("sha256:"));
			for (const uint8 Byte : Digest)
			{
				Result += FString::Printf(TEXT("%02x"), Byte);
			}
			return Result;
		}

		FString Canonical(const TSharedPtr<FJsonObject>& Object)
		{
			FString Result;
			FString Error;
			return Json::TryCanonicalize(Object, Result, Error)
				? Result
				: FString(TEXT("{}"));
		}

		FUeAgentResourceTracker MakeResources()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 4096},
				{EUeAgentResourceDimension::Items, 65536},
				{EUeAgentResourceDimension::Files, 64},
				{EUeAgentResourceDimension::ReferenceEdges, 65536},
				{EUeAgentResourceDimension::WorkUnits, 262144},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					128 * 1024 * 1024},
			}));
		}

		bool IsValidComponentName(const FString& Name)
		{
			if (Name.IsEmpty()
				|| Name != Name.TrimStartAndEnd()
				|| FChar::IsDigit(Name[0]))
			{
				return false;
			}
			for (const TCHAR Character : Name)
			{
				if (!FChar::IsAlnum(Character) && Character != TEXT('_'))
				{
					return false;
				}
			}
			return true;
		}

		UBlueprint* LoadBlueprint(
			const FString& Target,
			FUeAgentBlueprintComponentMechanicsError& OutError)
		{
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Target);
			if (!Blueprint || !Blueprint->SimpleConstructionScript)
			{
				Fail(
					OutError,
					TEXT("blueprint_component_target_unresolved"),
					Target);
				return nullptr;
			}
			return Blueprint;
		}

		UClass* ResolveSupportedClass(
			const FString& ComponentClass,
			FUeAgentBlueprintComponentMechanicsError& OutError)
		{
			if (!IsSupportedBlueprintComponentClass(ComponentClass))
			{
				Fail(
					OutError,
					TEXT("blueprint_component_class_not_registered"),
					ComponentClass);
				return nullptr;
			}
			UClass* Class = LoadObject<UClass>(nullptr, *ComponentClass);
			if (!Class || !Class->IsChildOf(USceneComponent::StaticClass()))
			{
				Fail(
					OutError,
					TEXT("blueprint_component_class_unresolved"),
					ComponentClass);
				return nullptr;
			}
			return Class;
		}

		bool TryCaptureScs(
			UBlueprint& Blueprint,
			FString& OutRevision,
			FString& OutEvidence,
			FUeAgentBlueprintComponentMechanicsError& OutError)
		{
			FBlueprintUmgGraphTopology Topology;
			FBlueprintUmgGraphAdapterError AdapterError;
			FBlueprintUmgGraphNativeHook Hook;
			if (!Hook.TryCaptureBlueprintScs(
					Blueprint,
					Blueprint.GetPathName(),
					Topology,
					OutRevision,
					OutEvidence,
					AdapterError))
			{
				return Fail(
					OutError,
					AdapterError.GetCode().IsEmpty()
						? FString(TEXT("blueprint_component_scs_capture_failed"))
						: AdapterError.GetCode(),
					Blueprint.GetPathName(),
					AdapterError.GetDetail());
			}
			return true;
		}

		FString BindingRevision(
			const FString& Target,
			const FString& ComponentClass,
			const FString& CandidateName,
			const FString& ScsRevision)
		{
			return Sha256(
				Target + TEXT("|") + ComponentClass + TEXT("|")
				+ CandidateName + TEXT("|") + ScsRevision);
		}

		USCS_Node* FindNodeByName(
			const UBlueprint& Blueprint,
			const FString& Name)
		{
			if (!Blueprint.SimpleConstructionScript)
			{
				return nullptr;
			}
			for (USCS_Node* Node
				: Blueprint.SimpleConstructionScript->GetAllNodes())
			{
				if (Node && Node->GetVariableName().ToString() == Name)
				{
					return Node;
				}
			}
			return nullptr;
		}

		TSharedPtr<FJsonObject> VectorObject(const FVector& Value)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("x"), Value.X);
			Result->SetNumberField(TEXT("y"), Value.Y);
			Result->SetNumberField(TEXT("z"), Value.Z);
			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> BuildExistingComponents(
			const UBlueprint& Blueprint)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			if (!Blueprint.SimpleConstructionScript)
			{
				return Result;
			}
			TMap<const USCS_Node*, const USCS_Node*> Parents;
			TMap<const USCS_Node*, int32> Orders;
			const TArray<USCS_Node*>& Roots =
				Blueprint.SimpleConstructionScript->GetRootNodes();
			for (int32 Index = 0; Index < Roots.Num(); ++Index)
			{
				if (Roots[Index])
				{
					Orders.Add(Roots[Index], Index);
				}
			}
			for (USCS_Node* Parent
				: Blueprint.SimpleConstructionScript->GetAllNodes())
			{
				if (!Parent)
				{
					continue;
				}
				const TArray<USCS_Node*>& Children = Parent->GetChildNodes();
				for (int32 Index = 0; Index < Children.Num(); ++Index)
				{
					if (Children[Index])
					{
						Parents.Add(Children[Index], Parent);
						Orders.Add(Children[Index], Index);
					}
				}
			}

			TArray<USCS_Node*> Nodes =
				Blueprint.SimpleConstructionScript->GetAllNodes();
			Nodes.Sort([](const USCS_Node& Left, const USCS_Node& Right)
			{
				return Left.GetVariableName().LexicalLess(
					Right.GetVariableName());
			});
			for (USCS_Node* Node : Nodes)
			{
				if (!Node)
				{
					continue;
				}
				const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(
					TEXT("name"), Node->GetVariableName().ToString());
				Entry->SetStringField(
					TEXT("component_class"),
					Node->ComponentTemplate
						? Node->ComponentTemplate->GetClass()->GetPathName()
						: FString());
				Entry->SetStringField(
					TEXT("variable_guid"),
					Node->VariableGuid.IsValid()
						? Node->VariableGuid.ToString(
							EGuidFormats::DigitsWithHyphensLower)
						: FString());
				const USCS_Node* const* Parent = Parents.Find(Node);
				if (Parent && *Parent)
				{
					Entry->SetStringField(
						TEXT("parent"), (*Parent)->GetVariableName().ToString());
				}
				else
				{
					Entry->SetField(
						TEXT("parent"), MakeShared<FJsonValueNull>());
				}
				Entry->SetNumberField(
					TEXT("order"), Orders.FindRef(Node));
				const TSharedPtr<FJsonObject> Properties =
					MakeShared<FJsonObject>();
				if (const USceneComponent* Scene =
					Cast<USceneComponent>(Node->ComponentTemplate))
				{
					Properties->SetObjectField(
						TEXT("relative_location"),
						VectorObject(Scene->GetRelativeLocation()));
				}
				if (const UPointLightComponent* Light =
					Cast<UPointLightComponent>(Node->ComponentTemplate))
				{
					Properties->SetNumberField(
						TEXT("intensity"), Light->Intensity);
				}
				Entry->SetObjectField(TEXT("properties"), Properties);
				Result.Add(MakeShared<FJsonValueObject>(Entry));
			}
			return Result;
		}

		TSharedPtr<FJsonObject> BuildScsReadback(
			UBlueprint& Blueprint,
			const FString& Revision,
			const FString& Evidence,
			const bool bReloaded)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("target"), Blueprint.GetPathName());
			Result->SetStringField(TEXT("root_type"), BlueprintType);
			Result->SetStringField(TEXT("scs_revision"), Revision);
			Result->SetStringField(TEXT("readback_evidence_id"), Evidence);
			Result->SetBoolField(TEXT("reloaded"), bReloaded);
			Result->SetStringField(
				TEXT("compile_status"),
				Blueprint.Status == BS_Error ? TEXT("error") : TEXT("valid"));
			Result->SetArrayField(
				TEXT("components"), BuildExistingComponents(Blueprint));
			return Result;
		}

		const FHjsonNode* RequiredObjectField(
			const FHjsonNode& Object,
			const FString& Name)
		{
			const FHjsonNode* Value = Object.Get(Name);
			return Value && Value->GetKind() == EHjsonNodeKind::Object
				? Value
				: nullptr;
		}

		bool StringField(
			const FHjsonNode& Object,
			const FString& Name,
			FString& Out)
		{
			const FHjsonNode* Value = Object.Get(Name);
			if (!Value || Value->GetKind() != EHjsonNodeKind::String)
			{
				Out.Reset();
				return false;
			}
			Out = Value->GetText();
			return !Out.IsEmpty();
		}

		bool IntegerField(
			const FHjsonNode& Object,
			const FString& Name,
			int32& Out)
		{
			const FHjsonNode* Value = Object.Get(Name);
			if (!Value || Value->GetKind() != EHjsonNodeKind::Integer)
			{
				return false;
			}
			Out = FCString::Atoi(*Value->GetLexeme());
			return true;
		}

		bool Number(const FHjsonNode& Node, double& Out)
		{
			if (Node.GetKind() != EHjsonNodeKind::Integer
				&& Node.GetKind() != EHjsonNodeKind::Decimal)
			{
				return false;
			}
			Out = FCString::Atod(*Node.GetLexeme());
			return FMath::IsFinite(Out);
		}

		bool TryParseProperties(
			const FHjsonNode& Entry,
			FPlannedComponent& Out,
			FUeAgentBlueprintComponentMechanicsError& OutError)
		{
			const FHjsonNode* Properties = Entry.Get(TEXT("properties"));
			if (!Properties)
			{
				return true;
			}
			if (Properties->GetKind() != EHjsonNodeKind::Object)
			{
				return Fail(
					OutError,
					TEXT("blueprint_component_properties_invalid"),
					Out.Id);
			}
			for (const TPair<FString, FHjsonNodeRef>& Pair
				: Properties->GetEntries())
			{
				if (Pair.Key == TEXT("relative_location"))
				{
					if (Pair.Value->GetKind() != EHjsonNodeKind::Array
						|| Pair.Value->GetItems().Num() != 3)
					{
						return Fail(
							OutError,
							TEXT("blueprint_component_relative_location_invalid"),
							Out.Id);
					}
					double Values[3] = {};
					for (int32 Index = 0; Index < 3; ++Index)
					{
						if (!Number(*Pair.Value->GetItems()[Index], Values[Index]))
						{
							return Fail(
								OutError,
								TEXT("blueprint_component_relative_location_invalid"),
								Out.Id);
						}
					}
					Out.bHasRelativeLocation = true;
					Out.RelativeLocation = FVector(
						Values[0], Values[1], Values[2]);
				}
				else if (Pair.Key == TEXT("intensity")
					&& Out.ComponentClass == PointLightComponentType)
				{
					if (!Number(*Pair.Value, Out.Intensity)
						|| Out.Intensity < 0.0)
					{
						return Fail(
							OutError,
							TEXT("blueprint_component_intensity_invalid"),
							Out.Id);
					}
					Out.bHasIntensity = true;
				}
				else
				{
					return Fail(
						OutError,
						TEXT("blueprint_component_property_not_registered"),
						Out.Id,
						Pair.Key);
				}
			}
			return true;
		}

		bool ResolveDepth(
			const FString& Id,
			TMap<FString, FPlannedComponent*>& ById,
			TSet<FString>& Visiting,
			FUeAgentBlueprintComponentMechanicsError& OutError)
		{
			FPlannedComponent* const* PlannedPtr = ById.Find(Id);
			if (!PlannedPtr || !*PlannedPtr)
			{
				return Fail(
					OutError,
					TEXT("blueprint_component_parent_unresolved"),
					Id);
			}
			FPlannedComponent& Planned = **PlannedPtr;
			if (Planned.Depth > 0 || Planned.ParentId.IsEmpty())
			{
				return true;
			}
			if (Visiting.Contains(Id))
			{
				return Fail(
					OutError,
					TEXT("blueprint_component_parent_cycle"),
					Id);
			}
			Visiting.Add(Id);
			if (!ResolveDepth(Planned.ParentId, ById, Visiting, OutError))
			{
				return false;
			}
			Planned.Depth = ById.FindChecked(Planned.ParentId)->Depth + 1;
			Visiting.Remove(Id);
			return true;
		}

		bool TryPrepareImport(
			const TSharedPtr<FJsonObject>& Params,
			FPreparedImport& Out,
			FUeAgentBlueprintComponentMechanicsError& OutError)
		{
			Out = {};
			if (!Params.IsValid()
				|| !Params->TryGetStringField(TEXT("target"), Out.Target)
				|| Out.Target.IsEmpty()
				|| !Params->TryGetStringField(
					TEXT("expected_revision"), Out.ExpectedRevision)
				|| Out.ExpectedRevision.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("blueprint_component_import_params_invalid"));
			}
			if (Params->HasField(TEXT("carrier"))
				|| Params->HasField(TEXT("hjson_document")))
			{
				return Fail(
					OutError,
					TEXT("blueprint_component_legacy_source_contract_rejected"));
			}
			const TSharedPtr<FJsonObject>* Source = nullptr;
			FString SourceKind;
			FString SourceTargetIdentity;
			FString EntryFile;
			const TArray<TSharedPtr<FJsonValue>>* Documents = nullptr;
			if (!Params->TryGetObjectField(TEXT("source"), Source)
				|| !Source || !Source->IsValid()
				|| !(*Source)->TryGetStringField(TEXT("kind"), SourceKind)
				|| SourceKind != TEXT("resolved")
				|| !(*Source)->TryGetStringField(
					TEXT("target_identity"), SourceTargetIdentity)
				|| SourceTargetIdentity != Out.Target
				|| !(*Source)->TryGetStringField(TEXT("entry_file"), EntryFile)
				|| EntryFile.IsEmpty()
				|| !(*Source)->TryGetArrayField(TEXT("documents"), Documents)
				|| !Documents || Documents->IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("blueprint_component_resolved_source_invalid"));
			}
			FString DocumentText;
			int32 EntryMatches = 0;
			for (const TSharedPtr<FJsonValue>& Value : *Documents)
			{
				const TSharedPtr<FJsonObject> DocumentObject = Value.IsValid()
					? Value->AsObject()
					: nullptr;
				FString RelativePath;
				FString Text;
				if (!DocumentObject.IsValid()
					|| !DocumentObject->TryGetStringField(
						TEXT("path"), RelativePath)
					|| !DocumentObject->TryGetStringField(TEXT("document"), Text))
				{
					return Fail(
						OutError,
						TEXT("blueprint_component_resolved_source_invalid"));
				}
				if (RelativePath == EntryFile)
				{
					DocumentText = MoveTemp(Text);
					++EntryMatches;
				}
			}
			if (EntryMatches != 1 || DocumentText.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("blueprint_component_resolved_source_invalid"),
					EntryFile);
			}
			Out.Blueprint = LoadBlueprint(Out.Target, OutError);
			if (!Out.Blueprint)
			{
				return false;
			}
			if (Out.Blueprint->GetOutermost()->IsDirty())
			{
				return Fail(
					OutError,
					TEXT("blueprint_component_target_dirty"),
					Out.Target,
					TEXT("save or discard the exact target before HJSON authoring"));
			}

			FString CaptureEvidence;
			if (!TryCaptureScs(
					*Out.Blueprint,
					Out.InitialRevision,
					CaptureEvidence,
					OutError))
			{
				return false;
			}
			if (Out.ExpectedRevision != Out.InitialRevision)
			{
				return Fail(
					OutError,
					TEXT("blueprint_component_stale_revision"),
					Out.Target,
					Out.InitialRevision);
			}

			FUeAgentResourceTracker Resources = MakeResources();
			FHjsonDocumentPtr Document;
			FWorkspaceError WorkspaceError;
			if (!TryParseHjsonDocument(
					DocumentText,
					TEXT("inline:blueprint-component"),
					Resources,
					Document,
					WorkspaceError)
				|| !Document.IsValid()
				|| Document->GetRoot()->GetKind() != EHjsonNodeKind::Object)
			{
				return Fail(
					OutError,
					WorkspaceError.GetCode().IsEmpty()
						? FString(TEXT("blueprint_component_hjson_invalid"))
						: WorkspaceError.GetCode(),
					WorkspaceError.GetSourceUnit(),
					WorkspaceError.GetJsonPointer());
			}
			const FHjsonNode& DocumentRoot = *Document->GetRoot();
			FString DocumentSchema;
			if (!StringField(DocumentRoot, TEXT("schema"), DocumentSchema)
				|| DocumentSchema != SchemaId)
			{
				return Fail(
					OutError,
					TEXT("blueprint_component_schema_invalid"),
					DocumentSchema);
			}
			const FHjsonNode* Root =
				RequiredObjectField(DocumentRoot, TEXT("root"));
			FString RootType;
			if (!Root || !StringField(*Root, TEXT("type"), RootType)
				|| RootType != BlueprintType)
			{
				return Fail(
					OutError,
					TEXT("blueprint_component_root_identity_invalid"),
					Out.Target);
			}
			const FHjsonNode* Components = Root->Get(TEXT("components"));
			if (!Components
				|| Components->GetKind() != EHjsonNodeKind::Array
				|| Components->GetItems().Num() == 0)
			{
				return Fail(
					OutError,
					TEXT("blueprint_component_entries_required"),
					Out.Target);
			}

			TSet<FString> Ids;
			TSet<FString> Names;
			for (const FHjsonNodeRef& Value : Components->GetItems())
			{
				if (Value->GetKind() != EHjsonNodeKind::Object)
				{
					return Fail(
						OutError,
						TEXT("blueprint_component_entry_invalid"));
				}
				FPlannedComponent Planned;
				const FHjsonNode* Candidate =
					RequiredObjectField(*Value, TEXT("candidate"));
				if (!StringField(*Value, TEXT("id"), Planned.Id)
					|| Ids.Contains(Planned.Id)
					|| !Candidate
					|| !StringField(
						*Candidate,
						TEXT("name"),
						Planned.CandidateName)
					|| !StringField(
						*Candidate,
						TEXT("component_class"),
						Planned.ComponentClass)
					|| !StringField(
						*Candidate,
						TEXT("binding_revision"),
						Planned.BindingRevision)
					|| !IsValidComponentName(Planned.CandidateName)
					|| Names.Contains(Planned.CandidateName)
					|| !IntegerField(*Value, TEXT("order"), Planned.Order)
					|| Planned.Order < 0)
				{
					return Fail(
						OutError,
						TEXT("blueprint_component_candidate_binding_invalid"),
						Planned.Id);
				}
				const FHjsonNode* Parent = Value->Get(TEXT("parent"));
				if (Parent && Parent->GetKind() == EHjsonNodeKind::String)
				{
					Planned.ParentId = Parent->GetText();
				}
				else if (Parent && Parent->GetKind() != EHjsonNodeKind::Null)
				{
					return Fail(
						OutError,
						TEXT("blueprint_component_parent_invalid"),
						Planned.Id);
				}
				Planned.Class = ResolveSupportedClass(
					Planned.ComponentClass, OutError);
				if (!Planned.Class
					|| Planned.BindingRevision != BindingRevision(
						Out.Target,
						Planned.ComponentClass,
						Planned.CandidateName,
						Out.InitialRevision)
					|| FindNodeByName(
						*Out.Blueprint, Planned.CandidateName)
					|| !TryParseProperties(*Value, Planned, OutError))
				{
					if (OutError.Code.IsEmpty())
					{
						Fail(
							OutError,
							TEXT("blueprint_component_candidate_binding_invalid"),
							Planned.Id);
					}
					return false;
				}
				Ids.Add(Planned.Id);
				Names.Add(Planned.CandidateName);
				Out.Components.Add(MoveTemp(Planned));
			}

			TMap<FString, FPlannedComponent*> ById;
			TSet<FString> SiblingOrders;
			for (FPlannedComponent& Planned : Out.Components)
			{
				const FString OrderingKey = Planned.ParentId
					+ TEXT("|") + FString::FromInt(Planned.Order);
				if (SiblingOrders.Contains(OrderingKey))
				{
					return Fail(
						OutError,
						TEXT("blueprint_component_order_duplicate"),
						Planned.Id,
						OrderingKey);
				}
				SiblingOrders.Add(OrderingKey);
				ById.Add(Planned.Id, &Planned);
			}
			for (FPlannedComponent& Planned : Out.Components)
			{
				TSet<FString> Visiting;
				if (!ResolveDepth(Planned.Id, ById, Visiting, OutError))
				{
					return false;
				}
			}
			Out.Components.Sort([](
				const FPlannedComponent& Left,
				const FPlannedComponent& Right)
			{
				if (Left.Depth != Right.Depth)
				{
					return Left.Depth < Right.Depth;
				}
				if (Left.ParentId != Right.ParentId)
				{
					return Left.ParentId < Right.ParentId;
				}
				if (Left.Order != Right.Order)
				{
					return Left.Order < Right.Order;
				}
				return Left.Id < Right.Id;
			});

			TSharedPtr<const FImportInitializationDomainProviderRegistry>
				ProviderRegistry;
			if (!TryFreezeProcessImportInitializationDomainProviders(
					ProviderRegistry, WorkspaceError)
				|| !ProviderRegistry.IsValid())
			{
				return Fail(
					OutError,
					WorkspaceError.GetCode().IsEmpty()
						? FString(TEXT("blueprint_component_provider_registry_unavailable"))
						: WorkspaceError.GetCode());
			}
			for (const FPlannedComponent& Planned : Out.Components)
			{
				FImportInitializationDomainResolution Resolution;
				if (!ProviderRegistry->TryResolve(
						FImportInitializationDomainRequest(
							TEXT("content_graph.import_workspace"),
							SchemaId,
							Out.Target + TEXT("#") + Planned.Id,
							Planned.ComponentClass,
							EInitializationNodeRole::OwnedObject,
							TEXT("{}")),
						Resolution,
						WorkspaceError)
					|| !Resolution.IsValid()
					|| Resolution.GetInitializationContractId()
						!= ComponentContract)
				{
					return Fail(
						OutError,
						TEXT("blueprint_component_initialization_contract_unresolved"),
						Planned.Id);
				}
			}
			return true;
		}

		bool SaveBlueprint(
			UBlueprint& Blueprint,
			FUeAgentBlueprintComponentMechanicsError& OutError)
		{
			UPackage* Package = Blueprint.GetOutermost();
			const FString Filename = FPackageName::LongPackageNameToFilename(
				Package->GetName(), FPackageName::GetAssetPackageExtension());
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			Args.SaveFlags = SAVE_NoError;
			if (!UPackage::SavePackage(Package, &Blueprint, *Filename, Args))
			{
				return Fail(
					OutError,
					TEXT("blueprint_component_save_failed"),
					Blueprint.GetPathName(),
					Filename);
			}
			return true;
		}

		void ReloadForRollback(UPackage* Package)
		{
			if (!Package)
			{
				return;
			}
			FText Error;
			UPackageTools::ReloadPackages(
				{Package},
				Error,
				EReloadPackagesInteractionMode::AssumeNegative);
		}

		bool ExecuteImport(
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayloadJson,
			FUeAgentBlueprintComponentMechanicsError& OutError)
		{
			FPreparedImport Prepared;
			if (!TryPrepareImport(Params, Prepared, OutError))
			{
				return false;
			}
			UPackage* Package = Prepared.Blueprint->GetOutermost();
			Prepared.Blueprint->Modify();
			Prepared.Blueprint->SimpleConstructionScript->Modify();
			FBlueprintUmgGraphNativeHook Hook;
			TMap<FString, USCS_Node*> Created;
			TArray<FString> MutationEvidence;
			for (const FPlannedComponent& Planned : Prepared.Components)
			{
				USCS_Node* Parent = Planned.ParentId.IsEmpty()
					? nullptr
					: Created.FindRef(Planned.ParentId);
				if (!Planned.ParentId.IsEmpty() && !Parent)
				{
					ReloadForRollback(Package);
					return Fail(
						OutError,
						TEXT("blueprint_component_parent_unresolved"),
						Planned.ParentId);
				}
				USCS_Node* Node = nullptr;
				FBlueprintUmgGraphNativeMutationReceipt Receipt;
				FBlueprintUmgGraphAdapterError AdapterError;
				if (!Hook.TryAddSCSComponent(
						*Prepared.Blueprint,
						*Planned.Class,
						FName(*Planned.CandidateName),
						FGuid::NewGuid(),
						Parent,
						Node,
						Receipt,
						AdapterError)
					|| !Node || !Node->ComponentTemplate)
				{
					ReloadForRollback(Package);
					return Fail(
						OutError,
						AdapterError.GetCode().IsEmpty()
							? FString(TEXT("blueprint_component_create_failed"))
							: AdapterError.GetCode(),
						Planned.Id,
						AdapterError.GetDetail());
				}
				Node->Modify();
				Node->ComponentTemplate->Modify();
				if (Planned.bHasRelativeLocation)
				{
					USceneComponent* Scene =
						Cast<USceneComponent>(Node->ComponentTemplate);
					if (!Scene)
					{
						ReloadForRollback(Package);
						return Fail(
							OutError,
							TEXT("blueprint_component_relative_location_unsupported"),
							Planned.Id);
					}
					Scene->SetRelativeLocation(Planned.RelativeLocation);
				}
				if (Planned.bHasIntensity)
				{
					UPointLightComponent* Light =
						Cast<UPointLightComponent>(Node->ComponentTemplate);
					if (!Light)
					{
						ReloadForRollback(Package);
						return Fail(
							OutError,
							TEXT("blueprint_component_intensity_unsupported"),
							Planned.Id);
					}
					Light->SetIntensity(Planned.Intensity);
				}
				Created.Add(Planned.Id, Node);
				MutationEvidence.Add(Receipt.GetEvidenceId());
			}

			FBlueprintUmgGraphCompileSettleReceipt CompileReceipt;
			FBlueprintUmgGraphAdapterError CompileError;
			if (!Hook.TryCompileAndSettle(
					*Prepared.Blueprint, CompileReceipt, CompileError)
				|| !CompileReceipt.IsCompileComplete()
				|| CompileReceipt.GetErrorCount() != 0)
			{
				ReloadForRollback(Package);
				return Fail(
					OutError,
					CompileError.GetCode().IsEmpty()
						? FString(TEXT("blueprint_component_compile_failed"))
						: CompileError.GetCode(),
					Prepared.Target,
					CompileError.GetDetail());
			}
			if (!SaveBlueprint(*Prepared.Blueprint, OutError))
			{
				ReloadForRollback(Package);
				return false;
			}

			const FString Target = Prepared.Target;
			Created.Reset();
			Prepared.Blueprint = nullptr;
			FText UnloadError;
			if (!UPackageTools::UnloadPackages({Package}, UnloadError))
			{
				return Fail(
					OutError,
					TEXT("blueprint_component_unload_failed"),
					Target,
					UnloadError.ToString());
			}
			UBlueprint* Reloaded = LoadBlueprint(Target, OutError);
			if (!Reloaded)
			{
				return Fail(
					OutError,
					TEXT("blueprint_component_reload_failed"),
					Target);
			}
			FString ReloadedRevision;
			FString ReadbackEvidence;
			if (!TryCaptureScs(
					*Reloaded,
					ReloadedRevision,
					ReadbackEvidence,
					OutError))
			{
				return false;
			}
			const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(
				TEXT("command_id"), TEXT("content_graph.import_workspace"));
			Payload->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
			Payload->SetBoolField(TEXT("mutation_started"), true);
			Payload->SetStringField(
				TEXT("persistence_receipt_id"),
				Sha256(
					Target + TEXT("|") + ReloadedRevision + TEXT("|")
					+ CompileReceipt.GetSettleEvidenceId()));
			Payload->SetStringField(
				TEXT("compile_evidence_id"),
				CompileReceipt.GetCompileEvidenceId());
			Payload->SetStringField(
				TEXT("settle_evidence_id"),
				CompileReceipt.GetSettleEvidenceId());
			TArray<TSharedPtr<FJsonValue>> MutationReceipts;
			for (const FString& Evidence : MutationEvidence)
			{
				MutationReceipts.Add(MakeShared<FJsonValueString>(Evidence));
			}
			Payload->SetArrayField(
				TEXT("mutation_evidence_ids"), MoveTemp(MutationReceipts));
			Payload->SetObjectField(
				TEXT("independent_readback"),
				BuildScsReadback(
					*Reloaded,
					ReloadedRevision,
					ReadbackEvidence,
					true));
			OutPayloadJson = Canonical(Payload);
			return true;
		}
	}

	bool IsSupportedBlueprintComponentClass(const FString& ComponentClass)
	{
		return ComponentClass == SceneComponentType
			|| ComponentClass == PointLightComponentType
			|| ComponentClass == StaticMeshComponentType
			|| ComponentClass == BoxComponentType;
	}

	bool TryQueryBlueprintComponentCandidates(
		const FUeAgentBlueprintComponentCandidateQuery& Query,
		FString& OutPayloadJson,
		FUeAgentBlueprintComponentMechanicsError& OutError)
	{
		OutPayloadJson.Reset();
		OutError = {};
		if (Query.Limit < 1 || Query.Limit > 20 || Query.Cursor < 0)
		{
			return Fail(
				OutError,
				TEXT("blueprint_component_candidate_page_invalid"),
				Query.Target);
		}
		const FString Target = Query.Target.TrimStartAndEnd();
		const FString Parent = Query.Parent.TrimStartAndEnd();
		UBlueprint* Blueprint = LoadBlueprint(Target, OutError);
		if (!Blueprint)
		{
			return false;
		}
		if (!Parent.IsEmpty() && !FindNodeByName(*Blueprint, Parent))
		{
			return Fail(
				OutError,
				TEXT("blueprint_component_parent_unresolved"),
				Parent);
		}
		FString ScsRevision;
		FString ReadbackEvidence;
		if (!TryCaptureScs(
				*Blueprint, ScsRevision, ReadbackEvidence, OutError))
		{
			return false;
		}

		UeAgentPaletteSearch::FSearchParams SearchParams;
		SearchParams.CatalogType = TEXT("actor_component");
		SearchParams.SearchText = Query.Query.TrimStartAndEnd();
		SearchParams.CandidateId = Query.CandidateId.TrimStartAndEnd();
		SearchParams.AssetPath = Blueprint->GetPathName();
		SearchParams.bContextSensitive = true;
		SearchParams.bIncludeItems = true;
		SearchParams.bIncludeGroups = false;
		SearchParams.bIncludeCategoryTree = false;
		SearchParams.Limit = Query.Limit;
		SearchParams.Offset = Query.Cursor;
		UeAgentPaletteSearch::FSearchResult SearchResult;
		FString SearchError;
		if (!UeAgentPaletteSearch::ExecuteSearch(
				SearchParams, SearchResult, SearchError))
		{
			return Fail(
				OutError,
				TEXT("blueprint_component_candidate_query_failed"),
				Target,
				SearchError);
		}
		const int32 StartIndex = FMath::Min(
			Query.Cursor, SearchResult.MatchingCandidates.Num());
		const int32 EndIndex = FMath::Min(
			StartIndex + Query.Limit,
			SearchResult.MatchingCandidates.Num());
		const bool bHasMore = EndIndex < SearchResult.TotalMatching;

		const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(
			TEXT("command_id"), TEXT("blueprint.component.candidates"));
		Payload->SetBoolField(TEXT("read_only"), true);
		Payload->SetStringField(TEXT("target"), Target);
		Payload->SetStringField(TEXT("scs_revision"), ScsRevision);
		Payload->SetStringField(
			TEXT("readback_evidence_id"), ReadbackEvidence);
		const TSharedPtr<FJsonObject> Context = MakeShared<FJsonObject>();
		Context->SetStringField(
			TEXT("parent"), Parent.IsEmpty() ? TEXT("actor_root") : Parent);
		Context->SetStringField(
			TEXT("content_browser_selection"), TEXT("ignored"));
		Payload->SetObjectField(TEXT("context"), Context);
		Payload->SetBoolField(TEXT("complete"), !bHasMore);
		Payload->SetBoolField(TEXT("has_more"), bHasMore);
		Payload->SetNumberField(
			TEXT("total_matching"), SearchResult.TotalMatching);
		Payload->SetNumberField(
			TEXT("next_cursor"), bHasMore ? EndIndex : -1);
		TArray<TSharedPtr<FJsonValue>> Candidates;
		for (int32 Index = StartIndex; Index < EndIndex; ++Index)
		{
			const UeAgentPaletteSearch::FCandidate& Match =
				SearchResult.MatchingCandidates[Index];
			const TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
			Candidate->SetStringField(
				TEXT("name"),
				Match.Name.IsEmpty() ? Match.DisplayName : Match.Name);
			Candidate->SetStringField(TEXT("id"), Match.GetPublicId());
			Candidate->SetStringField(
				TEXT("display_name"),
				Match.DisplayName.IsEmpty() ? Match.Name : Match.DisplayName);
			Candidate->SetStringField(TEXT("kind"), Match.Kind);
			if (!Match.CategoryPath.IsEmpty())
			{
				Candidate->SetStringField(
					TEXT("category"), Match.CategoryPath);
			}
			if (const FString* ComponentClass =
				Match.AuthoringSelectors.Find(TEXT("component_class")))
			{
				Candidate->SetStringField(TEXT("class_path"), *ComponentClass);
			}
			if (const FString* AssetOverride =
				Match.AuthoringSelectors.Find(TEXT("asset_override")))
			{
				Candidate->SetStringField(TEXT("asset_path"), *AssetOverride);
			}
			Candidates.Add(MakeShared<FJsonValueObject>(Candidate));
		}
		Payload->SetArrayField(TEXT("candidates"), MoveTemp(Candidates));
		Payload->SetArrayField(
			TEXT("existing_components"), BuildExistingComponents(*Blueprint));
		OutPayloadJson = Canonical(Payload);
		return true;
	}

	bool TryQueryBlueprintComponentCandidates(
		const FString& Target,
		const FString& ComponentClass,
		const FString& RequestedName,
		FString& OutPayloadJson,
		FUeAgentBlueprintComponentMechanicsError& OutError)
	{
		OutPayloadJson.Reset();
		OutError = {};
		if (!IsValidComponentName(RequestedName))
		{
			return Fail(
				OutError,
				TEXT("blueprint_component_name_invalid"),
				RequestedName);
		}
		UClass* Class = ResolveSupportedClass(ComponentClass, OutError);
		UBlueprint* Blueprint = Class ? LoadBlueprint(Target, OutError) : nullptr;
		if (!Class || !Blueprint)
		{
			return false;
		}
		FString ScsRevision;
		FString ReadbackEvidence;
		if (!TryCaptureScs(
				*Blueprint, ScsRevision, ReadbackEvidence, OutError))
		{
			return false;
		}
		const bool bAvailable = !FindNodeByName(*Blueprint, RequestedName);
		const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(
			TEXT("command_id"), TEXT("blueprint.component.candidates"));
		Payload->SetBoolField(TEXT("read_only"), true);
		Payload->SetStringField(TEXT("target"), Target);
		Payload->SetStringField(TEXT("scs_revision"), ScsRevision);
		Payload->SetStringField(TEXT("readback_evidence_id"), ReadbackEvidence);
		Payload->SetBoolField(TEXT("candidate_available"), bAvailable);
		TArray<TSharedPtr<FJsonValue>> Candidates;
		if (bAvailable)
		{
			const TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
			Candidate->SetStringField(TEXT("name"), RequestedName);
			Candidate->SetStringField(
				TEXT("component_class"), ComponentClass);
			Candidate->SetStringField(
				TEXT("binding_revision"),
				BindingRevision(
					Target,
					ComponentClass,
					RequestedName,
					ScsRevision));
			Candidates.Add(MakeShared<FJsonValueObject>(Candidate));
		}
		Payload->SetArrayField(TEXT("candidates"), MoveTemp(Candidates));
		Payload->SetArrayField(
			TEXT("existing_components"), BuildExistingComponents(*Blueprint));
		Payload->SetBoolField(TEXT("complete"), true);
		Payload->SetBoolField(TEXT("has_more"), false);
		OutPayloadJson = Canonical(Payload);
		return true;
	}

}
