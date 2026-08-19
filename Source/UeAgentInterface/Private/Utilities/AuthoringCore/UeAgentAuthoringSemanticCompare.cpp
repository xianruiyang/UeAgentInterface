// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeRWLock.h"
#include "UObject/UnrealType.h"

namespace UeAgentAuthoringSemanticCompare
{
	namespace
	{
		FString NormalizeId(FString Value)
		{
			Value.TrimStartAndEndInline();
			Value.ToLowerInline();
			return Value;
		}

		void NormalizeList(TArray<FString>& Values)
		{
			for (FString& Value : Values) Value = NormalizeId(MoveTemp(Value));
			Values.RemoveAll([](const FString& Value) { return Value.IsEmpty(); });
			Values.Sort();
			for (int32 Index = Values.Num() - 1; Index > 0; --Index)
			{
				if (Values[Index] == Values[Index - 1]) Values.RemoveAt(Index);
			}
		}

		FComparatorDescriptor Normalize(FComparatorDescriptor Descriptor)
		{
			Descriptor.ComparatorId = NormalizeId(MoveTemp(Descriptor.ComparatorId));
			NormalizeList(Descriptor.ValueKinds);
			NormalizeList(Descriptor.CapabilityTags);
			return Descriptor;
		}

		bool ValidateComparator(
			const FComparatorDescriptor& Descriptor,
			const FComparator& Comparator,
			FString& OutError)
		{
			if (Descriptor.ComparatorId.IsEmpty()) OutError = TEXT("semantic_comparator_id_missing");
			else if (Descriptor.ValueKinds.IsEmpty()) OutError = TEXT("semantic_comparator_value_kinds_missing");
			else if (!Comparator) OutError = TEXT("semantic_comparator_callback_missing");
			else
			{
				OutError.Reset();
				return true;
			}
			return false;
		}

		bool NearlyEqual(double Left, double Right)
		{
			const double Scale = FMath::Max(1.0, FMath::Max(FMath::Abs(Left), FMath::Abs(Right)));
			return FMath::Abs(Left - Right) <= static_cast<double>(KINDA_SMALL_NUMBER) * Scale;
		}

		bool ReadNumberArray(
			const TSharedPtr<FJsonValue>& Value,
			const FString& FieldName,
			int32 ExpectedCount,
			TArray<double>& OutNumbers)
		{
			OutNumbers.Reset();
			if (!Value.IsValid() || Value->Type != EJson::Object) return false;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Value->AsObject()->TryGetArrayField(FieldName, Values) || !Values || Values->Num() != ExpectedCount) return false;
			for (const TSharedPtr<FJsonValue>& Item : *Values)
			{
				if (!Item.IsValid() || Item->Type != EJson::Number) return false;
				OutNumbers.Add(Item->AsNumber());
			}
			return true;
		}

		bool RotatorsEquivalent(const TArray<double>& Left, const TArray<double>& Right)
		{
			if (Left.Num() != 3 || Right.Num() != 3) return false;
			const FQuat LeftQuat = FRotator(Left[0], Left[1], Left[2]).Quaternion();
			const FQuat RightQuat = FRotator(Right[0], Right[1], Right[2]).Quaternion();
			return FMath::Abs(LeftQuat | RightQuat) >= 1.0 - KINDA_SMALL_NUMBER;
		}

		bool QuatsEquivalent(const TArray<double>& Left, const TArray<double>& Right)
		{
			if (Left.Num() != 4 || Right.Num() != 4) return false;
			FQuat LeftQuat(Left[0], Left[1], Left[2], Left[3]);
			FQuat RightQuat(Right[0], Right[1], Right[2], Right[3]);
			if (LeftQuat.SizeSquared() <= SMALL_NUMBER || RightQuat.SizeSquared() <= SMALL_NUMBER) return false;
			LeftQuat.Normalize();
			RightQuat.Normalize();
			return FMath::Abs(LeftQuat | RightQuat) >= 1.0 - KINDA_SMALL_NUMBER;
		}

		bool NumberArraysEquivalent(const TArray<double>& Left, const TArray<double>& Right)
		{
			if (Left.Num() != Right.Num()) return false;
			for (int32 Index = 0; Index < Left.Num(); ++Index)
			{
				if (!NearlyEqual(Left[Index], Right[Index])) return false;
			}
			return true;
		}

