// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/Blueprint/UeAgentHttpServer_Blueprint.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"

namespace UeAgentComponentEventQuery
{
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
		FString ObjectPath = InPath;
		ObjectPath.TrimStartAndEndInline();
		if (ObjectPath.IsEmpty())
		{
			return FString();
		}
		if (ObjectPath.Contains(TEXT(".")))
		{
			return ObjectPath;
		}

		const FString AssetPath = NormalizeAssetPath(ObjectPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		return AssetName.IsEmpty() ? FString() : FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
	}

	template <typename AssetType>
	static AssetType* LoadAsset(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		return ObjectPath.IsEmpty() ? nullptr : LoadObject<AssetType>(nullptr, *ObjectPath);
	}

	static FString ClassPath(const UClass* Class)
	{
		return Class ? Class->GetPathName() : FString();
	}

	static FString DelegateDisplayName(const FMulticastDelegateProperty* Property)
	{
		if (!Property)
		{
			return FString();
		}
		const FString MetaDisplayName = Property->GetMetaData(TEXT("DisplayName"));
		return MetaDisplayName.IsEmpty()
			? FName::NameToDisplayString(Property->GetName(), false)
			: MetaDisplayName;
	}

	static bool IsHiddenInDetails(const FMulticastDelegateProperty* Property)
	{
		return Property && (
			Property->HasMetaData(TEXT("HideInDetailPanel")) ||
			Property->HasMetaData(TEXT("HideInDetails")) ||
			Property->HasMetaData(TEXT("DeprecatedProperty")));
	}

	static bool MatchesQuery(const FMulticastDelegateProperty* Property, const FString& DisplayName, const FString& Query)
	{
		if (!Property || Query.IsEmpty())
		{
			return true;
		}
		if (Property->GetName().Contains(Query, ESearchCase::IgnoreCase) ||
			DisplayName.Contains(Query, ESearchCase::IgnoreCase))
		{
			return true;
		}
		const FString Category = Property->GetMetaData(TEXT("Category"));
		const FString ToolTip = Property->GetMetaData(TEXT("ToolTip"));
		return Category.Contains(Query, ESearchCase::IgnoreCase) || ToolTip.Contains(Query, ESearchCase::IgnoreCase);
	}

