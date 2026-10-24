// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileStateAI.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileState.h"
#include "UObject/Class.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "Domains/Blackboard/Authoring/UeAgentBlackboardDocument.h"
#include "Domains/BehaviorTree/Authoring/UeAgentBehaviorTreeGraphDocument.h"
#include "Domains/BehaviorTree/Authoring/UeAgentBehaviorTreeAuthoringState.h"
#include "Domains/Eqs/Authoring/UeAgentEqsGraphDocument.h"
#include "Domains/Eqs/Authoring/UeAgentEqsAuthoringState.h"
#include "Domains/SmartObject/Authoring/UeAgentSmartObjectDocument.h"
#include "Domains/SmartObject/Authoring/UeAgentSmartObjectAuthoringState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "SmartObjectDefinition.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"

namespace UeAgentReflectionAuthoring::ProfileStateAI
{
	namespace
	{
		constexpr const TCHAR* BehaviorTreeTypeId = TEXT("behavior_tree");
		constexpr const TCHAR* BlackboardTypeId = TEXT("blackboard");
		constexpr const TCHAR* EqsTypeId = TEXT("eqs");
		constexpr const TCHAR* SmartObjectTypeId = TEXT("smart_object");
		constexpr const TCHAR* UnsupportedError = TEXT("reflection_profile_state_ai_unsupported");

		UE::UAI::Contract::FDiagnostic Unsupported(const FProfile& Profile)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(UnsupportedError,
				TEXT("The AI profile state owner does not support the requested profile."), TEXT("reflection_profile_state"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = TEXT("profile_type");
			Error.ActualValue = MakeShared<FJsonValueString>(Profile.TypeId);
			Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("behavior_tree, blackboard, eqs, or smart_object"));
			return Error;
		}

		bool ReadBehaviorTree(UObject& Asset, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
		{
			UBehaviorTree* Tree = Cast<UBehaviorTree>(&Asset);
			if (!Tree) { OutError = ProfileState::TargetFailure(TEXT("behavior_tree_asset_invalid"), Asset, *UBehaviorTree::StaticClass()); return false; }
			return UeAgentBehaviorTreeAuthoring::ReadGraphDocument(*Tree, OutValue, OutError);
		}

		bool ReadEqs(UObject& Asset, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
		{
			UEnvQuery* Query = Cast<UEnvQuery>(&Asset);
			if (!Query) { OutError = ProfileState::TargetFailure(TEXT("eqs_asset_invalid"), Asset, *UEnvQuery::StaticClass()); return false; }
			return UeAgentEqsAuthoring::ReadGraphDocument(*Query, OutValue, OutError);
		}

		bool ApplyBehaviorTree(UObject& Asset, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError,
			TArray<UeAgentDetailsAuthoringService::FScopedPropertyIntent>* OutIntents)
		{
			UBehaviorTree* Tree = Cast<UBehaviorTree>(&Asset);
			if (!Tree) { OutError = ProfileState::TargetFailure(TEXT("behavior_tree_asset_invalid"), Asset, *UBehaviorTree::StaticClass()); return false; }
			return UeAgentBehaviorTreeAuthoring::ApplyGraphDocument(*Tree, Value, OutError, OutIntents);
		}

		bool ApplyEqs(UObject& Asset, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError,
			TArray<UeAgentDetailsAuthoringService::FScopedPropertyIntent>* OutIntents)
		{
			UEnvQuery* Query = Cast<UEnvQuery>(&Asset);
			if (!Query) { OutError = ProfileState::TargetFailure(TEXT("eqs_asset_invalid"), Asset, *UEnvQuery::StaticClass()); return false; }
			return UeAgentEqsAuthoring::ApplyGraphDocument(*Query, Value, OutError, OutIntents);
		}
	}

	bool Supports(const FProfile& Profile)
	{
		return Profile.TypeId == BehaviorTreeTypeId || Profile.TypeId == BlackboardTypeId
				|| Profile.TypeId == EqsTypeId || Profile.TypeId == SmartObjectTypeId;
	}

	bool Validate(
		const FProfile& Profile,
		const TSharedPtr<FJsonValue>& Value,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (Profile.TypeId == BehaviorTreeTypeId)
		{
			return UeAgentBehaviorTreeAuthoring::ValidateGraphDocument(Value, OutError);
		}
		if (Profile.TypeId == BlackboardTypeId)
		{
			return UeAgentBlackboardAuthoring::ValidateDocument(Value, OutError);
		}
		if (Profile.TypeId == EqsTypeId)
		{
			return UeAgentEqsAuthoring::ValidateGraphDocument(Value, OutError);
		}
		if (Profile.TypeId == SmartObjectTypeId)
			return UeAgentSmartObjectAuthoring::ValidateDefinitionDocument(Value, OutError);
		OutError = Unsupported(Profile);
		return false;
	}

	bool Read(
		const FProfile& Profile,
		UObject& Asset,
		TSharedPtr<FJsonValue>& OutValue,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutValue.Reset();
		if (Profile.TypeId == BehaviorTreeTypeId) return ReadBehaviorTree(Asset, OutValue, OutError);
		if (Profile.TypeId == BlackboardTypeId)
		{
			auto* Blackboard = Cast<UBlackboardData>(&Asset);
			if (!Blackboard) { OutError = ProfileState::TargetFailure(TEXT("blackboard_asset_invalid"), Asset, *UBlackboardData::StaticClass()); return false; }
			return UeAgentBlackboardAuthoring::ReadDocument(*Blackboard, OutValue, OutError);
		}
		if (Profile.TypeId == EqsTypeId) return ReadEqs(Asset, OutValue, OutError);
		if (Profile.TypeId == SmartObjectTypeId)
		{
			auto* Definition = Cast<USmartObjectDefinition>(&Asset);
			if (!Definition) {
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("smart_object_asset_invalid"),
					TEXT("The selected asset is not a SmartObjectDefinition."), TEXT("reflection_profile_state"));
				OutError.SourceKind = TEXT("native_result"); OutError.TargetPath = Asset.GetPathName();
				OutError.PropertyPath = TEXT("asset.class");
				OutError.ActualValuePresent = true;
				OutError.ActualValue = MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(USmartObjectDefinition::StaticClass()->GetPathName());
				return false;
			}
			return UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, OutValue, OutError);
		}
		OutError = Unsupported(Profile);
		return false;
	}