		struct FJsonCompareNode
		{
			TSharedPtr<FJsonValue> Left;
			TSharedPtr<FJsonValue> Right;
			FString Path;
			FString FieldName;
		};

		bool CompareJsonValuesIterative(
			const TSharedPtr<FJsonValue>& Left,
			const TSharedPtr<FJsonValue>& Right,
			const FString& RootPath,
			const FString& RootFieldName,
			FString* OutDifferencePath)
		{
			auto SetDifference = [OutDifferencePath](const FString& Path)
			{
				if (OutDifferencePath) *OutDifferencePath = Path.IsEmpty() ? TEXT("/") : Path;
				return false;
			};
			TArray<FJsonCompareNode> Pending = {{Left, Right, RootPath, RootFieldName}};
			while (!Pending.IsEmpty())
			{
				FJsonCompareNode Node = Pending.Pop(EAllowShrinking::No);
				if (!Node.Left.IsValid() || !Node.Right.IsValid() || Node.Left->Type != Node.Right->Type)
				{
					return SetDifference(Node.Path);
				}
				if (Node.Left->Type == EJson::Object
					&& Node.FieldName.Equals(TEXT("rotation"), ESearchCase::IgnoreCase))
				{
					TArray<double> LeftRotation;
					TArray<double> RightRotation;
					if (ReadNumberArray(Node.Left, TEXT("components"), 3, LeftRotation)
						&& ReadNumberArray(Node.Right, TEXT("components"), 3, RightRotation))
					{
						if (RotatorsEquivalent(LeftRotation, RightRotation)) continue;
						if (!OutDifferencePath) return false;
					}
				}
				switch (Node.Left->Type)
				{
				case EJson::String:
					if (Node.Left->AsString() != Node.Right->AsString()) return SetDifference(Node.Path);
					break;
				case EJson::Number:
					if (!NearlyEqual(Node.Left->AsNumber(), Node.Right->AsNumber())) return SetDifference(Node.Path);
					break;
				case EJson::Boolean:
					if (Node.Left->AsBool() != Node.Right->AsBool()) return SetDifference(Node.Path);
					break;
				case EJson::Array:
				{
					const TArray<TSharedPtr<FJsonValue>>& LeftItems = Node.Left->AsArray();
					const TArray<TSharedPtr<FJsonValue>>& RightItems = Node.Right->AsArray();
					if (LeftItems.Num() != RightItems.Num()) return SetDifference(Node.Path + TEXT("/length"));
					for (int32 Index = LeftItems.Num() - 1; Index >= 0; --Index)
					{
						Pending.Add({
							LeftItems[Index], RightItems[Index],
							Node.Path + TEXT("/") + LexToString(Index), Node.FieldName});
					}
					break;
				}
				case EJson::Object:
				{
					const TSharedPtr<FJsonObject> LeftObject = Node.Left->AsObject();
					const TSharedPtr<FJsonObject> RightObject = Node.Right->AsObject();
					if (!LeftObject.IsValid() || !RightObject.IsValid()) return SetDifference(Node.Path);
					TArray<FString> Keys;
					LeftObject->Values.GenerateKeyArray(Keys);
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : RightObject->Values)
					{
						Keys.AddUnique(Pair.Key);
					}
					Keys.Sort();
					for (int32 Index = Keys.Num() - 1; Index >= 0; --Index)
					{
						const FString& Key = Keys[Index];
						Pending.Add({
							LeftObject->TryGetField(Key), RightObject->TryGetField(Key),
							Node.Path + TEXT("/") + Key, Key});
					}
					break;
				}
				case EJson::Null:
				case EJson::None:
				default:
					break;
				}
			}
			if (OutDifferencePath) OutDifferencePath->Reset();
			return true;
		}

		bool JsonEquivalent(
			const TSharedPtr<FJsonValue>& Left,
			const TSharedPtr<FJsonValue>& Right)
		{
			return CompareJsonValuesIterative(Left, Right, FString(), FString(), nullptr);
		}

		bool FindJsonDifferencePath(
			const TSharedPtr<FJsonValue>& Left,
			const TSharedPtr<FJsonValue>& Right,
			const FString& Path,
			FString& OutPath)
		{
			return !CompareJsonValuesIterative(Left, Right, Path, FString(), &OutPath);
		}

		bool CanonicalValuesEquivalent(
			const UeAgentAuthoringState::FStateEntry& Left,
			const UeAgentAuthoringState::FStateEntry& Right,
			bool& bOutEquivalent,
			FString& OutError)
		{
			if (!Left.Value.IsValid() || !Right.Value.IsValid())
			{
				OutError = TEXT("semantic_compare_value_missing");
				return false;
			}
			if (Left.Type != Right.Type)
			{
				bOutEquivalent = false;
				OutError.Reset();
				return true;
			}
			if (Left.NativeValue.IsValid() && Right.NativeValue.IsValid()
				&& Left.NativeValue->GetProperty() == Right.NativeValue->GetProperty())
			{
				const bool bNativeEquivalent = UeAgentPropertyValueCodec::AreSemanticallyEquivalent(
					Left.NativeValue->GetProperty(),
					Left.NativeValue->GetValuePtr(),
					Right.NativeValue->GetValuePtr(),
					nullptr,
					UeAgentPropertyValueCodec::FOptions());
				if (bNativeEquivalent)
				{
					bOutEquivalent = true;
					OutError.Reset();
					return true;
				}
			}

			const FString Kind = NormalizeId(Left.Type.Kind);
			TArray<double> LeftNumbers;
			TArray<double> RightNumbers;
			if ((Kind == TEXT("rotator") || Kind == TEXT("rotator3f"))
				&& ReadNumberArray(Left.Value, TEXT("components"), 3, LeftNumbers)
				&& ReadNumberArray(Right.Value, TEXT("components"), 3, RightNumbers))
			{
				bOutEquivalent = RotatorsEquivalent(LeftNumbers, RightNumbers);
				OutError.Reset();
				return true;
			}
			if ((Kind == TEXT("quat") || Kind == TEXT("quat4f"))
				&& ReadNumberArray(Left.Value, TEXT("components"), 4, LeftNumbers)
				&& ReadNumberArray(Right.Value, TEXT("components"), 4, RightNumbers))
			{
				bOutEquivalent = QuatsEquivalent(LeftNumbers, RightNumbers);
				OutError.Reset();
				return true;
			}
			if (Kind == TEXT("transform"))
			{
				TArray<double> LeftTranslation;
				TArray<double> RightTranslation;
				TArray<double> LeftRotation;
				TArray<double> RightRotation;
				TArray<double> LeftScale;
				TArray<double> RightScale;
				if (ReadNumberArray(Left.Value, TEXT("translation"), 3, LeftTranslation)
					&& ReadNumberArray(Right.Value, TEXT("translation"), 3, RightTranslation)
					&& ReadNumberArray(Left.Value, TEXT("rotation"), 3, LeftRotation)
					&& ReadNumberArray(Right.Value, TEXT("rotation"), 3, RightRotation)
					&& ReadNumberArray(Left.Value, TEXT("scale"), 3, LeftScale)
					&& ReadNumberArray(Right.Value, TEXT("scale"), 3, RightScale))
				{
					bOutEquivalent = NumberArraysEquivalent(LeftTranslation, RightTranslation)
						&& RotatorsEquivalent(LeftRotation, RightRotation)
						&& NumberArraysEquivalent(LeftScale, RightScale);
					OutError.Reset();
					return true;
				}
			}

			bOutEquivalent = JsonEquivalent(Left.Value, Right.Value);
			OutError.Reset();
			if (!bOutEquivalent)
			{
				FString DifferencePath;
				FindJsonDifferencePath(Left.Value, Right.Value, FString(), DifferencePath);
				OutError = TEXT("canonical_value_mismatch:") + DifferencePath;
			}
			return true;
		}

		FString MakeEntryKey(const UeAgentAuthoringState::FStateEntry& Entry)
		{
			return FString::Printf(
				TEXT("%d:%s%s"), Entry.SemanticId.Len(), *Entry.SemanticId, *Entry.TargetIdentity.CanonicalKey);
		}

		FEntryComparison MakeMissingComparison(
			const UeAgentAuthoringState::FStateEntry& Entry,
			const FString& ErrorCode)
		{
			FEntryComparison Result;
			Result.Status = TEXT("different");
			Result.ErrorCode = ErrorCode;
			Result.SemanticId = Entry.SemanticId;
			Result.TargetIdentity = Entry.TargetIdentity;
			return Result;
		}

		void AddBindingIssue(
			FBindingReport& Report,
			const FString& SemanticId,
			const FString& ErrorCode,
			const FString& ErrorDetail = FString())
		{
			FBindingIssue& Issue = Report.Issues.AddDefaulted_GetRef();
			Issue.SemanticId = SemanticId;
			Issue.ErrorCode = ErrorCode;
			Issue.ErrorDetail = ErrorDetail;
		}
	}

	bool FComparatorDescriptor::SupportsValueKind(const FString& ValueKind) const
	{
		const FString CanonicalValueKind = NormalizeId(ValueKind);
		return ValueKinds.Contains(TEXT("*")) || ValueKinds.Contains(CanonicalValueKind);
	}

	bool FComparatorDescriptor::operator==(const FComparatorDescriptor& Other) const
	{
		return ComparatorId == Other.ComparatorId
			&& ValueKinds == Other.ValueKinds
			&& CapabilityTags == Other.CapabilityTags;
	}

	FComparatorRegistry& FComparatorRegistry::Get()
	{
		static FComparatorRegistry Registry;
		static const bool bBuiltInsRegistered = []()
		{
			FString Error;
			return RegisterBuiltInComparators(Registry, Error);
		}();
		ensureAlwaysMsgf(bBuiltInsRegistered, TEXT("Semantic comparator built-in registration failed."));
		return Registry;
	}

	bool FComparatorRegistry::RegisterComparator(
		const FComparatorDescriptor& Descriptor,
		FComparator Comparator,
		ERegistrationResult& OutResult,
		FString& OutError)
	{
		const FComparatorDescriptor Canonical = Normalize(Descriptor);
		if (!ValidateComparator(Canonical, Comparator, OutError)) return false;
		FWriteScopeLock Lock(RegistryLock);
		if (const FEntry* Existing = Comparators.Find(Canonical.ComparatorId))
		{
			if (Existing->Descriptor != Canonical)
			{
				OutError = FString::Printf(TEXT("semantic_comparator_conflict_%s"), *Canonical.ComparatorId);
				return false;
			}
			OutResult = ERegistrationResult::AlreadyRegistered;
			OutError.Reset();
			return true;
		}
		FEntry& Entry = Comparators.Add(Canonical.ComparatorId);
		Entry.Descriptor = Canonical;
		Entry.Comparator = MoveTemp(Comparator);
		OutResult = ERegistrationResult::Registered;
		OutError.Reset();
		return true;
	}

	bool FComparatorRegistry::FindComparator(const FString& ComparatorId, FComparatorDescriptor& OutDescriptor) const
	{
		FReadScopeLock Lock(RegistryLock);
		if (const FEntry* Entry = Comparators.Find(NormalizeId(ComparatorId)))
		{
			OutDescriptor = Entry->Descriptor;
			return true;
		}
		return false;
	}

	bool FComparatorRegistry::CopyComparator(const FString& ComparatorId, FComparator& OutComparator) const
	{
		FReadScopeLock Lock(RegistryLock);
		if (const FEntry* Entry = Comparators.Find(NormalizeId(ComparatorId)))
		{
			OutComparator = Entry->Comparator;
			return true;
		}
		return false;
	}

	TArray<FComparatorDescriptor> FComparatorRegistry::ListComparators() const
	{
		FReadScopeLock Lock(RegistryLock);
		TArray<FComparatorDescriptor> Result;
		for (const TPair<FString, FEntry>& Pair : Comparators) Result.Add(Pair.Value.Descriptor);
		Result.Sort([](const FComparatorDescriptor& Left, const FComparatorDescriptor& Right)
		{
			return Left.ComparatorId < Right.ComparatorId;
		});
		return Result;
	}

	int32 FComparatorRegistry::GetComparatorCount() const
	{
		FReadScopeLock Lock(RegistryLock);
		return Comparators.Num();
	}

	TSharedRef<FJsonObject> FEntryComparison::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("status"), Status);
		Object->SetStringField(TEXT("error_code"), ErrorCode);
		Object->SetBoolField(TEXT("equivalent"), bEquivalent);
		if (!ErrorDetail.IsEmpty()) Object->SetStringField(TEXT("error_detail"), ErrorDetail);
		if (!SemanticId.IsEmpty()) Object->SetStringField(TEXT("semantic_id"), SemanticId);
		if (!ComparatorId.IsEmpty()) Object->SetStringField(TEXT("comparator_id"), ComparatorId);
		if (TargetIdentity.IsValid()) Object->SetObjectField(TEXT("target_identity"), TargetIdentity.ToJsonObject());
		if (!bEquivalent && !ErrorCode.IsEmpty())
		{
			UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
			Diagnostic.Code = ErrorCode;
			Diagnostic.Stage = TEXT("semantic_compare");
			Diagnostic.Severity = UeAgentAuthoringDiagnostics::ESeverity::Error;
			Diagnostic.Message = ErrorDetail;
			Diagnostic.TargetPath = TargetIdentity.CanonicalKey;
			Diagnostic.PropertyPath = TargetIdentity.GetSelector(TEXT("property_path"));
			Diagnostic.SemanticId = SemanticId;
			Object->SetObjectField(TEXT("diagnostic"), Diagnostic.ToJson());
		}
		return Object;
	}

	TSharedRef<FJsonObject> FComparisonReport::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("status"), Status);
		Object->SetStringField(TEXT("error_code"), ErrorCode);
		Object->SetBoolField(TEXT("equivalent"), bEquivalent);
		if (!ErrorDetail.IsEmpty()) Object->SetStringField(TEXT("error_detail"), ErrorDetail);
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FEntryComparison& Entry : Entries) Values.Add(MakeShared<FJsonValueObject>(Entry.ToJsonObject()));
		Object->SetArrayField(TEXT("entries"), Values);
		return Object;
	}

	TSharedRef<FJsonObject> FBindingIssue::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("semantic_id"), SemanticId);
		Object->SetStringField(TEXT("error_code"), ErrorCode);
		if (!ErrorDetail.IsEmpty()) Object->SetStringField(TEXT("error_detail"), ErrorDetail);
		return Object;
	}

	TSharedRef<FJsonObject> FBindingReport::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("status"), Status);
		Object->SetStringField(TEXT("error_code"), ErrorCode);
		Object->SetBoolField(TEXT("valid"), bValid);
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FBindingIssue& Issue : Issues) Values.Add(MakeShared<FJsonValueObject>(Issue.ToJsonObject()));
		Object->SetArrayField(TEXT("issues"), Values);
		return Object;
	}

	bool RegisterBuiltInComparators(FComparatorRegistry& Registry, FString& OutError)
	{
		FComparatorDescriptor Descriptor;
		Descriptor.ComparatorId = TEXT("uai.compare.canonical_value");
		Descriptor.ValueKinds = { TEXT("*") };
		Descriptor.CapabilityTags = { TEXT("canonical_value"), TEXT("native_property"), TEXT("semantic_tolerance") };
		ERegistrationResult Result = ERegistrationResult::Registered;
		return Registry.RegisterComparator(Descriptor, CanonicalValuesEquivalent, Result, OutError);
	}

	bool BuildExpectedPropertyEntry(
		const FString& SemanticId,
		const UeAgentAuthoringIdentity::FCanonicalIdentity& TargetIdentity,
		FProperty* Property,
		const TSharedPtr<FJsonValue>& CanonicalValue,
		UObject* Owner,
		UeAgentAuthoringState::FStateEntry& OutEntry,
		FString& OutError)
	{
		OutEntry = UeAgentAuthoringState::FStateEntry();
		if (NormalizeId(SemanticId).IsEmpty() || !TargetIdentity.IsValid() || !Property || !CanonicalValue.IsValid())
		{
			OutError = TEXT("semantic_expected_property_invalid");
			return false;
		}
		TSharedPtr<UeAgentAuthoringPropertyValues::FPropertyValueStorage> Storage =
			UeAgentAuthoringPropertyValues::FPropertyValueStorage::CreateInitialized(Property, OutError);
		if (!Storage.IsValid()) return false;
		if (!UeAgentPropertyValueCodec::Decode(
			Property,
			CanonicalValue,
			Storage->GetMutableValuePtr(),
			Owner,
			UeAgentPropertyValueCodec::FOptions(),
			OutError))
		{
			return false;
		}
		const TSharedPtr<FJsonValue> Reencoded = UeAgentPropertyValueCodec::Encode(
			Property, Storage->GetValuePtr(), Owner, UeAgentPropertyValueCodec::FOptions());
		FString ReliabilityReason;
		if (!UeAgentPropertyValueCodec::IsReliableCanonicalValue(Property, Reencoded, &ReliabilityReason))
		{
			OutError = ReliabilityReason.IsEmpty() ? FString(TEXT("semantic_expected_property_unreliable")) : ReliabilityReason;
			return false;
		}
		OutEntry.SemanticId = NormalizeId(SemanticId);
		OutEntry.TargetIdentity = TargetIdentity;
		OutEntry.Type = UeAgentAuthoringTypeDescriptors::Describe(Property);
		OutEntry.Value = UeAgentAuthoringState::CloneCanonicalValue(Reencoded);
		OutEntry.NativeValue = Storage;
		OutEntry.bAvailable = true;
		OutEntry.bEditable = false;
		OutEntry.AccessorId = TEXT("expected.canonical_property");
		Property->ExportTextItem_Direct(
			OutEntry.ValueText, Storage->GetValuePtr(), nullptr, Owner, PPF_None);
		OutEntry.Provenance = TEXT("expected_canonical_value");
		OutError.Reset();
		return true;
	}

	bool ArePropertyValuesEquivalent(
		FProperty* Property,
		const void* LeftValuePtr,
		const void* RightValuePtr,
		UObject* Owner,
		bool& bOutEquivalent,
		FString& OutError)
	{
		bOutEquivalent = false;
		if (!Property || !LeftValuePtr || !RightValuePtr)
		{
			OutError = TEXT("semantic_property_compare_input_invalid");
			return false;
		}
		UeAgentPropertyValueCodec::FOptions CompareOptions;
		CompareOptions.MaxDepth = -1;
		CompareOptions.MaxContainerItems = 0;
		CompareOptions.MaxStringChars = 0;
		bOutEquivalent = UeAgentPropertyValueCodec::AreSemanticallyEquivalent(
			Property,
			LeftValuePtr,
			RightValuePtr,
			Owner,
			CompareOptions);
		OutError.Reset();
		return true;
	}

	bool CompareEntries(
		const UeAgentAuthoringState::FStateEntry& Left,
		const UeAgentAuthoringState::FStateEntry& Right,
		const FCompareOptions& Options,
		FEntryComparison& OutResult,
		const UeAgentAuthoringSemanticRegistry::FRegistry* SemanticRegistry,
		const FComparatorRegistry* ComparatorRegistry)
	{
		OutResult = FEntryComparison();
		OutResult.SemanticId = Left.SemanticId;
		OutResult.TargetIdentity = Left.TargetIdentity;
		if (!Left.IsValid() || !Right.IsValid())
		{
			OutResult.Status = TEXT("failed");
			OutResult.ErrorCode = TEXT("semantic_compare_entry_invalid");
			return false;
		}
		if (Left.SemanticId != Right.SemanticId)
		{
			OutResult.Status = TEXT("different");
			OutResult.ErrorCode = TEXT("semantic_id_mismatch");
			return true;
		}
		if (Left.TargetIdentity.CanonicalKey != Right.TargetIdentity.CanonicalKey)
		{
			OutResult.Status = TEXT("different");
			OutResult.ErrorCode = TEXT("semantic_identity_mismatch");
			return true;
		}

		const UeAgentAuthoringSemanticRegistry::FRegistry& Semantics = SemanticRegistry
			? *SemanticRegistry
			: UeAgentAuthoringSemanticRegistry::FRegistry::Get();
		UeAgentAuthoringSemanticRegistry::FSemanticDescriptor Semantic;
		if (!Semantics.FindSemantic(Left.SemanticId, Semantic))
		{
			OutResult.Status = TEXT("failed");
			OutResult.ErrorCode = TEXT("semantic_compare_semantic_unknown");
			return false;
		}
		OutResult.ComparatorId = Semantic.Comparator;
		if (Options.bCompareAvailability
			&& (Left.bAvailable != Right.bAvailable || Left.bEditable != Right.bEditable))
		{
			OutResult.Status = TEXT("different");
			OutResult.ErrorCode = TEXT("semantic_availability_mismatch");
			return true;
		}
		if (!Options.bCompareValue || (!Left.bAvailable && !Right.bAvailable))
		{
			OutResult.bEquivalent = true;
			OutResult.Status = TEXT("equivalent");
			OutResult.ErrorCode = TEXT("semantic_equivalent");
			return true;
		}

		const FComparatorRegistry& Comparators = ComparatorRegistry ? *ComparatorRegistry : FComparatorRegistry::Get();
		FComparatorDescriptor Descriptor;
		FComparator Comparator;
		if (!Comparators.FindComparator(Semantic.Comparator, Descriptor)
			|| !Comparators.CopyComparator(Semantic.Comparator, Comparator))
		{
			OutResult.Status = TEXT("failed");
			OutResult.ErrorCode = TEXT("semantic_comparator_unknown");
			return false;
		}
		if (!Descriptor.SupportsValueKind(Semantic.ValueType))
		{
			OutResult.Status = TEXT("failed");
			OutResult.ErrorCode = TEXT("semantic_comparator_value_kind_unsupported");
			return false;
		}
		FString ComparatorError;
		if (!Comparator(Left, Right, OutResult.bEquivalent, ComparatorError))
		{
			OutResult.Status = TEXT("failed");
			OutResult.ErrorCode = TEXT("semantic_comparator_failed");
			OutResult.ErrorDetail = ComparatorError;
			return false;
		}
		OutResult.Status = OutResult.bEquivalent ? TEXT("equivalent") : TEXT("different");
		OutResult.ErrorCode = OutResult.bEquivalent ? TEXT("semantic_equivalent") : TEXT("semantic_value_mismatch");
		OutResult.ErrorDetail = OutResult.bEquivalent ? FString() : ComparatorError;
		return true;
	}

	bool CompareSnapshots(
		const UeAgentAuthoringState::FStateSnapshot& Left,
		const UeAgentAuthoringState::FStateSnapshot& Right,
		const FCompareOptions& Options,
		FComparisonReport& OutReport,
		const UeAgentAuthoringSemanticRegistry::FRegistry* SemanticRegistry,
		const FComparatorRegistry* ComparatorRegistry)
	{
		OutReport = FComparisonReport();
		const TArray<UeAgentAuthoringState::FStateEntry> LeftEntries = Left.CopyEntries();
		const TArray<UeAgentAuthoringState::FStateEntry> RightEntries = Right.CopyEntries();
		int32 LeftIndex = 0;
		int32 RightIndex = 0;
		while (LeftIndex < LeftEntries.Num() || RightIndex < RightEntries.Num())
		{
			if (LeftIndex >= LeftEntries.Num())
			{
				OutReport.Entries.Add(MakeMissingComparison(RightEntries[RightIndex++], TEXT("semantic_missing_left")));
				continue;
			}
			if (RightIndex >= RightEntries.Num())
			{
				OutReport.Entries.Add(MakeMissingComparison(LeftEntries[LeftIndex++], TEXT("semantic_missing_right")));
				continue;
			}
			const FString LeftKey = MakeEntryKey(LeftEntries[LeftIndex]);
			const FString RightKey = MakeEntryKey(RightEntries[RightIndex]);
			if (LeftKey < RightKey)
			{
				OutReport.Entries.Add(MakeMissingComparison(LeftEntries[LeftIndex++], TEXT("semantic_missing_right")));
				continue;
			}
			if (RightKey < LeftKey)
			{
				OutReport.Entries.Add(MakeMissingComparison(RightEntries[RightIndex++], TEXT("semantic_missing_left")));
				continue;
			}
			FEntryComparison Comparison;
			if (!CompareEntries(
				LeftEntries[LeftIndex++],
				RightEntries[RightIndex++],
				Options,
				Comparison,
				SemanticRegistry,
				ComparatorRegistry))
			{
				OutReport.Entries.Add(Comparison);
				OutReport.Status = TEXT("failed");
				OutReport.ErrorCode = Comparison.ErrorCode;
				OutReport.ErrorDetail = Comparison.ErrorDetail;
				return false;
			}
			OutReport.Entries.Add(Comparison);
		}
		OutReport.bEquivalent = !OutReport.Entries.ContainsByPredicate(
			[](const FEntryComparison& Entry) { return !Entry.bEquivalent; });
		OutReport.Status = OutReport.bEquivalent ? TEXT("equivalent") : TEXT("different");
		OutReport.ErrorCode = OutReport.bEquivalent ? TEXT("semantic_snapshots_equivalent") : TEXT("semantic_snapshots_different");
		return true;
	}

	bool ValidateSharedCoreBindings(
		FBindingReport& OutReport,
		const UeAgentAuthoringSemanticRegistry::FRegistry* SemanticRegistry,
		const UeAgentAuthoringIdentity::FRegistry* IdentityRegistry,
		const UeAgentAuthoringState::FStateAccessorRegistry* AccessorRegistry,
		const FComparatorRegistry* ComparatorRegistry)
	{
		OutReport = FBindingReport();
		const UeAgentAuthoringSemanticRegistry::FRegistry& Semantics = SemanticRegistry
			? *SemanticRegistry
			: UeAgentAuthoringSemanticRegistry::FRegistry::Get();
		const UeAgentAuthoringIdentity::FRegistry& Identities = IdentityRegistry
			? *IdentityRegistry
			: UeAgentAuthoringIdentity::FRegistry::Get();
		const UeAgentAuthoringState::FStateAccessorRegistry& Accessors = AccessorRegistry
			? *AccessorRegistry
			: UeAgentAuthoringState::FStateAccessorRegistry::Get();
		const FComparatorRegistry& Comparators = ComparatorRegistry
			? *ComparatorRegistry
			: FComparatorRegistry::Get();
		for (const UeAgentAuthoringSemanticRegistry::FSemanticDescriptor& Semantic : Semantics.ListSemantics())
		{
			UeAgentAuthoringSemanticRegistry::FDomainDescriptor Domain;
			if (!Semantics.FindDomain(Semantic.DomainId, Domain))
			{
				AddBindingIssue(OutReport, Semantic.SemanticId, TEXT("binding_domain_unknown"), Semantic.DomainId);
			}
			UeAgentAuthoringIdentity::FModelDescriptor IdentityModel;
			if (!Identities.FindModel(Semantic.IdentityModel, IdentityModel))
			{
				AddBindingIssue(OutReport, Semantic.SemanticId, TEXT("binding_identity_model_unknown"), Semantic.IdentityModel);
			}
			else if (!IdentityModel.TargetKind.IsEmpty() && IdentityModel.TargetKind != Semantic.TargetKind)
			{
				AddBindingIssue(OutReport, Semantic.SemanticId, TEXT("binding_identity_target_kind_mismatch"), IdentityModel.TargetKind);
			}
			UeAgentAuthoringState::FStateAccessorDescriptor Accessor;
			if (!Accessors.FindAccessor(Semantic.StateAccessor, Accessor))
			{
				AddBindingIssue(OutReport, Semantic.SemanticId, TEXT("binding_state_accessor_unknown"), Semantic.StateAccessor);
			}
			else if (!Accessor.SupportsTargetKind(Semantic.TargetKind))
			{
				AddBindingIssue(OutReport, Semantic.SemanticId, TEXT("binding_state_accessor_target_kind_unsupported"), Semantic.TargetKind);
			}
			FComparatorDescriptor Comparator;
			if (!Comparators.FindComparator(Semantic.Comparator, Comparator))
			{
				AddBindingIssue(OutReport, Semantic.SemanticId, TEXT("binding_comparator_unknown"), Semantic.Comparator);
			}
			else if (!Comparator.SupportsValueKind(Semantic.ValueType))
			{
				AddBindingIssue(OutReport, Semantic.SemanticId, TEXT("binding_comparator_value_kind_unsupported"), Semantic.ValueType);
			}
		}
		OutReport.Issues.Sort([](const FBindingIssue& Left, const FBindingIssue& Right)
		{
			if (Left.SemanticId != Right.SemanticId) return Left.SemanticId < Right.SemanticId;
			if (Left.ErrorCode != Right.ErrorCode) return Left.ErrorCode < Right.ErrorCode;
			return Left.ErrorDetail < Right.ErrorDetail;
		});
		OutReport.bValid = OutReport.Issues.IsEmpty();
		OutReport.Status = OutReport.bValid ? TEXT("valid") : TEXT("invalid");
		OutReport.ErrorCode = OutReport.bValid ? TEXT("shared_core_bindings_valid") : TEXT("shared_core_bindings_invalid");
		return OutReport.bValid;
	}
}
