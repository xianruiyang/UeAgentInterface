// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"

#include "Engine/Blueprint.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"

namespace UeAgentBlueprintAssetLifecycle
{
	namespace
	{
		TMap<FName, FGuid> CaptureAuthoredMemberGuids(const UBlueprint* Blueprint)
		{
			TMap<FName, FGuid> Result;
			if (Blueprint)
			{
				for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
				{
					Result.Add(Variable.VarName, Variable.VarGuid);
				}
			}
			return Result;
		}

		void RestoreAuthoredMemberGuids(
			UBlueprint& Blueprint,
			const TMap<FName, FGuid>& AuthoredMemberGuids)
		{
			for (FBPVariableDescription& Variable : Blueprint.NewVariables)
			{
				if (const FGuid* Guid = AuthoredMemberGuids.Find(Variable.VarName)) Variable.VarGuid = *Guid;
			}
			TArray<UK2Node_Variable*> VariableNodes;
			FBlueprintEditorUtils::GetAllNodesOfClass(&Blueprint, VariableNodes);
			for (UK2Node_Variable* Node : VariableNodes)
			{
				if (!Node || !Node->VariableReference.IsSelfContext()) continue;
				if (const FGuid* Guid = AuthoredMemberGuids.Find(Node->GetVarName()))
				{
					Node->VariableReference.SetSelfMember(Node->GetVarName(), *Guid);
				}
			}
			TArray<UK2Node_BaseMCDelegate*> DelegateNodes;
			FBlueprintEditorUtils::GetAllNodesOfClass(&Blueprint, DelegateNodes);
			for (UK2Node_BaseMCDelegate* Node : DelegateNodes)
			{
				if (!Node || !Node->DelegateReference.IsSelfContext()) continue;
				const FName MemberName = Node->GetPropertyName();
				if (const FGuid* Guid = AuthoredMemberGuids.Find(MemberName))
				{
					Node->DelegateReference.SetSelfMember(MemberName, *Guid);
				}
			}
		}
	}

	FString NormalizePackagePath(const FString& InPath)
	{
		FString Path = InPath.TrimStartAndEnd();
		if (Path.Contains(TEXT("."))) Path = FPackageName::ObjectPathToPackageName(Path);
		return Path;
	}

	FString ToObjectPath(const FString& PackagePath)
	{
		const FString Name = FPackageName::GetLongPackageAssetName(PackagePath);
		return Name.IsEmpty() ? PackagePath : FString::Printf(TEXT("%s.%s"), *PackagePath, *Name);
	}

	UBlueprint* LoadBlueprint(const FString& InPath)
	{
		const FString PackagePath = NormalizePackagePath(InPath);
		return PackagePath.IsEmpty()
			? nullptr
			: Cast<UBlueprint>(StaticLoadObject(
				UBlueprint::StaticClass(), nullptr, *ToObjectPath(PackagePath)));
	}

	UBlueprint* DuplicateBlueprint(UBlueprint* Source, UObject* Outer, const FName Name)
	{
		const TMap<FName, FGuid> AuthoredMemberGuids = CaptureAuthoredMemberGuids(Source);
		FBlueprintDuplicationScopeFlags DuplicationScope(
			FBlueprintDuplicationScopeFlags::TheSameNodeGuid
				| FBlueprintDuplicationScopeFlags::TheSameTimelineGuid);
		UBlueprint* Duplicate = Source && Outer
			? Cast<UBlueprint>(StaticDuplicateObject(Source, Outer, Name, RF_AllFlags))
			: nullptr;
		if (Duplicate) RestoreAuthoredMemberGuids(*Duplicate, AuthoredMemberGuids);
		return Duplicate;
	}

	UBlueprint* PublishBlueprint(UBlueprint* Source, UPackage* Package, const FString& Name)
	{
		const TMap<FName, FGuid> AuthoredMemberGuids = CaptureAuthoredMemberGuids(Source);

		FBlueprintDuplicationScopeFlags DuplicationScope(
			FBlueprintDuplicationScopeFlags::TheSameNodeGuid
				| FBlueprintDuplicationScopeFlags::TheSameTimelineGuid);
		UBlueprint* Published = Source && Package && !Name.IsEmpty()
			? Cast<UBlueprint>(StaticDuplicateObject(Source, Package, *Name, RF_AllFlags))
			: nullptr;
		if (!Published) return nullptr;

		RestoreAuthoredMemberGuids(*Published, AuthoredMemberGuids);
		Published->ClearFlags(RF_Transient);
		Published->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
		return Published;
	}

	bool DeleteBlueprint(UBlueprint* Blueprint, FString& OutError)
	{
		OutError.Reset();
		if (!Blueprint)
		{
			OutError = TEXT("blueprint_delete_target_missing");
			return false;
		}
		TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
		if (!ObjectTools::DeleteSingleObject(Blueprint, true))
		{
			OutError = TEXT("blueprint_delete_blocked_by_references");
			return false;
		}
		return true;
	}

	bool CompileBlueprint(UBlueprint* Blueprint, FString& OutError)
	{
		OutError.Reset();
		if (!Blueprint)
		{
			OutError = TEXT("blueprint_compile_target_missing");
			return false;
		}
		FCompilerResultsLog CompilerLog;
		CompilerLog.bLogInfoOnly = false;
		FKismetEditorUtilities::CompileBlueprint(
			Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
		if (CompilerLog.NumErrors > 0 || Blueprint->Status == BS_Error)
		{
			OutError = FString::Printf(TEXT("blueprint_compile_failed:%d"), CompilerLog.NumErrors);
			return false;
		}
		return true;
	}
}
