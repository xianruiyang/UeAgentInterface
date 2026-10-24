#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringOps.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"

#include "BlueprintEditorLibrary.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Editor.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "SubobjectDataSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace UeAgentBlueprintAuthoringOps
{
	bool TryCreateGraphConnection(const UEdGraphSchema& Schema, UEdGraphPin* From, UEdGraphPin* To, FDiagnostic& OutError)
	{
		OutError.Reset();
		const FString FromName = From ? From->PinName.ToString() : FString(), ToName = To ? To->PinName.ToString() : FString();
		const FEdGraphPinType FromType = From ? From->PinType : FEdGraphPinType();
		const FEdGraphPinType ToType = To ? To->PinType : FEdGraphPinType();
		const TWeakObjectPtr<UEdGraphNode> FromNode = From ? From->GetOwningNode() : nullptr;
		const TWeakObjectPtr<UEdGraphNode> ToNode = To ? To->GetOwningNode() : nullptr;
		const UBlueprint* Blueprint = FromNode.IsValid() ? FBlueprintEditorUtils::FindBlueprintForNode(FromNode.Get())
			: ToNode.IsValid() ? FBlueprintEditorUtils::FindBlueprintForNode(ToNode.Get()) : nullptr;
		const FString TargetPath = Blueprint ? UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint) : FString();
		const FString SchemaClass = Schema.GetClass()->GetPathName();
		const auto Reject = [&](const TCHAR* Code, const FString& Reason) {
			OutError = FDiagnostic::Failure(Code, Reason, TEXT("native_connection"));
			OutError.SourceKind = TEXT("native_result"); OutError.TargetPath = TargetPath;
			OutError.PropertyPath = TEXT("/connection"); OutError.ActualValuePresent = true;
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false); OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			const auto Facts = MakeShared<FJsonObject>();
			Facts->SetStringField(TEXT("schema_class"), SchemaClass);
			Facts->SetStringField(TEXT("from_pin"), FromName); Facts->SetStringField(TEXT("to_pin"), ToName);
			if (!FromName.IsEmpty()) Facts->SetObjectField(TEXT("from_type"), BuildPinTypeJsonObject(FromType, Blueprint, TargetPath));
			if (!ToName.IsEmpty()) Facts->SetObjectField(TEXT("to_type"), BuildPinTypeJsonObject(ToType, Blueprint, TargetPath));
			if (FromNode.IsValid()) Facts->SetStringField(TEXT("from_node"), FromNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
			if (ToNode.IsValid()) Facts->SetStringField(TEXT("to_node"), ToNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
			OutError.ActualEvidence = MakeShared<FJsonValueObject>(Facts); return false;
		};
		if (!From || !To) return Reject(TEXT("blueprint_edge_endpoint_unavailable"),
			!From ? TEXT("The requested source pin is unavailable.") : TEXT("The requested destination pin is unavailable."));
		const FEdGraphPinReference FromRef(From), ToRef(To);
		const auto ConnectionResponse = Schema.CanCreateConnection(From, To);
		if (ConnectionResponse.Response == CONNECT_RESPONSE_MAKE_WITH_PROMOTION)
		{
			// Preserve K2's replacement/multiple-self semantics for explicit edges.
			if (const auto* K2Schema = Cast<UEdGraphSchema_K2>(&Schema))
			{
				const UEdGraphPin* Input = From->Direction == EGPD_Input ? From : To;
				const UEdGraphPin* Output = From->Direction == EGPD_Output ? From : To;
				const auto Multiplicity = K2Schema->DetermineConnectionResponseOfCompatibleTypedPins(From, To, Input, Output).Response;
				if (Multiplicity == CONNECT_RESPONSE_BREAK_OTHERS_A || Multiplicity == CONNECT_RESPONSE_BREAK_OTHERS_AB)
					Schema.BreakPinLinks(*From, true);
				if (Multiplicity == CONNECT_RESPONSE_BREAK_OTHERS_B || Multiplicity == CONNECT_RESPONSE_BREAK_OTHERS_AB)
					if (UEdGraphPin* CurrentTo = ToRef.Get()) Schema.BreakPinLinks(*CurrentTo, true);
			}
		}
		From = FromRef.Get(); To = ToRef.Get();
		if (!From || !To) return Reject(TEXT("blueprint_edge_endpoint_unavailable"),
			TEXT("A requested pin is no longer available after the native connection updates."));
		if (!Schema.TryCreateConnection(From, To)) {
			Reject(TEXT("blueprint_edge_connection_rejected"), TEXT("The native graph schema did not create the requested pin connection."));
			if (ConnectionResponse.Response == CONNECT_RESPONSE_DISALLOW && !ConnectionResponse.Message.IsEmpty()) {
				auto Cause = FDiagnostic::Failure(TEXT("blueprint_edge_schema_disallowed"), ConnectionResponse.Message.ToString(), TEXT("native_connection_check"));
				Cause.SourceKind = TEXT("native_result"); Cause.TargetPath = TargetPath; Cause.PropertyPath = TEXT("/connection");
				OutError.PublicCause = MakeShared<FDiagnostic>(MoveTemp(Cause));
			}
			return false;
		}
		// Notify only after schema pin callbacks have returned.
		if (FromNode.IsValid()) FromNode->NodeConnectionListChanged();
		if (ToNode.IsValid() && ToNode != FromNode) ToNode->NodeConnectionListChanged();
		return true;
	}

	static FString NormalizeAssetPath(const FString& InPath)
	{
		FString OutPath = InPath;
		OutPath.TrimStartAndEndInline();
		int32 DotIndex = INDEX_NONE;
		if (OutPath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			OutPath = OutPath.Left(DotIndex);
		}
		return OutPath;
	}

	static FString ToObjectPath(const FString& InPath)
	{
		FString InObjectPath = InPath;
		InObjectPath.TrimStartAndEndInline();
		if (InObjectPath.IsEmpty()) return FString();
		if (InObjectPath.Contains(TEXT("."))) return InObjectPath;
		const FString AssetPath = NormalizeAssetPath(InObjectPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (AssetName.IsEmpty()) return FString();
		return FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
	}

	USCS_Node* FindSCSNodeByAnyName(USimpleConstructionScript* SCS, const FString& InComponentName)
	{
		if (!SCS || InComponentName.IsEmpty()) return nullptr;
		if (USCS_Node* Node = SCS->FindSCSNode(FName(*InComponentName))) return Node;
		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (!Node) continue;
			if (Node->GetVariableName().ToString().Equals(InComponentName, ESearchCase::IgnoreCase)
				|| (Node->ComponentTemplate && Node->ComponentTemplate->GetName().Equals(InComponentName, ESearchCase::IgnoreCase)))
			{
				return Node;
			}
		}
		return nullptr;
	}

	void GatherInheritedComponents(UBlueprint& Blueprint, TArray<FInheritedComponent>& OutComponents)
	{
		OutComponents.Reset();
		auto* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint.GeneratedClass);
		if (!GeneratedClass) return;
		TSet<FName> Names;
		if (Blueprint.SimpleConstructionScript)
			for (const auto* Node : Blueprint.SimpleConstructionScript->GetAllNodes())
				if (Node) Names.Add(Node->GetVariableName());
		// SCS variables need not have CDO values. Their native template owner,
		// including the closest derived override, is USCS_Node.
		for (UClass* ParentClass = Blueprint.ParentClass; ParentClass; ParentClass = ParentClass->GetSuperClass())
		{
			const auto* ParentBlueprint = Cast<UBlueprint>(ParentClass->ClassGeneratedBy);
			USimpleConstructionScript* SCS = ParentBlueprint ? ParentBlueprint->SimpleConstructionScript.Get() : nullptr;
			if (!SCS) continue;
			for (USCS_Node* Node : SCS->GetAllNodes())
			{
				if (!Node || Names.Contains(Node->GetVariableName())) continue;
				UActorComponent* Template = Node->GetActualComponentTemplate(GeneratedClass);
				if (!Template) continue;
				FInheritedComponent Entry;
				Entry.Component = Template;
				Entry.SourceNode = Node;
				Entry.Name = Node->GetVariableName().ToString();
				const USCS_Node* Parent = SCS->FindParentNode(Node);
				Entry.ParentName = Parent ? Parent->GetVariableName().ToString()
					: Node->ParentComponentOrVariableName.IsNone() ? FString() : Node->ParentComponentOrVariableName.ToString();
				Names.Add(Node->GetVariableName());
				OutComponents.Add(MoveTemp(Entry));
			}
		}
		TMap<UActorComponent*, int32> Templates;
		for (int32 Index = 0; Index < OutComponents.Num(); ++Index) Templates.Add(OutComponents[Index].Component, Index);
		UObject* CDO = GeneratedClass->GetDefaultObject(false);
		for (TFieldIterator<FObjectPropertyBase> It(GeneratedClass, EFieldIteratorFlags::IncludeSuper); CDO && It; ++It)
		{
			if (!It->PropertyClass || !It->PropertyClass->IsChildOf(UActorComponent::StaticClass())
				|| Names.Contains(It->GetFName())) continue;
			if (auto* Component = Cast<UActorComponent>(It->GetObjectPropertyValue_InContainer(CDO)))
			{
				// Multiple reflected members can reference one native subobject
				// (for example StaticMeshComponent and RootComponent).
				if (const int32* Existing = Templates.Find(Component))
				{
					OutComponents[*Existing].Aliases.AddUnique(It->GetName());
					continue;
				}
				FInheritedComponent Entry;
				Entry.Component = Component;
				Entry.Name = It->GetName();
				Names.Add(It->GetFName());
				Templates.Add(Component, OutComponents.Num());
				OutComponents.Add(MoveTemp(Entry));
			}
		}
		for (auto& Entry : OutComponents)
			if (!Entry.SourceNode)
				if (const auto* Scene = Cast<USceneComponent>(Entry.Component))
					for (const auto& Parent : OutComponents)
						if (Parent.Component == Scene->GetAttachParent()) { Entry.ParentName = Parent.Name; break; }
		OutComponents.Sort([](const auto& A, const auto& B) { return A.Name < B.Name; });
	}

	UActorComponent* ResolveInheritedComponent(UBlueprint& Blueprint, const FString& InComponentName)
	{
		TArray<FInheritedComponent> Components;
		GatherInheritedComponents(Blueprint, Components);
		for (const auto& Entry : Components)
			if (FName(*Entry.Name) == FName(*InComponentName)
				|| Entry.Aliases.ContainsByPredicate([&InComponentName](const FString& Alias)
					{ return FName(*Alias) == FName(*InComponentName); })) return Entry.Component;
		return nullptr;
	}

	static FSubobjectDataHandle FindComponentHandle(USubobjectDataSubsystem& Subobjects,
		UBlueprint& Blueprint, const FSubobjectDataHandle& RootHandle,
		const TArray<FSubobjectDataHandle>& Handles, UActorComponent* Component);

	static FDiagnostic ComponentFailure(UBlueprint& Blueprint, const FString& Code, const FString& Reason,
		const FString& Name, const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
	{
		FDiagnostic Error = FDiagnostic::Failure(Code, Reason, TEXT("component_edit"));
		Error.SourceKind = TEXT("native_result");
		Error.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
		Error.CandidateName = Name;
		Error.PropertyPath = Field;
		Error.ActualValue = MoveTemp(Actual);
		Error.ExpectedValue = MoveTemp(Expected);
		return Error;
	}

	UActorComponent* ResolveEditableInheritedComponent(UBlueprint& Blueprint,
		const FString& InComponentName, FDiagnostic& OutError)
	{
		TArray<FInheritedComponent> Components;
		GatherInheritedComponents(Blueprint, Components);
		for (const auto& Entry : Components)
			if (FName(*Entry.Name) == FName(*InComponentName)
				|| Entry.Aliases.ContainsByPredicate([&InComponentName](const FString& Alias)
					{ return FName(*Alias) == FName(*InComponentName); }))
			{
				auto* Subobjects = USubobjectDataSubsystem::Get();
				TArray<FSubobjectDataHandle> Handles;
				if (Subobjects) Subobjects->K2_GatherSubobjectDataForBlueprint(&Blueprint, Handles);
				FSubobjectDataHandle RootHandle;
				for (const auto& Handle : Handles)
					if (const auto* Data = Handle.GetData(); Data && Data->IsRootActor()) { RootHandle = Handle; break; }
				const auto Handle = Subobjects && RootHandle.IsValid()
					? FindComponentHandle(*Subobjects, Blueprint, RootHandle, Handles, Entry.Component)
					: FSubobjectDataHandle::InvalidHandle;
				const auto* Data = Handle.GetData();
				const bool bEditable = Data && Data->CanEdit();
				if (!bEditable)
				{
					OutError = ComponentFailure(Blueprint, TEXT("blueprint_inherited_component_not_editable"),
						FString::Printf(TEXT("Inherited component '%s' %s."), *Entry.Name, Data ? TEXT("is not editable in this Blueprint") : TEXT("has no native editor data")),
						Entry.Name, TEXT("editable"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return nullptr;
				}
				Blueprint.Modify();
				if (auto* Handler = Blueprint.GetInheritableComponentHandler(false)) Handler->Modify();
				if (auto* Editable = Data->GetObjectForBlueprint<UActorComponent>(&Blueprint))
					return const_cast<UActorComponent*>(Editable);
				OutError = ComponentFailure(Blueprint, TEXT("blueprint_inherited_component_override_failed"),
					FString::Printf(TEXT("The native editor did not provide an editable override for inherited component '%s'."), *Entry.Name),
					Entry.Name, TEXT("editable_override"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return nullptr;
			}
		OutError = ComponentFailure(Blueprint, TEXT("blueprint_component_not_found"),
			FString::Printf(TEXT("Inherited component '%s' was not found by name or alias."), *InComponentName),
			InComponentName, TEXT("component"), MakeShared<FJsonValueString>(InComponentName),
			MakeShared<FJsonValueString>(TEXT("an existing inherited component name or alias")));
		return nullptr;
	}

	bool TryChangeInheritedComponentClass(UBlueprint& Blueprint, const FString& Name,
		UClass& NewClass, FDiagnostic& OutError)
	{
		auto* Component = ResolveInheritedComponent(Blueprint, Name);
		auto* Subobjects = USubobjectDataSubsystem::Get();
		TArray<FSubobjectDataHandle> Handles;
		if (Subobjects) Subobjects->K2_GatherSubobjectDataForBlueprint(&Blueprint, Handles);
		FSubobjectDataHandle RootHandle;
		for (const auto& Handle : Handles)
			if (const auto* Data = Handle.GetData(); Data && Data->IsRootActor()) { RootHandle = Handle; break; }
		const auto Handle = Component && Subobjects && RootHandle.IsValid()
			? FindComponentHandle(*Subobjects, Blueprint, RootHandle, Handles, Component)
			: FSubobjectDataHandle::InvalidHandle;
		const auto* Data = Handle.GetData();
		const bool bOverridesAllowed = GetAllowNativeComponentClassOverrides();
		if (!bOverridesAllowed || !Data || !Data->IsNativeComponent())
		{
			OutError = ComponentFailure(Blueprint, TEXT("blueprint_inherited_component_structure_not_editable"),
				FString::Printf(TEXT("Cannot change class of inherited component '%s' to '%s': %s."), *Name, *NewClass.GetPathName(),
					!bOverridesAllowed ? TEXT("native component class overrides are disabled") : !Data ? TEXT("the native component context is unavailable") : TEXT("the component is not native")),
				Name, TEXT("class"), Component ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Component->GetClass()->GetPathName())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
				MakeShared<FJsonValueString>(NewClass.GetPathName()));
			return false;
		}
		if (!Subobjects->ChangeSubobjectClass(Handle, &NewClass))
		{
			OutError = ComponentFailure(Blueprint, TEXT("blueprint_component_native_class_override_failed"),
				FString::Printf(TEXT("The native editor rejected class override of component '%s' to '%s'."), *Name, *NewClass.GetPathName()),
				Name, TEXT("class"), Component ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Component->GetClass()->GetPathName())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
				MakeShared<FJsonValueString>(NewClass.GetPathName()));
			return false;
		}
		return true;
	}

	void CollectComponentRemovalDependents(UBlueprint& Blueprint, const USCS_Node& Node,
		TArray<UBlueprint*>& OutBlueprints, TArray<AActor*>& OutActors)
	{
		if (!Blueprint.GeneratedClass || !Node.ComponentTemplate) return;
		auto AddInstances = [&](UActorComponent* Template) {
			TArray<UObject*> Instances;
			Template->GetArchetypeInstances(Instances);
			for (UObject* Object : Instances) if (const auto* Component = Cast<UActorComponent>(Object))
				if (AActor* Actor = Component->GetOwner(); Actor && !Actor->IsTemplate()) OutActors.AddUnique(Actor);
		};
		AddInstances(Node.ComponentTemplate);
		// Match DeleteSubobjects: only loaded derived templates are changed by
		// this native operation. Do not load arbitrary asset referencers.
		TArray<UClass*> Classes;
		GetDerivedClasses(Blueprint.GeneratedClass, Classes);
		for (UClass* Class : Classes)
		{
			const UBlueprint* Owner = Cast<UBlueprint>(Class->ClassGeneratedBy);
			if (Owner && UeAgentBlueprintAssetLifecycle::IsAuthoringWorkingCopy(*Owner)) continue;
			if (auto* Template = Cast<UActorComponent>(FindObjectWithOuter(Class, UActorComponent::StaticClass(), Node.ComponentTemplate->GetFName())))
			{
				if (UBlueprint* Dependent = Cast<UBlueprint>(Class->ClassGeneratedBy))
					OutBlueprints.AddUnique(Dependent);
				AddInstances(Template);
			}
		}
	}

	bool TryRemoveBlueprintComponentNode(UBlueprint& Blueprint, USCS_Node& Node, FDiagnostic& OutError)
	{
		USubobjectDataSubsystem* SubobjectSystem = USubobjectDataSubsystem::Get();
		if (!SubobjectSystem)
		{
			OutError = ComponentFailure(Blueprint, TEXT("blueprint_component_native_owner_unavailable"),
				TEXT("The native subobject subsystem is unavailable for component removal."),
				Node.GetVariableName().ToString(), TEXT("subobject_subsystem"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		TArray<FSubobjectDataHandle> Handles;
		SubobjectSystem->K2_GatherSubobjectDataForBlueprint(&Blueprint, Handles);
		for (const FSubobjectDataHandle& Handle : Handles)
		{
			const FSubobjectData* Data = Handle.GetData();
			if (!Data || Data->GetComponentTemplate() != Node.ComponentTemplate.Get()
				|| Data->GetBlueprint() != &Blueprint) continue;
			const FString Name = Node.GetVariableName().ToString();
			const int32 DeletedCount = SubobjectSystem->DeleteSubobject(Handles[0], Handle, &Blueprint);
			const bool bNodeRetained = DeletedCount == 1 && Blueprint.SimpleConstructionScript->GetAllNodes().Contains(&Node);
			if (DeletedCount != 1 || bNodeRetained)
			{
				OutError = ComponentFailure(Blueprint, TEXT("blueprint_component_native_remove_failed"),
					FString::Printf(TEXT("Component '%s' removal %s."), *Name, DeletedCount != 1
						? TEXT("did not delete exactly one native subobject") : TEXT("left the component node in the construction script")),
					Name, DeletedCount != 1 ? TEXT("deleted_count") : TEXT("node_present"),
					DeletedCount != 1 ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(DeletedCount)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)),
					DeletedCount != 1 ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(1)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)));
				return false;
			}
			return true;
		}
		OutError = ComponentFailure(Blueprint, TEXT("blueprint_component_native_context_unresolved"),
			TEXT("No native subobject owned by this Blueprint matched the component template for removal."),
			Node.GetVariableName().ToString(), TEXT("native_component_context"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		return false;
	}

	bool TryRenameBlueprintComponentNode(UBlueprint& Blueprint, USCS_Node& Node,
		const FString& RequestedName, FDiagnostic& OutError)
	{
		const FString Name = RequestedName.TrimStartAndEnd();
		if (!Name.IsEmpty() && Node.GetVariableName() == FName(*Name)) return true;
		USubobjectDataSubsystem* SubobjectSystem = USubobjectDataSubsystem::Get();
		if (!SubobjectSystem)
		{
			OutError = ComponentFailure(Blueprint, TEXT("blueprint_component_native_owner_unavailable"),
				TEXT("The native subobject subsystem is unavailable for component rename."),
				Node.GetVariableName().ToString(), TEXT("subobject_subsystem"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		TArray<FSubobjectDataHandle> Handles;
		SubobjectSystem->K2_GatherSubobjectDataForBlueprint(&Blueprint, Handles);
		for (const FSubobjectDataHandle& Handle : Handles)
		{
			const FSubobjectData* Data = Handle.GetData();
			if (!Data || Data->GetComponentTemplate() != Node.ComponentTemplate.Get()) continue;
			FText Reason;
			const FText NewName = FText::FromString(Name);
			const bool bValidRename = SubobjectSystem->IsValidRename(Handle, NewName, Reason);
			if (!bValidRename
				|| !SubobjectSystem->RenameSubobject(Handle, NewName))
			{
				OutError = ComponentFailure(Blueprint, TEXT("blueprint_component_native_rename_failed"),
					FString::Printf(TEXT("The native editor %s component rename from '%s' to '%s'."), bValidRename ? TEXT("could not apply") : TEXT("rejected"),
						*Node.GetVariableName().ToString(), *Name),
					Node.GetVariableName().ToString(), TEXT("name"), MakeShared<FJsonValueString>(Node.GetVariableName().ToString()), MakeShared<FJsonValueString>(Name));
				if (!Reason.IsEmpty())
				{
					auto Cause = MakeShared<FDiagnostic>(FDiagnostic::Failure(TEXT("blueprint_component_rename_validation_failed"), Reason.ToString()));
					Cause->SourceKind = TEXT("native_result");
					OutError.PublicCause = MoveTemp(Cause);
				}
				return false;
			}
			if (Node.GetVariableName() != FName(*Name))
			{
				OutError = ComponentFailure(Blueprint, TEXT("blueprint_component_native_rename_mismatch"),
					TEXT("The component name read back after native rename differs from the requested name."),
					Node.GetVariableName().ToString(), TEXT("name"), MakeShared<FJsonValueString>(Node.GetVariableName().ToString()), MakeShared<FJsonValueString>(Name));
				return false;
			}
			return true;
		}
		OutError = ComponentFailure(Blueprint, TEXT("blueprint_component_native_context_unresolved"),
			TEXT("No native subobject matched the component template for rename."),
			Node.GetVariableName().ToString(), TEXT("native_component_context"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		return false;
	}

	static FSubobjectDataHandle FindComponentHandle(USubobjectDataSubsystem& Subobjects,
		UBlueprint& Blueprint, const FSubobjectDataHandle& RootHandle,
		const TArray<FSubobjectDataHandle>& Handles, UActorComponent* Component)
	{
		const auto Direct = Subobjects.FindHandleForObject(RootHandle, Component, &Blueprint);
		if (Direct.IsValid() || !Component) return Direct;
		// The cached object can be the ancestor template while editing uses a
		// derived override. Resolve that native template without creating overrides.
		UActorComponent* SourceTemplate = Component;
		UActorComponent* ParentTemplate = Component;
		TArray<FInheritedComponent> Inherited;
		GatherInheritedComponents(Blueprint, Inherited);
		for (const auto& Entry : Inherited)
			if (Entry.Component == Component && Entry.SourceNode)
			{
				SourceTemplate = Entry.SourceNode->ComponentTemplate;
				if (auto* ParentClass = Cast<UBlueprintGeneratedClass>(Blueprint.ParentClass))
					ParentTemplate = Entry.SourceNode->GetActualComponentTemplate(ParentClass);
				break;
			}
		for (const auto& Handle : Handles)
			if (const auto* Data = Handle.GetData(); Data && Data->IsComponent())
			{
				const auto* Template = Data->GetComponentTemplate();
				if (Template == Component || Template == SourceTemplate || Template == ParentTemplate) return Handle;
			}
		return FSubobjectDataHandle::InvalidHandle;
	}

	bool TryCreateBlueprintComponentNode(
		UBlueprint& Blueprint,
		USCS_Node* Parent,
		UClass& ComponentClass,
		const FString& RequestedName,
		UObject* AssetOverride,
		const TFunction<void(const FSubobjectDataHandle&)>& OnCreated,
		USCS_Node*& OutNode,
		FDiagnostic& OutError,
		USceneComponent* InheritedParent)
	{
		OutNode = nullptr;
		OutError.Reset();
		USimpleConstructionScript* SCS = Blueprint.SimpleConstructionScript;
		USubobjectDataSubsystem* SubobjectSystem = USubobjectDataSubsystem::Get();
		if (!SCS || !SubobjectSystem)
		{
			OutError = ComponentFailure(Blueprint, TEXT("blueprint_component_native_owner_unavailable"),
				!SCS ? TEXT("The Blueprint has no construction script for component creation.") : TEXT("The native subobject subsystem is unavailable for component creation."),
				RequestedName, !SCS ? TEXT("construction_script") : TEXT("subobject_subsystem"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}

		TArray<FSubobjectDataHandle> Handles;
		SubobjectSystem->K2_GatherSubobjectDataForBlueprint(&Blueprint, Handles);
		FSubobjectDataHandle RootHandle = FSubobjectDataHandle::InvalidHandle;
		FSubobjectDataHandle ParentHandle = FSubobjectDataHandle::InvalidHandle;
		FSubobjectDataHandle NamedParentHandle = FSubobjectDataHandle::InvalidHandle;
		int32 ParentMatches = 0;
		for (const FSubobjectDataHandle& Handle : Handles)
		{
			const FSubobjectData* Data = Handle.GetData();
			if (!Data) continue;
			if (!RootHandle.IsValid() && Data->IsRootActor()) RootHandle = Handle;
			if (Parent && Data->GetVariableName() == Parent->GetVariableName())
			{
				NamedParentHandle = Handle;
				++ParentMatches;
			}
		}
		if (!RootHandle.IsValid())
		{
			OutError = ComponentFailure(Blueprint, TEXT("blueprint_component_root_context_unresolved"),
				TEXT("The native editor returned no root actor context for component creation."),
				RequestedName, TEXT("root_actor_context"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		if (Parent || InheritedParent)
		{
			ParentHandle = FindComponentHandle(*SubobjectSystem, Blueprint, RootHandle,
				Handles, Parent ? Parent->ComponentTemplate.Get() : InheritedParent);
			if (!ParentHandle.IsValid() && ParentMatches == 1)
			{
				ParentHandle = NamedParentHandle;
			}
			if (!ParentHandle.IsValid())
			{
				const FString ParentName = Parent ? Parent->GetVariableName().ToString() : InheritedParent->GetName();
				OutError = ComponentFailure(Blueprint, ParentMatches == 0
					? TEXT("blueprint_component_parent_context_unresolved") : TEXT("blueprint_component_parent_context_ambiguous"),
					FString::Printf(TEXT("Cannot resolve parent '%s' for component '%s': direct template lookup failed and %d local names matched."),
						*ParentName, *RequestedName, ParentMatches),
					RequestedName, TEXT("parent"), MakeShared<FJsonValueNumber>(ParentMatches), MakeShared<FJsonValueNumber>(1));
				return false;
			}
		}
		else
		{
			ParentHandle = RootHandle;
		}

		FAddNewSubobjectParams AddParams;
		AddParams.ParentHandle = ParentHandle;
		AddParams.NewClass = &ComponentClass;
		AddParams.AssetOverride = AssetOverride;
		AddParams.BlueprintContext = &Blueprint;
		FText FailReason;
		const FSubobjectDataHandle NewHandle =
			SubobjectSystem->AddNewSubobject(AddParams, FailReason);
		if (!NewHandle.IsValid())
		{
			OutError = ComponentFailure(Blueprint, TEXT("blueprint_component_native_create_failed"),
				FString::Printf(TEXT("The native editor could not create component '%s' of class '%s'."), *RequestedName, *ComponentClass.GetPathName()),
				RequestedName, TEXT("component_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			if (!FailReason.IsEmpty())
			{
				auto Cause = MakeShared<FDiagnostic>(FDiagnostic::Failure(TEXT("blueprint_component_native_create_rejected"), FailReason.ToString()));
				Cause->SourceKind = TEXT("native_result");
				OutError.PublicCause = MoveTemp(Cause);
			}
			return false;
		}
		if (OnCreated) OnCreated(NewHandle);

		const FString TrimmedRequestedName = RequestedName.TrimStartAndEnd();
		const FSubobjectData* NewData = NewHandle.GetData();
		const FString CurrentName = NewData
			? NewData->GetVariableName().ToString()
			: FString();
		if (TrimmedRequestedName.IsEmpty()
			|| (!CurrentName.Equals(
					TrimmedRequestedName, ESearchCase::CaseSensitive)
				&& !SubobjectSystem->RenameSubobject(
					NewHandle, FText::FromString(TrimmedRequestedName))))
		{
			SubobjectSystem->DeleteSubobject(RootHandle, NewHandle, &Blueprint);
			OutError = ComponentFailure(Blueprint, TEXT("blueprint_component_native_rename_failed"),
				TrimmedRequestedName.IsEmpty() ? TEXT("The new component name is empty after trimming.")
					: TEXT("The native editor could not assign the requested name to the new component."),
				RequestedName, TEXT("name"), MakeShared<FJsonValueString>(CurrentName),
				MakeShared<FJsonValueString>(TrimmedRequestedName.IsEmpty() ? TEXT("a non-empty component name") : TrimmedRequestedName));
			return false;
		}

		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (Node && Node->GetVariableName().ToString().Equals(
					TrimmedRequestedName, ESearchCase::CaseSensitive))
			{
				OutNode = Node;
				break;
			}
		}
		if (!OutNode)
		{
			SubobjectSystem->DeleteSubobject(RootHandle, NewHandle, &Blueprint);
			OutError = ComponentFailure(Blueprint, TEXT("blueprint_component_native_result_unresolved"),
				FString::Printf(TEXT("The construction script contains no node with the exact requested name '%s' after component creation."), *TrimmedRequestedName),
				RequestedName, TEXT("component_node"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		return true;
	}

	bool TrySetBlueprintComponentParent(UBlueprint& Blueprint, USCS_Node& Node,
		USCS_Node* Parent, USceneComponent* InheritedParent, FDiagnostic& OutError)
	{
		OutError.Reset();
		USimpleConstructionScript* SCS = Blueprint.SimpleConstructionScript;
		USubobjectDataSubsystem* Subobjects = USubobjectDataSubsystem::Get();
		if (!SCS || !Subobjects || !Node.ComponentTemplate)
		{
			OutError = ComponentFailure(Blueprint, TEXT("blueprint_component_native_owner_unavailable"),
				!SCS ? TEXT("The Blueprint has no construction script for component reparenting.")
					: !Subobjects ? TEXT("The native subobject subsystem is unavailable for component reparenting.") : TEXT("The component node has no component template."),
				Node.GetVariableName().ToString(), !SCS ? TEXT("construction_script") : !Subobjects ? TEXT("subobject_subsystem") : TEXT("component_template"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		for (USCS_Node* Ancestor = Parent; Ancestor; Ancestor = SCS->FindParentNode(Ancestor))
		{
			if (Ancestor == &Node)
			{
				OutError = ComponentFailure(Blueprint, TEXT("blueprint_component_parent_cycle"),
					FString::Printf(TEXT("Parent '%s' is the component '%s' itself or one of its descendants."),
						*Parent->GetVariableName().ToString(), *Node.GetVariableName().ToString()),
					Node.GetVariableName().ToString(), TEXT("parent"), MakeShared<FJsonValueString>(Parent->GetVariableName().ToString()),
					MakeShared<FJsonValueString>(TEXT("a parent outside the component's subtree")));
				return false;
			}
		}
		const bool bScene = Node.ComponentTemplate->IsA<USceneComponent>();
		if ((Parent || InheritedParent) && (!bScene || (Parent &&
			(!Parent->ComponentTemplate || !Parent->ComponentTemplate->IsA<USceneComponent>()))))
		{
			const FString ParentName = Parent ? Parent->GetVariableName().ToString() : InheritedParent->GetName();
			OutError = ComponentFailure(Blueprint, TEXT("blueprint_component_parent_not_scene_component"),
				FString::Printf(TEXT("Component '%s' and requested parent '%s' must both be scene components for attachment."),
					*Node.GetVariableName().ToString(), *ParentName),
				Node.GetVariableName().ToString(), TEXT("parent"),
				MakeShared<FJsonValueString>(!bScene ? Node.ComponentTemplate->GetClass()->GetPathName()
					: Parent->ComponentTemplate ? Parent->ComponentTemplate->GetClass()->GetPathName() : TEXT("<missing parent template>")),
				MakeShared<FJsonValueString>(USceneComponent::StaticClass()->GetPathName()));
			return false;
		}
		TArray<FSubobjectDataHandle> Handles;
		Subobjects->K2_GatherSubobjectDataForBlueprint(&Blueprint, Handles);
		FSubobjectDataHandle RootHandle = FSubobjectDataHandle::InvalidHandle;
		for (const FSubobjectDataHandle& Handle : Handles)
		{
			const FSubobjectData* Data = Handle.GetData();
			if (!Data) continue;
			if (Data->IsRootActor()) RootHandle = Handle;
		}
		FSubobjectDataHandle NodeHandle = RootHandle.IsValid()
			? FindComponentHandle(*Subobjects, Blueprint, RootHandle, Handles, Node.ComponentTemplate)
			: FSubobjectDataHandle::InvalidHandle;
		if (!RootHandle.IsValid() || !NodeHandle.IsValid())
		{
			OutError = ComponentFailure(Blueprint, TEXT("blueprint_component_native_context_unresolved"),
				!RootHandle.IsValid() ? TEXT("The native root actor context is unavailable for component reparenting.")
					: TEXT("The native editor could not resolve the component template for reparenting."),
				Node.GetVariableName().ToString(), !RootHandle.IsValid() ? TEXT("root_actor_context") : TEXT("native_component_context"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		if (NodeHandle.GetData()->GetBlueprint() != &Blueprint)
		{
			// The native root operation clones a component from another Blueprint.
			const UBlueprint* NativeOwner = NodeHandle.GetData()->GetBlueprint();
			OutError = ComponentFailure(Blueprint, TEXT("blueprint_component_native_owner_mismatch"),
				TEXT("The native component belongs to another Blueprint; the requested root operation would clone it."),
				Node.GetVariableName().ToString(), TEXT("component_owner"),
				NativeOwner ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*NativeOwner))) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
				MakeShared<FJsonValueString>(UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint)));
			return false;
		}
		const auto ParentHandle = Parent || InheritedParent
			? FindComponentHandle(*Subobjects, Blueprint, RootHandle, Handles,
				Parent ? Parent->ComponentTemplate.Get() : InheritedParent)
			: RootHandle;
		if (!ParentHandle.IsValid())
		{
			const FString ParentName = Parent ? Parent->GetVariableName().ToString() : InheritedParent->GetName();
			OutError = ComponentFailure(Blueprint, TEXT("blueprint_component_parent_context_unresolved"),
				FString::Printf(TEXT("The native editor could not resolve requested parent '%s' for component '%s'."),
					*ParentName, *Node.GetVariableName().ToString()),
				Node.GetVariableName().ToString(), TEXT("parent"), MakeShared<FJsonValueString>(ParentName),
				MakeShared<FJsonValueString>(TEXT("a parent with a native editor context")));
			return false;
		}
		bool bApplied = false;
		if (!Parent && !InheritedParent && bScene)
		{
			const auto CurrentRoot = Subobjects->FindSceneRootForSubobject(RootHandle);
			const FSubobjectData* RootData = CurrentRoot.GetData();
			if (CurrentRoot == NodeHandle) return true;
			// MakeNewSceneRoot asserts these native ownership conditions.
			if (!RootData || (!RootData->IsDefaultSceneRoot() && !RootData->CanReparent())
				|| !NodeHandle.GetData()->CanReparent())
			{
				OutError = FDiagnostic::Failure(TEXT("blueprint_component_native_root_not_replaceable"),
					FString::Printf(TEXT("Cannot make component '%s' the scene root: %s."), *Node.GetVariableName().ToString(),
						!RootData ? TEXT("the current scene root is unavailable")
						: !NodeHandle.GetData()->CanReparent() ? TEXT("the selected component cannot be reparented")
						: TEXT("the current scene root is not the default root and cannot be reparented")), TEXT("component_reparent"));
				OutError.SourceKind = TEXT("native_result");
				OutError.CandidateName = Node.GetVariableName().ToString();
				OutError.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
				OutError.PropertyPath = TEXT("scene_root_replaceable");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				return false;
			}
			bApplied = Subobjects->MakeNewSceneRoot(RootHandle, NodeHandle, &Blueprint);
		}
		else
		{
			// Asset edits preserve the authored relative values; ReparentSubobject
			// additionally needs a preview actor for its world-space transform policy.
			bApplied = Subobjects->AttachSubobject(ParentHandle, NodeHandle);
		}
		if (!bApplied)
		{
			OutError = FDiagnostic::Failure(TEXT("blueprint_component_native_reparent_failed"),
				FString::Printf(TEXT("The native editor could not reparent component '%s' to '%s'."),
					*Node.GetVariableName().ToString(), Parent ? *Parent->GetVariableName().ToString()
					: InheritedParent ? *InheritedParent->GetName() : TEXT("the scene root")), TEXT("component_reparent"));
			OutError.SourceKind = TEXT("native_result");
			OutError.CandidateName = Node.GetVariableName().ToString();
			OutError.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
			OutError.PropertyPath = TEXT("parent");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
		}
		return bApplied;
	}

	UEdGraphPin* FindNodePinByName(UEdGraphNode* Node, const FString& InPinName)
	{
		if (!Node || InPinName.IsEmpty()) return nullptr;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName.ToString().Equals(InPinName, ESearchCase::IgnoreCase)) return Pin;
		}
		return nullptr;
	}

	UObject* LoadAssetObject(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty()) return nullptr;
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath)) return Existing;
		return LoadObject<UObject>(nullptr, *ObjectPath);
	}

	UBlueprint* LoadBlueprintAsset(const FString& InPath)
	{
		return Cast<UBlueprint>(LoadAssetObject(InPath));
	}

	UClass* ResolveClassByPath(const FString& InClassPath)
	{
		FString ClassPath = InClassPath;
		ClassPath.TrimStartAndEndInline();
		if (ClassPath.IsEmpty()) return nullptr;
		if (UClass* Existing = FindObject<UClass>(nullptr, *ClassPath)) return Existing;
		if (UClass* Loaded = LoadObject<UClass>(nullptr, *ClassPath)) return Loaded;
		if (!ClassPath.EndsWith(TEXT("_C")))
		{
			if (UClass* GeneratedClass = LoadObject<UClass>(nullptr, *(ClassPath + TEXT("_C")))) return GeneratedClass;
		}
		return nullptr;
	}

	UEdGraph* ResolveBlueprintGraph(UBlueprint* Blueprint, const FString& InGraphName)
	{
		if (!Blueprint) return nullptr;
		FString GraphName = InGraphName;
		GraphName.TrimStartAndEndInline();
		if (GraphName.IsEmpty() || GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase)) return UBlueprintEditorLibrary::FindEventGraph(Blueprint);
		auto MatchesGraph = [&GraphName](UEdGraph* Graph) { return Graph && (Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase) || Graph->GetPathName().Equals(GraphName, ESearchCase::IgnoreCase)); };
		if (!GraphName.Contains(TEXT(".")))
		{
			if (UEdGraph* Graph = UBlueprintEditorLibrary::FindGraph(Blueprint, FName(*GraphName))) return Graph;
		}
		auto FindFromList = [&MatchesGraph](const TArray<UEdGraph*>& Graphs) -> UEdGraph* { for (UEdGraph* Graph : Graphs) if (MatchesGraph(Graph)) return Graph; return nullptr; };
		if (UEdGraph* Graph = FindFromList(Blueprint->UbergraphPages)) return Graph;
		if (UEdGraph* Graph = FindFromList(Blueprint->FunctionGraphs)) return Graph;
		if (UEdGraph* Graph = FindFromList(Blueprint->MacroGraphs)) return Graph;
		if (UEdGraph* Graph = FindFromList(Blueprint->DelegateSignatureGraphs)) return Graph;
		TArray<UEdGraph*> AllGraphs; Blueprint->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs) if (MatchesGraph(Graph)) return Graph;
		return nullptr;
	}

	bool IsUserConstructionScriptGraph(const UEdGraph* Graph)
	{
		return Graph && Graph->GetName().Equals(TEXT("UserConstructionScript"), ESearchCase::IgnoreCase);
	}

	bool IsUnsafeConstructionScriptPcgCall(const FString& FunctionOwnerClass, const FString& FunctionName)
	{
		const bool bOwnerIsPcgComponent = FunctionOwnerClass.Contains(TEXT("PCGComponent"), ESearchCase::IgnoreCase) && FunctionOwnerClass.Contains(TEXT("PCG"), ESearchCase::IgnoreCase);
		if (bOwnerIsPcgComponent)
		{
			return FunctionName.Equals(TEXT("SetGraph"), ESearchCase::IgnoreCase) || FunctionName.Equals(TEXT("SetGraphInterface"), ESearchCase::IgnoreCase) || FunctionName.Equals(TEXT("Generate"), ESearchCase::IgnoreCase) || FunctionName.Equals(TEXT("GenerateGetTaskId"), ESearchCase::IgnoreCase) || FunctionName.Equals(TEXT("GenerateLocal"), ESearchCase::IgnoreCase) || FunctionName.Equals(TEXT("GenerateLocalGetTaskId"), ESearchCase::IgnoreCase) || FunctionName.Equals(TEXT("Cleanup"), ESearchCase::IgnoreCase) || FunctionName.Equals(TEXT("CleanupLocal"), ESearchCase::IgnoreCase) || FunctionName.Equals(TEXT("Refresh"), ESearchCase::IgnoreCase) || FunctionName.Equals(TEXT("RefreshAfterGraphChanged"), ESearchCase::IgnoreCase) || FunctionName.Equals(TEXT("NotifyPropertiesChangedFromBlueprint"), ESearchCase::IgnoreCase);
		}
		return FunctionOwnerClass.Contains(TEXT("PCGGraphParametersHelpers"), ESearchCase::IgnoreCase) && FunctionName.StartsWith(TEXT("Set"), ESearchCase::IgnoreCase);
	}

	bool RequiresExplicitParentOrEventNode(const FString& FunctionName)
	{
		return FunctionName.Equals(TEXT("ReceiveBeginPlay"), ESearchCase::IgnoreCase);
	}

	bool SaveBlueprintPackage(UBlueprint* Blueprint, FDiagnostic& OutError)
	{
		if (!Blueprint)
		{
			OutError = FDiagnostic::Failure(TEXT("blueprint_not_found"), TEXT("No Blueprint object is available to save."), TEXT("save"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("blueprint");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(UBlueprint::StaticClass()->GetPathName());
			return false;
		}
		if (!UeAgentBlueprintAssetLifecycle::ValidateBlueprintPersistence(*Blueprint, OutError)) return false;
		TArray<UPackage*> PackagesToSave; PackagesToSave.Add(Blueprint->GetOutermost());
		if (!UE::UAI::EditorResources::SavePackages(PackagesToSave, false, OutError)) { OutError = FDiagnostic::WrapFailure(TEXT("save_asset_failed"), OutError); return false; }
		return true;
	}
}