	static TSharedPtr<FJsonObject> BuildParameterJson(const FProperty* Param)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Param ? Param->GetName() : FString());
		Obj->SetStringField(TEXT("cpp_type"), Param ? Param->GetCPPType() : FString());
		if (Param)
		{
			Obj->SetStringField(TEXT("display_name"), FName::NameToDisplayString(Param->GetName(), false));
			Obj->SetStringField(TEXT("class"), Param->GetClass() ? Param->GetClass()->GetName() : FString());
		}
		return Obj;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildSignatureParameters(const UFunction* SignatureFunction)
	{
		TArray<TSharedPtr<FJsonValue>> Params;
		if (!SignatureFunction)
		{
			return Params;
		}
		for (TFieldIterator<FProperty> It(SignatureFunction); It; ++It)
		{
			FProperty* Param = *It;
			if (!Param || !Param->HasAnyPropertyFlags(CPF_Parm) || Param->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				continue;
			}
			Params.Add(MakeShared<FJsonValueObject>(BuildParameterJson(Param)));
		}
		return Params;
	}

	static TSharedPtr<FJsonObject> BuildAuthoringNodeJson(
		const FString& ComponentName,
		const FMulticastDelegateProperty* DelegateProperty)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("node_type"), TEXT("component_bound_event"));
		Obj->SetStringField(TEXT("component_name"), ComponentName);
		Obj->SetStringField(TEXT("delegate_property_name"), DelegateProperty ? DelegateProperty->GetName() : FString());
		Obj->SetStringField(TEXT("delegate_owner_class"), ClassPath(DelegateProperty ? DelegateProperty->GetOwnerClass() : nullptr));
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildEventJson(
		UBlueprint* Blueprint,
		const FString& ComponentName,
		const FMulticastDelegateProperty* DelegateProperty,
		const bool bCanAuthor)
	{
		const FString DelegateName = DelegateProperty ? DelegateProperty->GetName() : FString();
		const FString DisplayName = DelegateDisplayName(DelegateProperty);
		const UClass* OwnerClass = DelegateProperty ? DelegateProperty->GetOwnerClass() : nullptr;
		const UFunction* SignatureFunction = DelegateProperty ? DelegateProperty->SignatureFunction : nullptr;
		const UK2Node_ComponentBoundEvent* ExistingNode =
			(Blueprint && bCanAuthor && DelegateProperty)
				? FKismetEditorUtilities::FindBoundEventForComponent(Blueprint, DelegateProperty->GetFName(), FName(*ComponentName))
				: nullptr;

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), DelegateName);
		Obj->SetStringField(TEXT("full_name"), OwnerClass ? FString::Printf(TEXT("%s.%s"), *ClassPath(OwnerClass), *DelegateName) : DelegateName);
		Obj->SetStringField(TEXT("display_name"), DisplayName);
		Obj->SetStringField(TEXT("delegate_property_name"), DelegateName);
		Obj->SetStringField(TEXT("delegate_owner_class"), ClassPath(OwnerClass));
		Obj->SetStringField(TEXT("category"), DelegateProperty ? DelegateProperty->GetMetaData(TEXT("Category")) : FString());
		Obj->SetStringField(TEXT("tooltip"), DelegateProperty ? DelegateProperty->GetMetaData(TEXT("ToolTip")) : FString());
		Obj->SetStringField(TEXT("signature_function"), SignatureFunction ? SignatureFunction->GetName() : FString());
		Obj->SetNumberField(TEXT("parameter_count"), SignatureFunction ? SignatureFunction->NumParms : 0);
		Obj->SetArrayField(TEXT("parameters"), BuildSignatureParameters(SignatureFunction));
		Obj->SetBoolField(TEXT("blueprint_assignable"), DelegateProperty ? DelegateProperty->HasAllPropertyFlags(CPF_BlueprintAssignable) : false);
		Obj->SetBoolField(TEXT("hidden_in_detail_panel"), IsHiddenInDetails(DelegateProperty));
		Obj->SetBoolField(TEXT("can_author_component_bound_event"), bCanAuthor);
		Obj->SetBoolField(TEXT("already_bound"), ExistingNode != nullptr);
		if (ExistingNode)
		{
			Obj->SetStringField(TEXT("existing_node_guid"), ExistingNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		}
		Obj->SetObjectField(TEXT("authoring_node"), BuildAuthoringNodeJson(ComponentName, DelegateProperty));
		return Obj;
	}

	static void BuildEventList(
		UBlueprint* Blueprint,
		const FString& ComponentName,
		UClass* TargetClass,
		const bool bCanAuthor,
		const FString& Query,
		const int32 Limit,
		const bool bOnlyUnbound,
		const bool bIncludeHidden,
		TArray<TSharedPtr<FJsonValue>>& OutEvents,
		int32& OutTotalEventCount,
		int32& OutMatchedEventCount)
	{
		OutTotalEventCount = 0;
		OutMatchedEventCount = 0;
		OutEvents.Reset();

		if (!TargetClass)
		{
			return;
		}

		TArray<const FMulticastDelegateProperty*> Properties;
		for (TFieldIterator<FMulticastDelegateProperty> It(TargetClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			const FMulticastDelegateProperty* Property = *It;
			if (!Property || Property->HasAnyPropertyFlags(CPF_Parm) || !Property->HasAllPropertyFlags(CPF_BlueprintAssignable))
			{
				continue;
			}
			++OutTotalEventCount;
			Properties.Add(Property);
		}

		Properties.Sort([](const FMulticastDelegateProperty& A, const FMulticastDelegateProperty& B)
		{
			return A.GetName() < B.GetName();
		});

		for (const FMulticastDelegateProperty* Property : Properties)
		{
			const FString DisplayName = DelegateDisplayName(Property);
			if (!bIncludeHidden && IsHiddenInDetails(Property))
			{
				continue;
			}
			if (!MatchesQuery(Property, DisplayName, Query))
			{
				continue;
			}
			const bool bAlreadyBound = Blueprint && bCanAuthor && FKismetEditorUtilities::FindBoundEventForComponent(Blueprint, Property->GetFName(), FName(*ComponentName));
			if (bOnlyUnbound && bAlreadyBound)
			{
				continue;
			}
			++OutMatchedEventCount;
			if (Limit > 0 && OutEvents.Num() >= Limit)
			{
				continue;
			}
			OutEvents.Add(MakeShared<FJsonValueObject>(BuildEventJson(Blueprint, ComponentName, Property, bCanAuthor)));
		}
	}

	static UClass* FindScsComponentClass(UBlueprint* Blueprint, const FString& ComponentName)
	{
		if (!Blueprint || !Blueprint->SimpleConstructionScript)
		{
			return nullptr;
		}
		const FName ComponentFName(*ComponentName);
		UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass.Get());
		const TArray<USCS_Node*>& Nodes = Blueprint->SimpleConstructionScript->GetAllNodes();
		for (USCS_Node* Node : Nodes)
		{
			if (!Node)
			{
				continue;
			}
			UActorComponent* Template = GeneratedClass
				? Node->GetActualComponentTemplate(GeneratedClass)
				: Node->ComponentTemplate.Get();
			if (Node->GetVariableName() == ComponentFName ||
				(Template && Template->GetFName() == ComponentFName))
			{
				return Template ? Template->GetClass() : Node->ComponentClass.Get();
			}
		}
		return nullptr;
	}
}

