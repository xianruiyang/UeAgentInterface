#include "DomainAdapters/BlueprintCommands/UeAgentBlueprintCommandMechanicsBoundary.h"

#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Components/ActorComponent.h"
#include "Components/Widget.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::BlueprintCommands
{
	using Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile;

	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("blueprint.asset.create"),
			TEXT("blueprint.component.events.discover"),
		};
		return Ids;
	}

	bool IsQuery(const FString& CommandId)
	{
		return CommandId == TEXT("blueprint.component.events.discover");
	}

	bool TryGetActivationProfiles(
		TArray<FUeAgentGraphCompoundActivationProfile>& OutProfiles,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>* GraphProfiles = nullptr;
		if (!Composition::GraphCompound::TryGetGraphCompoundActivationProfiles(GraphProfiles, OutError)) return false;
		const auto& Canonical = *GraphProfiles;
		for (const FString& CommandId : GetCommandIds())
		{
			const FUeAgentGraphCompoundActivationProfile* Match =
				Canonical.FindByPredicate(
					[&CommandId](const FUeAgentGraphCompoundActivationProfile& Profile)
					{
						return Profile.GetCommandId() == CommandId;
					});
			if (!Match || !Match->IsValid())
			{
				OutProfiles.Reset();
				if (Match) {
					OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("blueprint_command_profile_missing"),
						Match->GetValidationDiagnostic(), TEXT("runtime_factory"), TEXT("A required Blueprint activation profile failed validation."));
				} else {
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("blueprint_command_profile_missing"),
						TEXT("No activation profile is registered for a required Blueprint command."), TEXT("runtime_factory"));
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.PropertyPath = TEXT("activation_profile_registered");
				}
				OutError.InternalContext = CommandId;
				OutError.SourceKind = TEXT("native_result");
				OutError.PublicMessage += TEXT(" Command: ") + CommandId + TEXT(".");
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == GetCommandIds().Num();
	}

	bool FBlueprintComponentEventDiscoveryRequest::IsValid() const
	{
		return !ComponentName.TrimStartAndEnd().IsEmpty();
	}

	namespace
	{
		bool IsHidden(const FMulticastDelegateProperty& Property)
		{
			return Property.HasMetaData(TEXT("HideInDetailPanel"))
				|| Property.HasMetaData(TEXT("HideInDetails"))
				|| Property.HasMetaData(TEXT("DeprecatedProperty"));
		}

		FString DisplayName(const FMulticastDelegateProperty& Property)
		{
			const FString Explicit = Property.GetMetaData(TEXT("DisplayName"));
			return Explicit.IsEmpty()
				? FName::NameToDisplayString(Property.GetName(), false)
				: Explicit;
		}

		bool MatchesQuery(
			const FMulticastDelegateProperty& Property,
			const FString& EventDisplayName,
			const FString& Query)
		{
			return Query.IsEmpty()
				|| Property.GetName().Contains(Query, ESearchCase::IgnoreCase)
				|| EventDisplayName.Contains(Query, ESearchCase::IgnoreCase)
				|| Property.GetMetaData(TEXT("Category")).Contains(
					Query, ESearchCase::IgnoreCase)
				|| Property.GetMetaData(TEXT("ToolTip")).Contains(
					Query, ESearchCase::IgnoreCase);
		}

		UClass* FindScsComponentClass(
			UBlueprint& Blueprint,
			const FName ComponentName)
		{
			if (!Blueprint.SimpleConstructionScript)
			{
				return nullptr;
			}
			UBlueprintGeneratedClass* const GeneratedClass =
				Cast<UBlueprintGeneratedClass>(Blueprint.GeneratedClass.Get());
			for (USCS_Node* const Node
				: Blueprint.SimpleConstructionScript->GetAllNodes())
			{
				if (!Node)
				{
					continue;
				}
				UActorComponent* const Template = GeneratedClass
					? Node->GetActualComponentTemplate(GeneratedClass)
					: Node->ComponentTemplate.Get();
				if (Node->GetVariableName() == ComponentName
					|| (Template && Template->GetFName() == ComponentName))
				{
					return Template ? Template->GetClass()
						: Node->ComponentClass.Get();
				}
			}
			return nullptr;
		}
	}

	bool TryDiscoverComponentEvents(
		UBlueprint& Blueprint,
		const FBlueprintComponentEventDiscoveryRequest& Request,
		FBlueprintComponentEventDiscoveryResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutResult = {};
		OutError.Reset();
		if (!Request.IsValid())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("blueprint_component_event_request_invalid"),
				TEXT("component_name must contain a non-whitespace Blueprint component name."), TEXT("input_decode"));
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-whitespace component name"));
			OutError.ActualValue = MakeShared<FJsonValueString>(Request.ComponentName);
			OutError.PropertyPath = TEXT("component_name");
			OutError.Source = Request.ComponentNameSource;
			OutError.SourceKind = Request.ComponentNameSourceKind;
			OutError.TargetPath = Blueprint.GetOutermost()->GetName();
			return false;
		}
		if (Request.bCompileBeforeQuery || !Blueprint.SkeletonGeneratedClass)
		{
			FKismetEditorUtilities::CompileBlueprint(&Blueprint);
			OutResult.bCompiledBeforeQuery = true;
		}

		const FName ComponentName(*Request.ComponentName);
		FObjectProperty* const ComponentProperty = Blueprint.SkeletonGeneratedClass
			? FindFProperty<FObjectProperty>(
				Blueprint.SkeletonGeneratedClass, ComponentName)
			: nullptr;
		UClass* const TargetClass = ComponentProperty
			? ComponentProperty->PropertyClass.Get()
			: FindScsComponentClass(Blueprint, ComponentName);
		if (!TargetClass)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("blueprint_component_event_component_not_found"),
				TEXT("No component class could be resolved from the Blueprint property or construction-script node for this component name."), TEXT("query_resolve"));
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("Blueprint component with a resolvable class"));
			OutError.ActualValue = MakeShared<FJsonValueString>(Request.ComponentName);
			OutError.PropertyPath = TEXT("component_name");
			OutError.Source = Request.ComponentNameSource;
			OutError.SourceKind = Request.ComponentNameSourceKind;
			OutError.TargetPath = Blueprint.GetOutermost()->GetName();
			return false;
		}

		OutResult.AssetPath = Blueprint.GetOutermost()->GetName();
		OutResult.ComponentName = Request.ComponentName;
		OutResult.TargetClassRef = TargetClass->GetPathName();
		OutResult.bComponentPropertyFound = ComponentProperty != nullptr;
		OutResult.bCanAuthorComponentBoundEvent = ComponentProperty != nullptr;
		TArray<const FMulticastDelegateProperty*> Properties;
		for (TFieldIterator<FMulticastDelegateProperty> It(
			TargetClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			const FMulticastDelegateProperty* const Property = *It;
			if (Property && !Property->HasAnyPropertyFlags(CPF_Parm)
				&& Property->HasAllPropertyFlags(CPF_BlueprintAssignable))
			{
				++OutResult.TotalEventCount;
				Properties.Add(Property);
			}
		}
		Properties.Sort([](
			const FMulticastDelegateProperty& Left,
			const FMulticastDelegateProperty& Right)
		{
			return Left.GetName() < Right.GetName();
		});

		for (const FMulticastDelegateProperty* const Property : Properties)
		{
			const bool bHidden = IsHidden(*Property);
			const FString EventDisplayName = DisplayName(*Property);
			const bool bQueryMatched = MatchesQuery(*Property, EventDisplayName, Request.Query);
			const bool bAlreadyBound = ComponentProperty
				&& FKismetEditorUtilities::FindBoundEventForComponent(
					&Blueprint, Property->GetFName(), ComponentName) != nullptr;
			if ((!Request.bIncludeHidden && bHidden)
				|| (!Request.bIncludeQueryContext && !bQueryMatched)
				|| (Request.bOnlyUnbound && bAlreadyBound))
			{
				continue;
			}
			if (bQueryMatched) ++OutResult.MatchedEventCount;
			FBlueprintComponentEventRecord& Event =
				OutResult.Events.Emplace_GetRef();
			Event.NativeFieldName = Property->GetName();
			Event.EventRef = TEXT("event:component:")
				+ Request.ComponentName + TEXT(":") + Event.NativeFieldName;
			Event.SignatureRef = Property->SignatureFunction
				? Property->SignatureFunction->GetPathName() : FString();
			Event.DisplayName = EventDisplayName;
			Event.Category = Property->GetMetaData(TEXT("Category"));
			Event.bHidden = bHidden;
			Event.bAlreadyBound = bAlreadyBound;
			Event.bQueryMatched = bQueryMatched;
		}
		return true;
	}

	bool TryGetBlueprintComponentNames(
		UBlueprint& Blueprint,
		TArray<FString>& OutComponentNames,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutComponentNames.Reset();
		OutError.Reset();
		TSet<FString> SeenNames;
		auto AddName = [&OutComponentNames, &SeenNames](const FName Name)
		{
			const FString Text = Name.ToString().TrimStartAndEnd();
			if (!Text.IsEmpty() && !SeenNames.Contains(Text))
			{
				SeenNames.Add(Text);
				OutComponentNames.Add(Text);
			}
		};

		if (Blueprint.SkeletonGeneratedClass)
		{
			for (TFieldIterator<FObjectProperty> It(
				Blueprint.SkeletonGeneratedClass, EFieldIteratorFlags::IncludeSuper);
				It; ++It)
			{
				const FObjectProperty* Property = *It;
				if (Property && Property->PropertyClass
					&& ((Property->PropertyClass->IsChildOf(UActorComponent::StaticClass())
						&& Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ExportObject))
						|| Property->PropertyClass->IsChildOf(UWidget::StaticClass())))
				{
					AddName(Property->GetFName());
				}
			}
		}
		if (Blueprint.SimpleConstructionScript)
		{
			for (USCS_Node* Node
				: Blueprint.SimpleConstructionScript->GetAllNodes())
			{
				if (Node)
				{
					AddName(Node->GetVariableName());
				}
			}
		}
		OutComponentNames.Sort();
		return true;
	}
}
