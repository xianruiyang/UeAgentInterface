#include "DomainAdapters/BlueprintCommands/UeAgentBlueprintCommandMechanicsBoundary.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/KismetEditorUtilities.h"
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
		FString& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		const TArray<FUeAgentGraphCompoundActivationProfile>& Canonical =
			Composition::GraphCompound::GetGraphCompoundActivationProfiles();
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
				OutError = TEXT("blueprint_command_profile_missing:") + CommandId;
				return false;
			}
			OutProfiles.Add(*Match);
		}
		return OutProfiles.Num() == GetCommandIds().Num();
	}

	bool FBlueprintComponentEventDiscoveryRequest::IsValid() const
	{
		return !ComponentName.TrimStartAndEnd().IsEmpty()
			&& Limit >= 0 && Limit <= 1000;
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
		FString& OutError)
	{
		OutResult = {};
		OutError.Reset();
		if (!Request.IsValid())
		{
			OutError = TEXT("blueprint_component_event_request_invalid");
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
			OutError = TEXT("blueprint_component_event_component_not_found");
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
			const bool bAlreadyBound = ComponentProperty
				&& FKismetEditorUtilities::FindBoundEventForComponent(
					&Blueprint, Property->GetFName(), ComponentName) != nullptr;
			if ((!Request.bIncludeHidden && bHidden)
				|| !MatchesQuery(*Property, EventDisplayName, Request.Query)
				|| (Request.bOnlyUnbound && bAlreadyBound))
			{
				continue;
			}
			++OutResult.MatchedEventCount;
			if (Request.Limit > 0
				&& OutResult.Events.Num() >= Request.Limit)
			{
				continue;
			}
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
		}
		OutResult.bTruncated = Request.Limit > 0
			&& OutResult.MatchedEventCount > OutResult.Events.Num();
		return true;
	}
}
