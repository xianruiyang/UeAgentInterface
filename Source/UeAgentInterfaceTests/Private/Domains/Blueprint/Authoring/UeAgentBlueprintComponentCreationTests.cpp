#include "Misc/AutomationTest.h"

#include "BlueprintEditorLibrary.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringOps.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintMutationModel.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentBlueprintReadableMemberMapTest,
	"UeAgentInterface.Domains.Blueprint.Authoring.ReadableMemberMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentBlueprintReadableMemberMapTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), GetTransientPackage(),
		TEXT("BP_ReadableMemberMap"), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
		TEXT("UeAgentBlueprintReadableMemberMapTest"));
	if (!TestNotNull(TEXT("a member-map Blueprint is created"), Blueprint))
	{
		return false;
	}
	auto MakeMemberExtension = [](const FString& Key, const FString& Operation,
		const FString& NewName = FString())
	{
		const TSharedRef<FJsonObject> Spec = MakeShared<FJsonObject>();
		Spec->SetStringField(TEXT("operation"), Operation);
		if (Operation == TEXT("create"))
		{
			const TSharedRef<FJsonObject> Type = MakeShared<FJsonObject>();
			Type->SetStringField(TEXT("pin_category"), TEXT("bool"));
			Spec->SetObjectField(TEXT("type"), Type);
		}
		if (!NewName.IsEmpty())
		{
			Spec->SetStringField(TEXT("name"), NewName);
		}
		const TSharedRef<FJsonObject> Variables = MakeShared<FJsonObject>();
		if (!NewName.IsEmpty()) {
			// Rename records resolve the captured member identity, not the desired name.
			Spec->SetStringField(TEXT("guid"), Key);
		}
		Variables->SetObjectField(Key, Spec);
		const TSharedRef<FJsonObject> Members = MakeShared<FJsonObject>();
		Members->SetObjectField(TEXT("variables"), Variables);
		const TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
		Extension->SetObjectField(TEXT("members"), Members);
		return Extension;
	};
	auto Prepare = [this, Blueprint](const TSharedRef<FJsonObject>& Extension,
		UeAgentBlueprintAuthoring::FMutationCounters& Counters)
	{
		TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation> Mutation;
		FString Error;
		const bool bPrepared =
			UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
				*Blueprint, Extension, Mutation, Counters, Error);
		if (!bPrepared && !Error.IsEmpty())
		{
			AddError(Error);
		}
		return bPrepared;
	};

	UeAgentBlueprintAuthoring::FMutationCounters Counters;
	TestTrue(TEXT("a readable member name creates a variable"),
		Prepare(MakeMemberExtension(TEXT("TempProbe"), TEXT("create")), Counters));
	TestTrue(TEXT("the created variable is present"),
		FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("TempProbe"))
			!= INDEX_NONE);
	Counters = {};
	TestTrue(TEXT("the readable member name supports update and rename"),
		Prepare(MakeMemberExtension(
			Blueprint->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("TempProbe"))].VarGuid.ToString(), TEXT("update"), TEXT("RenamedProbe")), Counters));
	TestTrue(TEXT("the renamed variable is present"),
		FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("RenamedProbe"))
			!= INDEX_NONE);
	Counters = {};
	TestTrue(TEXT("the readable renamed member supports explicit removal"),
		Prepare(MakeMemberExtension(TEXT("RenamedProbe"), TEXT("remove")), Counters));
	TestEqual(TEXT("the removed variable no longer exists"),
		FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("RenamedProbe")),
		INDEX_NONE);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentBlueprintComponentCreationTest,
	"UeAgentInterface.Domains.Blueprint.Authoring.ComponentCreation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentBlueprintComponentCreationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UBlueprint* Source = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		*FString(TEXT("BP_ComponentCreationSource_") + Suffix),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		TEXT("UeAgentBlueprintComponentCreationTest"));
	if (!TestNotNull(TEXT("a source Actor Blueprint is created"), Source))
	{
		return false;
	}

	USimpleConstructionScript* SourceSCS = Source->SimpleConstructionScript;
	USCS_Node* ExistingComponent = SourceSCS
		? SourceSCS->CreateNode(USceneComponent::StaticClass(), TEXT("ExistingComponent"))
		: nullptr;
	if (!TestNotNull(TEXT("an existing component node is created"), ExistingComponent))
	{
		return false;
	}
	SourceSCS->AddNode(ExistingComponent);
	FKismetEditorUtilities::CompileBlueprint(Source);

	UEdGraph* EventGraph = UBlueprintEditorLibrary::FindEventGraph(Source);
	if (!TestNotNull(TEXT("the source EventGraph exists"), EventGraph))
	{
		return false;
	}
	FGraphNodeCreator<UK2Node_VariableGet> VariableNodeCreator(*EventGraph);
	UK2Node_VariableGet* ExistingComponentGet = VariableNodeCreator.CreateNode();
	ExistingComponentGet->VariableReference.SetSelfMember(
		ExistingComponent->GetVariableName(), ExistingComponent->VariableGuid);
	VariableNodeCreator.Finalize();
	FKismetEditorUtilities::CompileBlueprint(Source);
	if (!TestEqual(
		TEXT("the source Blueprint with a component self-reference compiles"),
		Source->Status,
		BS_UpToDate))
	{
		return false;
	}

	UPackage* ScratchPackage = CreatePackage(
		*FString(TEXT("/Temp/UAI/Tests/BP_ComponentCreationScratch_") + Suffix));
	UBlueprint* Scratch = UeAgentBlueprintAssetLifecycle::DuplicateBlueprint(
		Source, ScratchPackage, NAME_None);
	if (!TestNotNull(TEXT("the source Blueprint duplicates for preflight"), Scratch))
	{
		return false;
	}

	const TFunction<void(const FSubobjectDataHandle&)> NoPostCreate;
	FString Error;
	USCS_Node* AddedRoot = nullptr;
	TestTrue(
		TEXT("class-only component creation uses the Blueprint subobject owner"),
		UeAgentBlueprintAuthoringOps::TryCreateBlueprintComponentNode(
			*Scratch,
			nullptr,
			*UStaticMeshComponent::StaticClass(),
			TEXT("AddedRoot"),
			nullptr,
			NoPostCreate,
			AddedRoot,
			Error));
	if (!Error.IsEmpty()) AddError(Error);
	if (!TestNotNull(TEXT("the root component node is returned"), AddedRoot))
	{
		return false;
	}

	USCS_Node* AddedChild = nullptr;
	TestTrue(
		TEXT("the same component owner creates a nested class-only component"),
		UeAgentBlueprintAuthoringOps::TryCreateBlueprintComponentNode(
			*Scratch,
			AddedRoot,
			*USceneComponent::StaticClass(),
			TEXT("AddedChild"),
			nullptr,
			NoPostCreate,
			AddedChild,
			Error));
	if (!Error.IsEmpty()) AddError(Error);
	TestTrue(
		TEXT("the nested component is attached to the requested parent"),
		AddedChild && AddedRoot->GetChildNodes().Contains(AddedChild));
	TestTrue(
		TEXT("the root component template belongs to the scratch generated class"),
		AddedRoot->ComponentTemplate
			&& AddedRoot->ComponentTemplate->GetOuter() == Scratch->GeneratedClass);
	TestTrue(
		TEXT("the nested component template belongs to the scratch generated class"),
		AddedChild && AddedChild->ComponentTemplate
			&& AddedChild->ComponentTemplate->GetOuter() == Scratch->GeneratedClass);

	FCompilerResultsLog CompilerLog;
	CompilerLog.bLogInfoOnly = false;
	FKismetEditorUtilities::CompileBlueprint(
		Scratch, EBlueprintCompileOptions::None, &CompilerLog);
	TestEqual(
		TEXT("structural component creation preserves existing self-reference nodes"),
		CompilerLog.NumErrors,
		0);
	TestEqual(
		TEXT("the structurally edited scratch Blueprint remains valid"),
		Scratch->Status,
		BS_UpToDate);

	TArray<UK2Node_VariableGet*> VariableNodes;
	FBlueprintEditorUtils::GetAllNodesOfClass(Scratch, VariableNodes);
	UK2Node_VariableGet* ScratchComponentGet = nullptr;
	for (UK2Node_VariableGet* Node : VariableNodes)
	{
		if (Node && Node->GetVarName() == TEXT("ExistingComponent"))
		{
			ScratchComponentGet = Node;
			break;
		}
	}
	TestTrue(
		TEXT("the duplicated component reference still resolves on the scratch class"),
		ScratchComponentGet
			&& ScratchComponentGet->VariableReference.ResolveMember<FProperty>(
				Scratch->GeneratedClass));

	UPackage* BackupPackage = CreatePackage(
		*FString(TEXT("/Temp/UAI/Tests/BP_ComponentCreationBackup_") + Suffix));
	UBlueprint* Backup = UeAgentBlueprintAssetLifecycle::DuplicateBlueprint(
		Scratch, BackupPackage, NAME_None);
	if (!TestNotNull(TEXT("a rollback Blueprint backup is captured"), Backup))
	{
		return false;
	}
	AddedRoot->ComponentTemplate->SetFlags(RF_Transient);
	Error.Reset();
	TestTrue(
		TEXT("rollback restores a structurally edited Blueprint through valid owners"),
		UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::RestoreFromBackup(
			*Backup, *Scratch, Error));
	if (!Error.IsEmpty()) AddError(Error);
	for (USCS_Node* Node : Scratch->SimpleConstructionScript->GetAllNodes())
	{
		TestTrue(
			*FString::Printf(
				TEXT("restored component template %s belongs to the generated class"),
				*Node->GetVariableName().ToString()),
			Node->ComponentTemplate
				&& Node->ComponentTemplate->GetOuter() == Scratch->GeneratedClass);
	}
	TestEqual(
		TEXT("the restored Blueprint remains compile-valid"),
		Scratch->Status,
		BS_UpToDate);

	UBlueprint* ExternalReferenceProbe = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		*FString(TEXT("BP_ComponentExternalReferenceProbe_") + Suffix),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		TEXT("UeAgentBlueprintComponentCreationTest"));
	USCS_Node* CollidingSelfMember = ExternalReferenceProbe
		&& ExternalReferenceProbe->SimpleConstructionScript
			? ExternalReferenceProbe->SimpleConstructionScript->CreateNode(
				USceneComponent::StaticClass(), ExistingComponent->GetVariableName())
			: nullptr;
	if (!TestNotNull(
		TEXT("an external-reference collision probe component is created"),
		CollidingSelfMember))
	{
		return false;
	}
	ExternalReferenceProbe->SimpleConstructionScript->AddNode(CollidingSelfMember);
	UEdGraph* ProbeGraph = UBlueprintEditorLibrary::FindEventGraph(ExternalReferenceProbe);
	if (!TestNotNull(TEXT("the external-reference probe graph exists"), ProbeGraph))
	{
		return false;
	}
	FGraphNodeCreator<UK2Node_VariableGet> ExternalNodeCreator(*ProbeGraph);
	UK2Node_VariableGet* ExternalVariableGet = ExternalNodeCreator.CreateNode();
	ExternalVariableGet->VariableReference.SetExternalMember(
		CollidingSelfMember->GetVariableName(), AActor::StaticClass());
	ExternalNodeCreator.Finalize();
	UeAgentBlueprintAssetLifecycle::RebindBlueprintSelfMemberReferences(
		*ExternalReferenceProbe);
	TestFalse(
		TEXT("same-named external member references are not rebound to self"),
		ExternalVariableGet->VariableReference.IsSelfContext());
	TestEqual(
		TEXT("the external member owner remains unchanged"),
		ExternalVariableGet->VariableReference.GetMemberParentClass(),
		AActor::StaticClass());
	return true;
}