bool FUeAgentHttpServer::CmdBlueprintListComponentEvents(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentComponentEventQuery;

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString ComponentName;
	if (!JsonTryGetString(Ctx.Params, TEXT("component_name"), ComponentName) || ComponentName.IsEmpty())
	{
		OutError = TEXT("missing_component_name");
		return false;
	}
	FString Query;
	JsonTryGetString(Ctx.Params, TEXT("query"), Query);
	bool bOnlyUnbound = false;
	JsonTryGetBool(Ctx.Params, TEXT("only_unbound"), bOnlyUnbound);
	bool bIncludeHidden = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_hidden"), bIncludeHidden);
	bool bCompileBeforeQuery = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_before_query"), bCompileBeforeQuery);
	double LimitNumber = 200.0;
	JsonTryGetNumber(Ctx.Params, TEXT("limit"), LimitNumber);
	const int32 Limit = FMath::Max(0, static_cast<int32>(LimitNumber));

	UBlueprint* Blueprint = LoadAsset<UBlueprint>(AssetPathInput);
	if (!Blueprint)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}

	bool bCompiledBeforeQuery = false;
	if (bCompileBeforeQuery || !Blueprint->SkeletonGeneratedClass)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		bCompiledBeforeQuery = true;
	}

	FObjectProperty* ComponentProperty = Blueprint->SkeletonGeneratedClass
		? FindFProperty<FObjectProperty>(Blueprint->SkeletonGeneratedClass, FName(*ComponentName))
		: nullptr;
	UClass* TargetClass = ComponentProperty ? ComponentProperty->PropertyClass.Get() : FindScsComponentClass(Blueprint, ComponentName);
	if (!TargetClass)
	{
		OutError = TEXT("component_not_found");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Events;
	int32 TotalEventCount = 0;
	int32 MatchedEventCount = 0;
	const bool bCanAuthor = ComponentProperty != nullptr;
	BuildEventList(Blueprint, ComponentName, TargetClass, bCanAuthor, Query, Limit, bOnlyUnbound, bIncludeHidden, Events, TotalEventCount, MatchedEventCount);

	OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("target_kind"), TEXT("actor_component"));
	OutData->SetStringField(TEXT("component_name"), ComponentName);
	OutData->SetStringField(TEXT("target_class"), ClassPath(TargetClass));
	OutData->SetBoolField(TEXT("component_property_found"), ComponentProperty != nullptr);
	OutData->SetBoolField(TEXT("can_author_component_bound_event"), bCanAuthor);
	OutData->SetBoolField(TEXT("compiled_before_query"), bCompiledBeforeQuery);
	OutData->SetStringField(TEXT("query"), Query);
	OutData->SetNumberField(TEXT("limit"), Limit);
	OutData->SetNumberField(TEXT("total_event_count"), TotalEventCount);
	OutData->SetNumberField(TEXT("matched_event_count"), MatchedEventCount);
	OutData->SetNumberField(TEXT("returned_event_count"), Events.Num());
	OutData->SetBoolField(TEXT("truncated"), Limit > 0 && MatchedEventCount > Events.Num());
	OutData->SetArrayField(TEXT("events"), Events);
	return true;
}

