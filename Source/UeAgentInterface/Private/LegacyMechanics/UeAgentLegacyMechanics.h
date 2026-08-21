// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "LegacyMechanics/UeAgentLegacyMechanicsTypes.h"

class FProperty;

// Private mechanics retained for current domain owners while the historical
// command bodies are split by domain. This class owns no transport, registry,
// public route, authentication, or command dispatch.
class FUeAgentLegacyMechanics final
{
public:
	FUeAgentLegacyMechanics() = default;
	~FUeAgentLegacyMechanics() = default;

	// Command member declarations are generated from domain-owned member headers.
#define UEAGENT_LEGACY_MECHANICS_COMMAND_MEMBER_DECL 1
#include "LegacyMechanics/Generated/UeAgentLegacyMechanicsCommandMembers.h"
#undef UEAGENT_LEGACY_MECHANICS_COMMAND_MEMBER_DECL

private:
	static bool JsonTryGetObject(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& Key,
		TSharedPtr<FJsonObject>& OutObj);
	static bool JsonTryGetString(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& Key,
		FString& OutValue);
	static bool JsonTryGetNumber(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& Key,
		double& OutValue);
	static bool JsonTryGetBool(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& Key,
		bool& OutValue);
	static bool JsonTryGetVector(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& Key,
		FVector& OutValue);
	static bool JsonTryGetRotator(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& Key,
		FRotator& OutValue);

	static TSharedPtr<FJsonObject> VecToJson(const FVector& V);
	static TSharedPtr<FJsonObject> RotToJson(const FRotator& R);
	static void SetPropertyImportResultFields(
		const TSharedPtr<FJsonObject>& Obj,
		FProperty* Property,
		const FString& RequestedValueText,
		const FString& AppliedValueText,
		const FString& ImportStatus,
		const FString& ImportError = FString());
	static TSharedPtr<FJsonObject> MakePropertyImportResultJson(
		const FString& PropertyName,
		FProperty* Property,
		const FString& RequestedValueText,
		const FString& AppliedValueText,
		const FString& ImportStatus,
		const FString& ImportError = FString());

public:
	static class UWorld* GetEditorWorld(FString& OutError);
	static class AActor* FindActorByNameOrLabel(
		UWorld* World,
		const FString& NameOrLabel);

private:
	mutable bool bPythonConsoleCommandExecuted = false;
};
