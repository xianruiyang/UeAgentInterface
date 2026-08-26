#include "CoreMinimal.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "Misc/AutomationTest.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "PCGGraph.h"
#include "UObject/UnrealType.h"

namespace
{
void AppendSignatureField(FString& Signature, const FString& Name,
	const FString& Value)
{
	Signature += FString::Printf(TEXT("%d:%s=%d:%s|"),
		Name.Len(), *Name, Value.Len(), *Value);
}

FString RemovePointerAddresses(const FString& Input)
{
	FString Result;
	Result.Reserve(Input.Len());
	for (int32 Index = 0; Index < Input.Len();)
	{
		if (Index + 2 < Input.Len()
			&& Input[Index] == TEXT('0')
			&& Input[Index + 1] == TEXT('x')
			&& FChar::IsHexDigit(Input[Index + 2]))
		{
			Result += TEXT("<address>");
			Index += 2;
			while (Index < Input.Len() && FChar::IsHexDigit(Input[Index]))
			{
				++Index;
			}
			continue;
		}
		Result.AppendChar(Input[Index]);
		++Index;
	}
	return Result;
}

UScriptStruct* ResolveActionScriptStruct(const FName TypeId)
{
	FString StructName = TypeId.ToString();
	if (StructName.StartsWith(TEXT("F")))
	{
		StructName.RightChopInline(1, EAllowShrinking::No);
	}
	return FindObject<UScriptStruct>(
		nullptr, *(TEXT("/Script/PCGEditor.") + StructName));
}

FString BuildActionSignature(const FEdGraphSchemaAction& Action,
	bool& bOutResolvedScriptStruct)
{
	FString Signature;
	AppendSignatureField(Signature, TEXT("type_id"),
		Action.GetTypeId().ToString());
	AppendSignatureField(Signature, TEXT("category"),
		Action.GetCategory().ToString());
	AppendSignatureField(Signature, TEXT("menu"),
		Action.GetMenuDescription().ToString());
	AppendSignatureField(Signature, TEXT("tooltip"),
		Action.GetTooltipDescription().ToString());
	AppendSignatureField(Signature, TEXT("keywords"),
		Action.GetKeywords().ToString());
	AppendSignatureField(Signature, TEXT("grouping"),
		LexToString(Action.Grouping));
	AppendSignatureField(Signature, TEXT("section_id"),
		LexToString(Action.SectionID));

	UScriptStruct* const ScriptStruct = ResolveActionScriptStruct(
		Action.GetTypeId());
	bOutResolvedScriptStruct = ScriptStruct != nullptr;
	if (!ScriptStruct)
	{
		return Signature;
	}

	TArray<const FProperty*> Properties;
	for (TFieldIterator<FProperty> It(
		ScriptStruct, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		Properties.Add(*It);
	}
	Properties.Sort([](const FProperty& Left, const FProperty& Right)
	{
		return Left.GetName().Compare(Right.GetName(),
			ESearchCase::CaseSensitive) < 0;
	});
	for (const FProperty* Property : Properties)
	{
		FString Exported;
		const void* const Value = Property->ContainerPtrToValuePtr<void>(&Action);
		Property->ExportTextItem_Direct(
			Exported, Value, nullptr, nullptr, PPF_None);
		AppendSignatureField(Signature,
			TEXT("property.") + Property->GetName(),
			RemovePointerAddresses(Exported));
	}
	return Signature;
}

struct FProjectionStats
{
	TArray<FString> Signatures;
	TSet<FString> SignatureSet;
	int32 ResolvedScriptStructs = 0;
	int32 UnresolvedScriptStructs = 0;
	int32 CollisionGroups = 0;
	int32 MaxCollisionGroup = 0;
	int32 NativeElementActions = 0;
	int32 NativeElementUniqueSignatures = 0;
	int32 NativeElementCollisionGroups = 0;
	int32 NativeElementMaxCollisionGroup = 0;
	FString NativeElementSignatureSummary;
};

FProjectionStats ProjectActions(const FGraphContextMenuBuilder& MenuBuilder)
{
	FProjectionStats Stats;
	TMap<FString, int32> Counts;
	TMap<FString, int32> NativeElementCounts;
	const FName NativeElementTypeId(
		TEXT("FPCGEditorGraphSchemaAction_NewNativeElement"));
	for (int32 Index = 0; Index < MenuBuilder.GetNumActions(); ++Index)
	{
		const TSharedPtr<FEdGraphSchemaAction>& Action =
			const_cast<FGraphContextMenuBuilder&>(MenuBuilder)
				.GetSchemaAction(Index);
		if (!Action.IsValid())
		{
			continue;
		}
		bool bResolved = false;
		const FString Signature = BuildActionSignature(*Action, bResolved);
		Stats.Signatures.Add(Signature);
		Stats.SignatureSet.Add(Signature);
		++Counts.FindOrAdd(Signature);
		bResolved ? ++Stats.ResolvedScriptStructs
			: ++Stats.UnresolvedScriptStructs;
		if (Action->GetTypeId() == NativeElementTypeId)
		{
			++Stats.NativeElementActions;
			++NativeElementCounts.FindOrAdd(Signature);
			if (Stats.NativeElementSignatureSummary.IsEmpty())
			{
				Stats.NativeElementSignatureSummary = FMD5::HashAnsiString(
					*Signature);
			}
		}
	}
	for (const TPair<FString, int32>& Pair : Counts)
	{
		if (Pair.Value > 1)
		{
			++Stats.CollisionGroups;
			Stats.MaxCollisionGroup = FMath::Max(
				Stats.MaxCollisionGroup, Pair.Value);
		}
	}
	Stats.NativeElementUniqueSignatures = NativeElementCounts.Num();
	for (const TPair<FString, int32>& Pair : NativeElementCounts)
	{
		if (Pair.Value > 1)
		{
			++Stats.NativeElementCollisionGroups;
			Stats.NativeElementMaxCollisionGroup = FMath::Max(
				Stats.NativeElementMaxCollisionGroup, Pair.Value);
		}
	}
	return Stats;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85PcgReflectedNativeActionProbe,
	"UAI.Root.T85.PCG.ReflectedNativeActionProbe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85PcgReflectedNativeActionProbe::RunTest(
	const FString& Parameters)
{
	(void)Parameters;

	FModuleManager::Get().LoadModule(TEXT("PCGEditor"));
	if (!TestTrue(
		TEXT("PCGEditor loads for the reflected native-action probe"),
		FModuleManager::Get().IsModuleLoaded(TEXT("PCGEditor"))))
	{
		return false;
	}

	UClass* const EditorGraphClass = FindObject<UClass>(
		nullptr, TEXT("/Script/PCGEditor.PCGEditorGraph"));
	UClass* const SchemaClass = FindObject<UClass>(
		nullptr, TEXT("/Script/PCGEditor.PCGEditorGraphSchema"));
	if (!TestNotNull(TEXT("PCGEditorGraph resolves by reflection"),
		EditorGraphClass)
		|| !TestNotNull(TEXT("PCGEditorGraphSchema resolves by reflection"),
			SchemaClass))
	{
		return false;
	}
	if (!TestTrue(TEXT("the reflected editor graph derives from UEdGraph"),
		EditorGraphClass->IsChildOf(UEdGraph::StaticClass()))
		|| !TestTrue(TEXT("the reflected schema derives from UEdGraphSchema"),
			SchemaClass->IsChildOf(UEdGraphSchema::StaticClass())))
	{
		return false;
	}

	UPCGGraph* const PcgGraph = NewObject<UPCGGraph>(GetTransientPackage());
	UEdGraph* const EditorGraph = Cast<UEdGraph>(NewObject<UObject>(
		GetTransientPackage(), EditorGraphClass));
	if (!TestNotNull(TEXT("the transient UPCGGraph is created"), PcgGraph)
		|| !TestNotNull(TEXT("the reflected PCG editor graph is created"),
			EditorGraph))
	{
		return false;
	}

	FObjectPropertyBase* const PcgGraphProperty =
		FindFProperty<FObjectPropertyBase>(EditorGraphClass, TEXT("PCGGraph"));
	if (!TestNotNull(TEXT("PCGEditorGraph exposes reflected PCGGraph"),
		PcgGraphProperty))
	{
		return false;
	}
	if (!TestTrue(TEXT("PCGGraph property accepts UPCGGraph"),
		PcgGraphProperty->PropertyClass
			&& PcgGraphProperty->PropertyClass->IsChildOf(
				UPCGGraph::StaticClass())))
	{
		return false;
	}
	PcgGraphProperty->SetObjectPropertyValue_InContainer(EditorGraph, PcgGraph);
	if (!TestEqual(TEXT("PCGGraph reflection assignment reads back"),
		PcgGraphProperty->GetObjectPropertyValue_InContainer(EditorGraph),
		static_cast<UObject*>(PcgGraph)))
	{
		return false;
	}

	FClassProperty* const SchemaProperty = FindFProperty<FClassProperty>(
		UEdGraph::StaticClass(), TEXT("Schema"));
	if (!TestNotNull(TEXT("UEdGraph exposes reflected Schema"), SchemaProperty))
	{
		return false;
	}
	if (!TestTrue(TEXT("Schema property accepts UEdGraphSchema classes"),
		SchemaProperty->MetaClass
			&& SchemaProperty->MetaClass->IsChildOf(
				UEdGraphSchema::StaticClass())))
	{
		return false;
	}
	SchemaProperty->SetPropertyValue_InContainer(EditorGraph, SchemaClass);
	UObject* const AssignedSchema =
		SchemaProperty->GetPropertyValue_InContainer(EditorGraph).Get();
	if (!TestEqual<UObject*>(TEXT("Schema reflection assignment reads back"),
		AssignedSchema, SchemaClass))
	{
		return false;
	}

	const UEdGraphSchema* const Schema = EditorGraph->GetSchema();
	if (!TestNotNull(TEXT("the public graph resolves its reflected schema"),
		Schema))
	{
		return false;
	}

	FGraphContextMenuBuilder FirstMenu(EditorGraph);
	Schema->GetGraphContextActions(FirstMenu);
	const FProjectionStats First = ProjectActions(FirstMenu);
	FGraphContextMenuBuilder SecondMenu(EditorGraph);
	Schema->GetGraphContextActions(SecondMenu);
	const FProjectionStats Second = ProjectActions(SecondMenu);

	AddInfo(FString::Printf(
		TEXT("pcg_identity_probe round1=%d round2=%d sequence_equal=%s set_equal=%s unique=%d collisions=%d max_collision=%d resolved_structs=%d unresolved_structs=%d"),
		FirstMenu.GetNumActions(), SecondMenu.GetNumActions(),
		First.Signatures == Second.Signatures ? TEXT("true") : TEXT("false"),
		First.SignatureSet.Difference(Second.SignatureSet).IsEmpty()
			&& Second.SignatureSet.Difference(First.SignatureSet).IsEmpty()
			? TEXT("true") : TEXT("false"),
		First.SignatureSet.Num(), First.CollisionGroups,
		First.MaxCollisionGroup, First.ResolvedScriptStructs,
		First.UnresolvedScriptStructs));
	AddInfo(FString::Printf(
		TEXT("pcg_identity_probe native_element_actions=%d native_unique=%d native_collision_groups=%d native_max_collision=%d signature_md5=%s"),
		First.NativeElementActions, First.NativeElementUniqueSignatures,
		First.NativeElementCollisionGroups,
		First.NativeElementMaxCollisionGroup,
		*First.NativeElementSignatureSummary));

	TestTrue(TEXT("the complete PCG context action list is non-empty"),
		FirstMenu.GetNumActions() > 0);
	TestEqual(TEXT("both enumerations return the same action count"),
		SecondMenu.GetNumActions(), FirstMenu.GetNumActions());
	TestTrue(TEXT("both enumerations return the same signature sequence"),
		First.Signatures == Second.Signatures);
	TestTrue(TEXT("both enumerations return the same signature set"),
		First.SignatureSet.Difference(Second.SignatureSet).IsEmpty()
			&& Second.SignatureSet.Difference(First.SignatureSet).IsEmpty());
	TestTrue(TEXT("the projection contains PCG Native Element actions"),
		First.NativeElementActions > 0);
	TestEqual(TEXT("the identity projection does not mutate UPCGGraph nodes"),
		PcgGraph->GetNodes().Num(), 0);

	return !HasAnyErrors();
}