bool FUeAgentHttpServer::CmdUmgListWidgetEvents(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentComponentEventQuery;

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString WidgetName;
	if (!JsonTryGetString(Ctx.Params, TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
	{
		OutError = TEXT("missing_widget_name");
		return false;
	}
	FString Query;
	JsonTryGetString(Ctx.Params, TEXT("query"), Query);
	bool bOnlyUnbound = false;
	JsonTryGetBool(Ctx.Params, TEXT("only_unbound"), bOnlyUnbound);
	bool bIncludeHidden = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_hidden"), bIncludeHidden);
	bool bCompileBeforeQuery = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_before_query"), bCompileBeforeQuery);
	double LimitNumber = 200.0;
	JsonTryGetNumber(Ctx.Params, TEXT("limit"), LimitNumber);
	const int32 Limit = FMath::Max(0, static_cast<int32>(LimitNumber));

	UWidgetBlueprint* WidgetBlueprint = LoadAsset<UWidgetBlueprint>(AssetPathInput);
	if (!WidgetBlueprint)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}
	if (!WidgetBlueprint->WidgetTree)
	{
		OutError = TEXT("widget_tree_missing");
		return false;
	}

	bool bCompiledBeforeQuery = false;
	if (bCompileBeforeQuery || !WidgetBlueprint->SkeletonGeneratedClass)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		bCompiledBeforeQuery = true;
	}

	UWidget* Widget = WidgetBlueprint->WidgetTree->FindWidget(FName(*WidgetName));
	if (!Widget)
	{
		OutError = TEXT("widget_not_found");
		return false;
	}

	FObjectProperty* WidgetProperty = WidgetBlueprint->SkeletonGeneratedClass
		? FindFProperty<FObjectProperty>(WidgetBlueprint->SkeletonGeneratedClass, FName(*WidgetName))
		: nullptr;
	UClass* TargetClass = WidgetProperty ? WidgetProperty->PropertyClass.Get() : Widget->GetClass();
	const bool bCanAuthor = WidgetProperty != nullptr;

	TArray<TSharedPtr<FJsonValue>> Events;
	int32 TotalEventCount = 0;
	int32 MatchedEventCount = 0;
	BuildEventList(WidgetBlueprint, WidgetName, TargetClass, bCanAuthor, Query, Limit, bOnlyUnbound, bIncludeHidden, Events, TotalEventCount, MatchedEventCount);

	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("target_kind"), TEXT("umg_widget"));
	OutData->SetStringField(TEXT("widget_name"), WidgetName);
	OutData->SetStringField(TEXT("component_name"), WidgetName);
	OutData->SetStringField(TEXT("target_class"), ClassPath(TargetClass));
	OutData->SetBoolField(TEXT("widget_property_found"), WidgetProperty != nullptr);
	OutData->SetBoolField(TEXT("can_author_component_bound_event"), bCanAuthor);
	OutData->SetBoolField(TEXT("compiled_before_query"), bCompiledBeforeQuery);
	OutData->SetStringField(TEXT("query"), Query);
	OutData->SetNumberField(TEXT("limit"), Limit);
	OutData->SetNumberField(TEXT("total_event_count"), TotalEventCount);
	OutData->SetNumberField(TEXT("matched_event_count"), MatchedEventCount);
	OutData->SetNumberField(TEXT("returned_event_count"), Events.Num());
	OutData->SetBoolField(TEXT("truncated"), Limit > 0 && MatchedEventCount > Events.Num());
	OutData->SetArrayField(TEXT("events"), Events);
	return true;
}