	bool Apply(
		const FProfile& Profile,
		UObject& Asset,
		const TSharedPtr<FJsonValue>& Value,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return Apply(Profile, Asset, Value, OutError, nullptr);
	}

	bool Apply(
		const FProfile& Profile,
		UObject& Asset,
		const TSharedPtr<FJsonValue>& Value,
		UE::UAI::Contract::FDiagnostic& OutError, ProfileState::FIntentVerifier* OutIntent)
	{
		if (OutIntent) *OutIntent = nullptr;
		const bool bBehaviorTree = Profile.TypeId == BehaviorTreeTypeId;
		if (bBehaviorTree || Profile.TypeId == EqsTypeId)
		{
			auto Intents = MakeShared<TArray<UeAgentDetailsAuthoringService::FScopedPropertyIntent>>();
			if (!(bBehaviorTree ? ApplyBehaviorTree(Asset, Value, OutError, &Intents.Get())
				: ApplyEqs(Asset, Value, OutError, &Intents.Get()))) return false;
			if (OutIntent && !Intents->IsEmpty()) *OutIntent = [Intents, bBehaviorTree](UObject& Current, UE::UAI::Contract::FDiagnostic& Error) {
				TArray<UeAgentAuthoringState::FReadRequest> Requests;
				if (bBehaviorTree)
				{
					auto* Tree = Cast<UBehaviorTree>(&Current);
					if (!Tree) { Error = ProfileState::TargetFailure(TEXT("behavior_tree_asset_invalid"), Current, *UBehaviorTree::StaticClass()); return false; }
					if (!UeAgentBehaviorTreeAuthoring::AppendGraphExportRequests(*Tree, Requests, Error)) return false;
				}
				else
				{
					auto* Query = Cast<UEnvQuery>(&Current);
					if (!Query) { Error = ProfileState::TargetFailure(TEXT("eqs_asset_invalid"), Current, *UEnvQuery::StaticClass()); return false; }
					if (!UeAgentEqsAuthoring::AppendGraphExportRequests(*Query, Requests, Error)) return false;
				}
				return UeAgentDetailsAuthoringService::VerifyScopedPropertyIntents(*Intents, Requests, Error);
			};
			return true;
		}
		if (Profile.TypeId == BlackboardTypeId)
		{
			auto* Blackboard = Cast<UBlackboardData>(&Asset);
			if (!Blackboard) { OutError = ProfileState::TargetFailure(TEXT("blackboard_asset_invalid"), Asset, *UBlackboardData::StaticClass()); return false; }
			return UeAgentBlackboardAuthoring::ApplyDocument(*Blackboard, Value, OutError);
		}
		if (Profile.TypeId == SmartObjectTypeId)
		{
			auto* Definition = Cast<USmartObjectDefinition>(&Asset);
			if (!Definition) { OutError = ProfileState::TargetFailure(TEXT("smart_object_asset_invalid"), Asset, *USmartObjectDefinition::StaticClass()); return false; }
			return UeAgentSmartObjectAuthoring::ApplyDefinitionDocumentForAuthoring(*Definition, Value, OutError);
		}
		OutError = Unsupported(Profile);
		return false;
	}
}
