#include "Composition/SimpleAuthoring/Families/Landscape/UeAgentLandscapeSimpleAuthoringProduction.h"
#include "Core/Contract/UeAgentContractJson.h"

#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/Landscape/UeAgentLandscapeHeightState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Landscape.h"
#include "LandscapeDataAccess.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::Composition::SimpleAuthoring::Landscape
{
	namespace
	{
		using namespace AssetDurable;
		using namespace ContextResource;
		using namespace Contract;
		using namespace Discovery;
		using namespace DomainAdapters::GenericEffect;
		using namespace
			DomainAdapters::LandscapeGeometryEffect;
		using namespace DomainAdapters::WorldSimpleEffect;
		using namespace ExecutionMutation;
		using namespace PlanningPreflight;
		using namespace StateSemantic;
		using namespace Verification;

		constexpr const TCHAR* PreStatePortId =
			TEXT("landscape-geometry.authoring-memory.v1");
		constexpr const TCHAR* PersistedStatePortId =
			TEXT("landscape-geometry.independent-readback.v1");
		constexpr const TCHAR* RequestEvidencePrefix =
			TEXT("landscape-geometry-request-v1:");

		enum class ELandscapeProfileMechanic : uint8
		{
			Create,
			HeightDelta,
		};

		struct FLandscapeProfileSpec
		{
			const TCHAR* DescriptorId;
			const TCHAR* CapabilityId;
			const TCHAR* NativeTargetKind;
			const TCHAR* StateKind;
			ELandscapeProfileMechanic Mechanic;
			EUeAgentCandidateOperation Operation;
		};

		const FLandscapeProfileSpec LandscapeProfiles[] = {
			{
				TEXT("uai.operation.landscape.create"),
				TEXT("landscape.create"),
				TEXT("landscape.geometry.create"),
				TEXT("landscape_geometry"),
				ELandscapeProfileMechanic::Create,
				EUeAgentCandidateOperation::Create,
			},
			{
				TEXT("uai.operation.landscape.height_delta"),
				TEXT("landscape.height_delta"),
				TEXT("landscape.geometry.height_delta"),
				TEXT("landscape_height_region"),
				ELandscapeProfileMechanic::HeightDelta,
				EUeAgentCandidateOperation::Update,
			},
		};

		const FLandscapeProfileSpec* FindProfile(
			const FUeAgentDescriptorIdentity& Identity)
		{
			for (const FLandscapeProfileSpec& Profile
				: LandscapeProfiles)
			{
				if (Identity.GetDescriptorId()
					== Profile.DescriptorId)
				{
					return &Profile;
				}
			}
			return nullptr;
		}

		void SetProductionError(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& SubjectId = FString())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SubjectId;
        }

		FDiagnostic LandscapeFailure(const FString& Code, const FString& Reason, const FString& Field)
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("simple_authoring"));
			Error.Schema = TEXT("uai.runtime.diagnostic/1");
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			return Error;
		}

		FDiagnostic JsonValueFailure(const FString& Path, const TSharedPtr<FJsonValue>& Actual, const FString& Expected)
		{
			auto Error = LandscapeFailure(TEXT("landscape_geometry_state_field_invalid"),
				TEXT("The Landscape state field does not satisfy the required value."), Path);
			Error.ActualValuePresent = Actual.IsValid();
			if (Actual.IsValid() && (Actual->Type == EJson::Array || Actual->Type == EJson::Object))
			{
				auto Summary = MakeShared<FJsonObject>();
				Summary->SetStringField(TEXT("kind"), Actual->Type == EJson::Array ? TEXT("array") : TEXT("object"));
				if (Actual->Type == EJson::Array) Summary->SetNumberField(TEXT("count"), Actual->AsArray().Num());
				else if (Actual->AsObject().IsValid()) Summary->SetNumberField(TEXT("count"), Actual->AsObject()->Values.Num());
				Error.ActualValue = MakeShared<FJsonValueObject>(Summary);
			}
			else Error.ActualValue = Actual;
			Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			return Error;
		}

		FDiagnostic JsonFieldFailure(const TSharedPtr<FJsonObject>& Object, const FString& Field,
			const FString& Expected, const FString& Path = FString())
		{
			return JsonValueFailure(Path.IsEmpty() ? Field : Path,
				Object.IsValid() ? Object->Values.FindRef(Field) : nullptr, Expected);
		}

		FDiagnostic MissingProfile(const FUeAgentDescriptorIdentity& Identity)
		{
			auto Error = LandscapeFailure(TEXT("landscape_geometry_profile_missing"),
				TEXT("No Landscape authoring profile is registered for the requested descriptor."), TEXT("descriptor_id"));
			Error.ActualValue = MakeShared<FJsonValueString>(Identity.GetDescriptorId());
			TArray<TSharedPtr<FJsonValue>> Supported;
			for (const FLandscapeProfileSpec& Profile : LandscapeProfiles)
				Supported.Add(MakeShared<FJsonValueString>(Profile.DescriptorId));
			Error.ExpectedValue = MakeShared<FJsonValueArray>(Supported);
			return Error;
		}

		FDiagnostic CountFailure(const FString& Code, const FString& Reason, const FString& Field, const int32 Actual, const int32 Expected)
		{
			auto Error = LandscapeFailure(Code, Reason, Field);
			Error.ActualValue = MakeShared<FJsonValueNumber>(Actual);
			Error.ExpectedValue = MakeShared<FJsonValueNumber>(Expected);
			return Error;
		}

		bool TryParseRequestEvidence(
			const FUeAgentCandidateBinding& Binding,
			TSharedPtr<FJsonObject>& OutPayload,
			FUeAgentDiscoveryError& OutError)
		{
			const FString& Evidence = Binding.GetBindingEvidence();
			if (!Evidence.StartsWith(RequestEvidencePrefix))
			{
				OutError = LandscapeFailure(TEXT("landscape_geometry_request_evidence_invalid"),
					TEXT("The Landscape binding evidence has an unsupported format prefix."), TEXT("binding_evidence/prefix"));
				OutError.SubjectId = Binding.GetBindingId();
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Evidence.Left(FCString::Strlen(RequestEvidencePrefix)));
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(RequestEvidencePrefix);
				return false;
			}
			if (!Contract::Json::TryParseObject(Evidence.RightChop(FCString::Strlen(RequestEvidencePrefix)), OutPayload, OutError))
			{
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("binding_evidence");
				OutError.SubjectId = Binding.GetBindingId();
				return false;
			}
			return true;
		}

		bool TryGetTargetRef(const TSharedPtr<FJsonObject>& Payload, FString& OutTargetRef, FDiagnostic& OutError,
			const FString& SourceKind = TEXT("native_result"))
		{
			OutTargetRef.Reset();
			if (Payload.IsValid() && Payload->TryGetStringField(TEXT("target_ref"), OutTargetRef) && !OutTargetRef.IsEmpty())
				return true;
			OutError = JsonFieldFailure(Payload, TEXT("target_ref"), TEXT("nonempty string"));
			OutError.SourceKind = SourceKind;
			if (SourceKind == TEXT("command_input")) OutError.Source.JsonPath = TEXT("/target_ref");
			return false;
		}

		bool TryReadNumberArray(
			const TSharedPtr<FJsonObject>& Object,
			const FString& Field,
			const int32 ExpectedCount,
			TArray<double>& OutValues,
			FDiagnostic& OutError,
			const FString& ParentPath = FString())
		{
			OutValues.Reset();
			const FString Path = ParentPath.IsEmpty() ? Field : ParentPath + TEXT("/") + Field;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || !Values)
			{
				OutError = JsonFieldFailure(Object, Field, TEXT("number array"), Path);
				return false;
			}
			if (Values->Num() != ExpectedCount)
			{
				OutError = CountFailure(TEXT("landscape_geometry_state_array_size_invalid"),
					TEXT("The Landscape state array has the wrong number of elements."), Path, Values->Num(), ExpectedCount);
				return false;
			}
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
				double Number = 0.0;
				if (!Value.IsValid() || !Value->TryGetNumber(Number) || !FMath::IsFinite(Number))
				{
					OutError = JsonValueFailure(Path + TEXT("/") + FString::FromInt(Index), Value, TEXT("finite number"));
					return false;
				}
				OutValues.Add(Number);
			}
			return true;
		}

		bool TryReadIntegerArray(
			const TSharedPtr<FJsonObject>& Object,
			const FString& Field,
			const int32 ExpectedCount,
			TArray<int32>& OutValues,
			FDiagnostic& OutError,
			const FString& ParentPath)
		{
			TArray<double> Numbers;
			if (!TryReadNumberArray(Object, Field, ExpectedCount, Numbers, OutError, ParentPath))
				return false;
			OutValues.Reset();
			for (int32 Index = 0; Index < Numbers.Num(); ++Index)
			{
				const double Number = Numbers[Index];
				const int32 Integer = FMath::RoundToInt(Number);
				if (!FMath::IsNearlyEqual(Number, static_cast<double>(Integer), UE_DOUBLE_SMALL_NUMBER))
				{
					OutError = JsonValueFailure(ParentPath + TEXT("/") + Field + TEXT("/") + FString::FromInt(Index),
						MakeShared<FJsonValueNumber>(Number), TEXT("integer within int32 range"));
					return false;
				}
				OutValues.Add(Integer);
			}
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> NumberArray(
			const TArray<double>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Values.Num());
			for (const double Value : Values)
			{
				Result.Add(MakeShared<FJsonValueNumber>(Value));
			}
			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> IntegerArray(
			const TArray<int32>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Values.Num());
			for (const int32 Value : Values)
			{
				Result.Add(MakeShared<FJsonValueNumber>(Value));
			}
			return Result;
		}

		bool TryCanonicalValue(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError,
			const FString& Path = TEXT("state"))
		{
			OutValue.Reset();
			if (!JsonValue.IsValid())
			{
				OutError = LandscapeFailure(TEXT("landscape_geometry_state_json_invalid"),
					TEXT("The generated Landscape state contains a missing JSON value."), Path);
				OutError.ActualValuePresent = false;
				OutError.ExpectedValuePresent = true;
				return false;
			}
			const auto Complete = [&](const bool bSucceeded)
			{
				if (!bSucceeded)
				{
					OutError.SourceKind = TEXT("native_result");
					OutError.PropertyPath = OutError.PropertyPath.IsEmpty()
						? Path : Path + (OutError.PropertyPath.StartsWith(TEXT("/")) ? OutError.PropertyPath : TEXT("/") + OutError.PropertyPath);
				}
				return bSucceeded;
			};
			switch (JsonValue->Type)
			{
			case EJson::Null:
				return Complete(FUeAgentCanonicalValue::TryNull(
					Resources,
					OutValue,
					OutError));
			case EJson::Boolean:
				return Complete(FUeAgentCanonicalValue::TryBoolean(
					JsonValue->AsBool(),
					Resources,
					OutValue,
					OutError));
			case EJson::String:
				return Complete(FUeAgentCanonicalValue::TryString(
					JsonValue->AsString(),
					Resources,
					OutValue,
					OutError));
			case EJson::Number:
				return Complete(FUeAgentCanonicalValue::TryDecimal(
					FString::SanitizeFloat(
						JsonValue->AsNumber()),
					Resources,
					OutValue,
					OutError));
			case EJson::Array:
				{
					TArray<FUeAgentCanonicalValueRef> Items;
					const auto& Values = JsonValue->AsArray();
					for (int32 Index = 0; Index < Values.Num(); ++Index)
					{
						FUeAgentCanonicalValuePtr CanonicalItem;
						if (!TryCanonicalValue(
								Values[Index],
								Resources,
								CanonicalItem,
								OutError,
								Path + TEXT("/") + FString::FromInt(Index)))
						{
							return false;
						}
						Items.Add(CanonicalItem.ToSharedRef());
					}
					return Complete(FUeAgentCanonicalValue::TryArray(
						Items,
						Resources,
						OutValue,
						OutError));
				}
			case EJson::Object:
				{
					TArray<
						TPair<
							FString,
							FUeAgentCanonicalValueRef>> Items;
					for (const TPair<
						FString,
						TSharedPtr<FJsonValue>>& Pair
						: JsonValue->AsObject()->Values)
					{
						FUeAgentCanonicalValuePtr CanonicalItem;
						if (!TryCanonicalValue(
								Pair.Value,
								Resources,
								CanonicalItem,
								OutError,
								Path + TEXT("/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"))))
						{
							return false;
						}
						Items.Emplace(
							Pair.Key,
							CanonicalItem.ToSharedRef());
					}
					return Complete(FUeAgentCanonicalValue::TryObject(
						Items,
						Resources,
						OutValue,
						OutError));
				}
			default:
				OutError = LandscapeFailure(TEXT("landscape_geometry_state_json_kind_invalid"),
					TEXT("The generated Landscape state contains an unsupported JSON kind."), Path);
				OutError.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(JsonValue->Type));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("null, boolean, string, number, array or object"));
				return false;
			}
		}

		bool TryCanonicalObject(
			const TSharedPtr<FJsonObject>& Object,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			return TryCanonicalValue(
				MakeShared<FJsonValueObject>(Object),
				Resources,
				OutValue,
				OutError);
		}

		FUeAgentCanonicalValuePtr FindField(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field)
		{
			if (!Object.IsValid()
				|| Object->GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<
				FString,
				FUeAgentCanonicalValueRef>& Item
					: Object->GetObjectItems())
			{
				if (Item.Key == Field)
				{
					return FUeAgentCanonicalValuePtr(Item.Value);
				}
			}
			return nullptr;
		}

		FDiagnostic CanonicalValueFailure(const FUeAgentCanonicalValuePtr& Value, const FString& Path, const FString& Expected)
		{
			auto Error = LandscapeFailure(TEXT("landscape_geometry_canonical_field_invalid"),
				TEXT("The Landscape canonical state field does not satisfy the required value."), Path);
			Error.ActualValuePresent = Value.IsValid();
			Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			if (!Value.IsValid()) return Error;
			auto Actual = MakeShared<FJsonObject>();
			switch (Value->GetKind())
			{
			case EUeAgentValueKind::Null: Actual->SetStringField(TEXT("kind"), TEXT("null")); break;
			case EUeAgentValueKind::Boolean:
				Actual->SetStringField(TEXT("kind"), TEXT("boolean"));
				Actual->SetBoolField(TEXT("value"), Value->GetBoolean()); break;
			case EUeAgentValueKind::String:
				Actual->SetStringField(TEXT("kind"), TEXT("string"));
				Actual->SetStringField(TEXT("value"), Value->GetText()); break;
			case EUeAgentValueKind::Integer:
			case EUeAgentValueKind::Decimal:
				Actual->SetStringField(TEXT("kind"), Value->GetKind() == EUeAgentValueKind::Integer ? TEXT("integer") : TEXT("decimal"));
				Actual->SetStringField(TEXT("value"), Value->GetLexeme()); break;
			case EUeAgentValueKind::Array:
				Actual->SetStringField(TEXT("kind"), TEXT("array"));
				Actual->SetNumberField(TEXT("count"), Value->GetArrayItems().Num()); break;
			case EUeAgentValueKind::Object:
				Actual->SetStringField(TEXT("kind"), TEXT("object"));
				Actual->SetNumberField(TEXT("count"), Value->GetObjectItems().Num()); break;
			case EUeAgentValueKind::Temporal: Actual->SetStringField(TEXT("kind"), TEXT("temporal")); break;
			case EUeAgentValueKind::Spatial: Actual->SetStringField(TEXT("kind"), TEXT("spatial")); break;
			default: Actual->SetNumberField(TEXT("kind"), static_cast<int32>(Value->GetKind())); break;
			}
			Error.ActualValue = MakeShared<FJsonValueObject>(Actual);
			return Error;
		}

		FDiagnostic CanonicalFieldFailure(const FUeAgentCanonicalValuePtr& Object, const FString& Field,
			const FString& Path, const FString& Expected)
		{
			if (!Object.IsValid() || Object->GetKind() != EUeAgentValueKind::Object)
			{
				const FString ParentPath = Path == Field ? TEXT("state") : Path.LeftChop(Field.Len() + 1);
				return CanonicalValueFailure(Object, ParentPath, TEXT("object"));
			}
			return CanonicalValueFailure(FindField(Object, Field), Path, Expected);
		}

		bool ReadCanonicalNumbers(const FUeAgentCanonicalValuePtr& Object, const FString& Field,
			TArray<double>& OutValues, FDiagnostic& OutError, const FString& ParentPath = FString())
		{
			OutValues.Reset();
			const FString Path = ParentPath.IsEmpty() ? Field : ParentPath + TEXT("/") + Field;
			const FUeAgentCanonicalValuePtr Array = FindField(Object, Field);
			if (!Array.IsValid() || Array->GetKind() != EUeAgentValueKind::Array)
			{
				OutError = CanonicalFieldFailure(Object, Field, Path, TEXT("number array"));
				return false;
			}
			for (int32 Index = 0; Index < Array->GetArrayItems().Num(); ++Index)
			{
				const FUeAgentCanonicalValueRef& Item = Array->GetArrayItems()[Index];
				if (Item->GetKind() != EUeAgentValueKind::Decimal && Item->GetKind() != EUeAgentValueKind::Integer)
				{
					OutError = CanonicalValueFailure(Item, Path + TEXT("/") + FString::FromInt(Index), TEXT("finite number"));
					return false;
				}
				const double Value = FCString::Atod(*Item->GetLexeme());
				if (!FMath::IsFinite(Value))
				{
					OutError = CanonicalValueFailure(Item, Path + TEXT("/") + FString::FromInt(Index), TEXT("finite number"));
					return false;
				}
				OutValues.Add(Value);
			}
			return true;
		}

		bool ReadCanonicalIntegers(const FUeAgentCanonicalValuePtr& Object, const FString& Field,
			TArray<int32>& OutValues, FDiagnostic& OutError, const FString& ParentPath = FString())
		{
			const FString Path = ParentPath.IsEmpty() ? Field : ParentPath + TEXT("/") + Field;
			const FUeAgentCanonicalValuePtr Array = FindField(Object, Field);
			if (!Array.IsValid() || Array->GetKind() != EUeAgentValueKind::Array)
			{
				OutError = CanonicalFieldFailure(Object, Field, Path, TEXT("integer array"));
				return false;
			}
			OutValues.Reset();
			for (int32 Index = 0; Index < Array->GetArrayItems().Num(); ++Index)
			{
				const FUeAgentCanonicalValueRef& Item = Array->GetArrayItems()[Index];
				int64 Integer = 0;
				if ((Item->GetKind() != EUeAgentValueKind::Integer && Item->GetKind() != EUeAgentValueKind::Decimal)
					|| !TryParseInt64Number(Item->GetLexeme(), Integer) || Integer < MIN_int32 || Integer > MAX_int32)
				{
					OutError = CanonicalValueFailure(Item, Path + TEXT("/") + FString::FromInt(Index), TEXT("integer within int32 range"));
					return false;
				}
				OutValues.Add(static_cast<int32>(Integer));
			}
			return true;
		}

		bool ReadCanonicalString(const FUeAgentCanonicalValuePtr& Object, const FString& Field,
			FString& OutValue, FDiagnostic& OutError)
		{
			OutValue.Reset();
			const FUeAgentCanonicalValuePtr Value = FindField(Object, Field);
			if (!Value.IsValid() || Value->GetKind() != EUeAgentValueKind::String)
			{
				OutError = CanonicalFieldFailure(Object, Field, Field, TEXT("nonempty string"));
				return false;
			}
			OutValue = Value->GetText();
			if (OutValue.IsEmpty())
			{
				OutError = CanonicalValueFailure(Value, Field, TEXT("nonempty string"));
				return false;
			}
			return true;
		}

		FUeAgentCanonicalType LandscapeStateType()
		{
			return FUeAgentCanonicalType(
				TEXT("landscape_geometry_current_state"),
				EUeAgentValueKind::Object,
				false,
				TEXT("object"));
		}

		FUeAgentDefaultValue LandscapeStateDefault(
			const FUeAgentCanonicalValueRef& Value)
		{
			return FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::Schema,
				TEXT(
					"landscape-geometry:empty-default"),
				TEXT(
					"Landscape geometry state has no implicit target"),
				Value);
		}

		FUeAgentStateDetails LandscapeStateDetails(
			const FLandscapeProfileSpec& Profile)
		{
			return FUeAgentStateDetails(
				Profile.CapabilityId,
				TEXT(""),
				{
					TEXT("Landscape"),
					TEXT("SimpleAuthoring"),
					Profile.StateKind,
				},
				true,
				TEXT("available"),
				{});
		}

		FUeAgentCanonicalIdentity StateIdentity(
			const FLandscapeProfileSpec& Profile,
			const FUeAgentCandidateBinding& Binding,
			const FString& TargetRef)
		{
			const FUeAgentCanonicalIdentity& Root =
				Binding.GetSummary().GetIdentity();
			return FUeAgentCanonicalIdentity(
				TEXT("uai.landscape.geometry:") + TargetRef,
				Profile.NativeTargetKind,
				Root.GetOwnerId(),
				TargetRef,
				TEXT("landscape_geometry"),
				Root.GetGeneration());
		}

		EUeAgentCandidateOperation CandidateOperation(
			const EChangeIntent Intent)
		{
			switch (Intent)
			{
			case EChangeIntent::Create:
				return EUeAgentCandidateOperation::Create;
			case EChangeIntent::Delete:
				return EUeAgentCandidateOperation::Delete;
			case EChangeIntent::Update:
			default:
				return EUeAgentCandidateOperation::Update;
			}
		}

		bool SameExactWorld(
			const FUeAgentWorldSimpleEffectWorldCandidate& Candidate,
			const FUeAgentCandidateBinding& Binding,
			const bool bRequireSourceInstance)
		{
			const FUeAgentCanonicalIdentity& Identity =
				Binding.GetSummary().GetIdentity();
			return Candidate.IsValid()
				&& Candidate.GetWorld()
				&& Candidate.GetWorldIdentity()
					== Identity.GetOwnerId()
				&& Candidate.GetWorld()
					->GetOutermost()
					->GetName()
					== Binding.GetPublicRef()
				&& (!bRequireSourceInstance
					|| (Candidate.GetContextRevision()
							== Identity.GetGeneration()
						&& Candidate.GetWorld()->GetUniqueID()
							== Binding.GetCatalogRevision()
								.GetSequence()));
		}

		bool TryResolveExactWorld(
			IUeAgentWorldSimpleEffectWorldCatalog& Catalog,
			const FUeAgentCandidateBinding& Binding,
			const bool bRequireSourceInstance,
			UWorld*& OutWorld,
			FUeAgentDiscoveryError& OutError)
		{
			OutWorld = nullptr;
			TArray<FUeAgentWorldSimpleEffectWorldCandidate> Candidates;
			FExecutionMutationError Error;
			if (!Catalog.TryEnumerate(Candidates, Error))
			{
				OutError = Error;
				if (OutError.IsEmpty())
					OutError = LandscapeFailure(TEXT("landscape_geometry_state_catalog_unavailable"),
						TEXT("The Landscape world catalog failed to enumerate worlds without a diagnostic."), TEXT("world_catalog"));
				return false;
			}
			for (const FUeAgentWorldSimpleEffectWorldCandidate&
				Candidate : Candidates)
			{
				if (!SameExactWorld(
						Candidate,
						Binding,
						bRequireSourceInstance))
				{
					continue;
				}
				if (OutWorld)
				{
					OutError = CountFailure(TEXT("landscape_geometry_state_world_ambiguous"),
						TEXT("More than one loaded world matches the Landscape binding."), TEXT("world"), 2, 1);
					OutError.TargetPath = Binding.GetPublicRef();
					OutError.SubjectId = Binding.GetBindingId();
					OutError.StoppedEarly = true;
					OutWorld = nullptr;
					return false;
				}
				OutWorld = Candidate.GetWorld();
			}
			if (!OutWorld)
			{
				OutError = CountFailure(TEXT("landscape_geometry_state_world_unresolved"),
					TEXT("No loaded world matches the Landscape binding."), TEXT("world"), 0, 1);
				OutError.TargetPath = Binding.GetPublicRef();
				OutError.SubjectId = Binding.GetBindingId();
				return false;
			}
			return true;
		}

		TSharedPtr<FJsonObject> MakeRegion(
			const TArray<int32>& Min,
			const TArray<int32>& Max)
		{
			const TSharedPtr<FJsonObject> Region =
				MakeShared<FJsonObject>();
			Region->SetArrayField(
				TEXT("min_inclusive"),
				IntegerArray(Min));
			Region->SetArrayField(
				TEXT("max_inclusive"),
				IntegerArray(Max));
			return Region;
		}

		bool TryBuildLiveState(
			const FLandscapeProfileSpec& Profile,
			const TSharedPtr<FJsonObject>& Payload,
			ALandscape& Landscape,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentDiscoveryError& OutError)
		{
			FString TargetRef;
			const TSharedPtr<FJsonObject>* Region = nullptr;
			const FString RegionField =
				Profile.Mechanic
					== ELandscapeProfileMechanic::Create
				? TEXT("sample_region")
				: TEXT("region");
			if (!TryGetTargetRef(Payload, TargetRef, OutError)) return false;
			if (!Payload->TryGetObjectField(RegionField, Region) || !Region)
			{
				OutError = JsonFieldFailure(Payload, RegionField, TEXT("object"));
				OutError.TargetPath = TargetRef;
				return false;
			}
			TArray<int32> RegionMin;
			TArray<int32> RegionMax;
			if (!TryReadIntegerArray(*Region, TEXT("min_inclusive"), 2, RegionMin, OutError, RegionField)
				|| !TryReadIntegerArray(*Region, TEXT("max_inclusive"), 2, RegionMax, OutError, RegionField))
			{
				OutError.TargetPath = TargetRef;
				return false;
			}

			DomainAdapters::Landscape::FLandscapeHeightState
				HeightState;
			DomainAdapters::Landscape::FLandscapeHeightStateError
				HeightError;
			if (!DomainAdapters::Landscape::
				TryReadLandscapeHeightState(
					Landscape,
					FIntPoint(RegionMin[0], RegionMin[1]),
					FIntPoint(RegionMax[0], RegionMax[1]),
					HeightState,
					HeightError))
			{
				OutError = HeightError;
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
				return false;
			}

			const FVector Scale = HeightState.Scale;
			const TSharedPtr<FJsonObject> State =
				MakeShared<FJsonObject>();
			State->SetStringField(TEXT("target_ref"), TargetRef);
			State->SetBoolField(
				TEXT("identity_bound"),
				true);
			State->SetObjectField(
				TEXT("extent"),
				MakeRegion(
					{
						HeightState.ExtentMinInclusive.X,
						HeightState.ExtentMinInclusive.Y,
					},
					{
						HeightState.ExtentMaxInclusive.X,
						HeightState.ExtentMaxInclusive.Y,
					}));
			State->SetObjectField(
				TEXT("region"),
				MakeRegion(RegionMin, RegionMax));
			State->SetArrayField(
				TEXT("scale"),
				NumberArray({Scale.X, Scale.Y, Scale.Z}));
			State->SetStringField(
				TEXT("height_encoding"),
				TEXT("landscape_local_height"));
			State->SetArrayField(
				TEXT("heights"),
				NumberArray(HeightState.Heights));

			if (Profile.Mechanic
				== ELandscapeProfileMechanic::Create)
			{
				State->SetStringField(
					TEXT("managed_id"),
					UeAgentLevelAuthoring::GetManagedActorId(
						Landscape));
				State->SetStringField(
					TEXT("managed_folder"),
					UeAgentLevelAuthoring::GetActorFolderPathText(
						Landscape));
				State->SetBoolField(
					TEXT("managed_tag_present"),
					Landscape.ActorHasTag(
						UeAgentLevelAuthoring::
							GetManagedActorMarkerTag()));
				const TSharedPtr<FJsonObject>* Placement = nullptr;
				TArray<double> Translation;
				TArray<double> Rotation;
				TArray<double> ExpectedScale;
				FString Unit;
				bool bPlacementMatches = false;
				FDiagnostic PlacementDiagnostic;
				if (Payload->TryGetObjectField(
						TEXT("placement"),
						Placement)
					&& Placement
					&& (*Placement)->TryGetStringField(
						TEXT("unit"),
						Unit)
					&& TryReadNumberArray(
						*Placement,
						TEXT("translation"),
						3,
						Translation, PlacementDiagnostic)
					&& TryReadNumberArray(
						*Placement,
						TEXT(
							"rotation_quaternion_xyzw"),
						4,
						Rotation, PlacementDiagnostic)
					&& TryReadNumberArray(
						*Placement,
						TEXT("scale"),
						3,
						ExpectedScale, PlacementDiagnostic))
				{
					const double UnitScale =
						Unit == TEXT("meter") ? 100.0 : 1.0;
					const FVector ExpectedTranslation(
						Translation[0] * UnitScale,
						Translation[1] * UnitScale,
						Translation[2] * UnitScale);
					const FQuat ExpectedRotation(
						Rotation[0],
						Rotation[1],
						Rotation[2],
						Rotation[3]);
					bPlacementMatches =
						Landscape.GetActorLocation().Equals(
							ExpectedTranslation,
							UE_KINDA_SMALL_NUMBER)
						&& Landscape.GetActorQuat().Equals(
							ExpectedRotation,
							UE_KINDA_SMALL_NUMBER)
						&& Scale.Equals(
							FVector(
								ExpectedScale[0],
								ExpectedScale[1],
								ExpectedScale[2]),
							UE_KINDA_SMALL_NUMBER);
				}
				State->SetBoolField(
					TEXT("placement_matches"),
					bPlacementMatches);
			}

			FUeAgentStateSemanticError StateError;
			if (!TryCanonicalObject(
					State,
					Resources,
					OutValue,
					StateError))
			{
				OutError = StateError;
				return false;
			}
			return true;
		}

		const FUeAgentStateEntry* FindPreStateEntry(
			const FUeAgentCanonicalSnapshotRef& PreState,
			const FUeAgentCanonicalIdentity& Identity)
		{
			for (const FUeAgentStateEntry& Entry
				: PreState->GetEntries())
			{
				if (Entry.GetIdentity() == Identity)
				{
					return &Entry;
				}
			}
			return nullptr;
		}

		bool TryBuildExpectedCreateState(
			const FPlannedChange& Change,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentSimpleAuthoringProductionError& OutError)
		{
			const FUeAgentCanonicalValuePtr& After =
				Change.GetAfter();
			FString TargetRef;
			TArray<int32> RegionMin;
			TArray<int32> RegionMax;
			TArray<double> Scale;
			double InitialHeight = 0.0;
			const FUeAgentCanonicalValuePtr Initial =
				FindField(
					After,
					TEXT("initial_local_height"));
			if (!ReadCanonicalString(After, TEXT("target_ref"), TargetRef, OutError)
				|| !ReadCanonicalIntegers(After, TEXT("sample_min_inclusive"), RegionMin, OutError)
				|| !ReadCanonicalIntegers(After, TEXT("sample_max_inclusive"), RegionMax, OutError)
				|| !ReadCanonicalNumbers(After, TEXT("scale"), Scale, OutError))
			{
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
				OutError.SubjectId = Change.GetIdentity().GetCanonicalKey();
				return false;
			}
			if (!Initial.IsValid() || (Initial->GetKind() != EUeAgentValueKind::Decimal && Initial->GetKind() != EUeAgentValueKind::Integer))
			{
				OutError = CanonicalValueFailure(Initial, TEXT("initial_local_height"), TEXT("number"));
				OutError.TargetPath = TargetRef;
				OutError.SubjectId = Change.GetIdentity().GetCanonicalKey();
				return false;
			}
			InitialHeight =
				FCString::Atod(*Initial->GetLexeme());
			const int32 Width =
				RegionMax[0] - RegionMin[0] + 1;
			const int32 Height =
				RegionMax[1] - RegionMin[1] + 1;
			TArray<double> Heights;
			Heights.Init(
				InitialHeight,
				Width * Height);
			const TSharedPtr<FJsonObject> State =
				MakeShared<FJsonObject>();
			State->SetStringField(
				TEXT("target_ref"),
				TargetRef);
			State->SetBoolField(
				TEXT("identity_bound"),
				true);
			State->SetObjectField(
				TEXT("extent"),
				MakeRegion(RegionMin, RegionMax));
			State->SetObjectField(
				TEXT("region"),
				MakeRegion(RegionMin, RegionMax));
			State->SetArrayField(
				TEXT("scale"),
				NumberArray(Scale));
			State->SetStringField(
				TEXT("height_encoding"),
				TEXT("landscape_local_height"));
			State->SetArrayField(
				TEXT("heights"),
				NumberArray(Heights));
			State->SetBoolField(
				TEXT("placement_matches"),
				true);
			State->SetStringField(
				TEXT("managed_id"),
				TargetRef);
			State->SetStringField(
				TEXT("managed_folder"),
				UeAgentLevelAuthoring::MakeManagedActorFolderPath(
					TEXT("landscape")));
			State->SetBoolField(
				TEXT("managed_tag_present"),
				true);
			FUeAgentStateSemanticError StateError;
			if (!TryCanonicalObject(
					State,
					Resources,
					OutValue,
					StateError))
			{
				SetProductionError(OutError, StateError, Change.GetIdentity().GetCanonicalKey());
				return false;
			}
			return true;
		}

		bool TryBuildExpectedDeltaState(
			const FPlannedChange& Change,
			const FUeAgentCanonicalSnapshotRef& PreState,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentSimpleAuthoringProductionError& OutError)
		{
			const FUeAgentStateEntry* Before =
				FindPreStateEntry(
					PreState,
					Change.GetIdentity());
			if (!Before)
			{
				OutError = LandscapeFailure(TEXT("landscape_geometry_expected_prestate_missing"),
					TEXT("No pre-state entry matches the Landscape height change."), TEXT("pre_state"));
				OutError.SubjectId = Change.GetIdentity().GetCanonicalKey();
				OutError.ActualValuePresent = false;
				OutError.ExpectedValuePresent = true;
				return false;
			}
			const FUeAgentCanonicalValuePtr BeforeValue(
				Before->GetValue());
			FString TargetRef;
			TArray<double> Scale;
			TArray<double> Heights;
			TArray<double> Delta;
			TArray<int32> ExtentMin;
			TArray<int32> ExtentMax;
			TArray<int32> RegionMin;
			TArray<int32> RegionMax;
			const FUeAgentCanonicalValuePtr Extent =
				FindField(BeforeValue, TEXT("extent"));
			const FUeAgentCanonicalValuePtr Region =
				FindField(BeforeValue, TEXT("region"));
			if (!ReadCanonicalString(BeforeValue, TEXT("target_ref"), TargetRef, OutError)
				|| !ReadCanonicalNumbers(BeforeValue, TEXT("scale"), Scale, OutError)
				|| !ReadCanonicalNumbers(BeforeValue, TEXT("heights"), Heights, OutError)
				|| !ReadCanonicalIntegers(Extent, TEXT("min_inclusive"), ExtentMin, OutError, TEXT("extent"))
				|| !ReadCanonicalIntegers(Extent, TEXT("max_inclusive"), ExtentMax, OutError, TEXT("extent"))
				|| !ReadCanonicalIntegers(Region, TEXT("min_inclusive"), RegionMin, OutError, TEXT("region"))
				|| !ReadCanonicalIntegers(Region, TEXT("max_inclusive"), RegionMax, OutError, TEXT("region"))
				|| !ReadCanonicalNumbers(Change.GetAfter(), TEXT("delta_local_heights"), Delta, OutError))
			{
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
				OutError.SubjectId = Change.GetIdentity().GetCanonicalKey();
				return false;
			}
			if (Heights.Num() != Delta.Num())
			{
				OutError = CountFailure(TEXT("landscape_geometry_expected_delta_invalid"),
					TEXT("The Landscape height delta count differs from the pre-state height count."),
					TEXT("delta_local_heights"), Delta.Num(), Heights.Num());
				OutError.TargetPath = TargetRef;
				OutError.SubjectId = Change.GetIdentity().GetCanonicalKey();
				return false;
			}
			for (int32 Index = 0; Index < Heights.Num(); ++Index)
			{
				const float Quantized =
					LandscapeDataAccess::GetLocalHeight(
						LandscapeDataAccess::GetTexHeight(
							static_cast<float>(
								Heights[Index]
									+ Delta[Index])));
				Heights[Index] = Quantized;
			}
			const TSharedPtr<FJsonObject> State =
				MakeShared<FJsonObject>();
			State->SetStringField(
				TEXT("target_ref"),
				TargetRef);
			State->SetBoolField(
				TEXT("identity_bound"),
				true);
			State->SetObjectField(
				TEXT("extent"),
				MakeRegion(ExtentMin, ExtentMax));
			State->SetObjectField(
				TEXT("region"),
				MakeRegion(RegionMin, RegionMax));
			State->SetArrayField(
				TEXT("scale"),
				NumberArray(Scale));
			State->SetStringField(
				TEXT("height_encoding"),
				TEXT("landscape_local_height"));
			State->SetArrayField(
				TEXT("heights"),
				NumberArray(Heights));
			FUeAgentStateSemanticError StateError;
			if (!TryCanonicalObject(
					State,
					Resources,
					OutValue,
					StateError))
			{
				SetProductionError(OutError, StateError, Change.GetIdentity().GetCanonicalKey());
				return false;
			}
			return true;
		}

		class FLandscapeTargetResolver final
			: public IUeAgentSimpleAuthoringTargetResolver
		{
		public:
			explicit FLandscapeTargetResolver(
				TSharedRef<
					FUeAgentWorldSimpleEffectExactWorldResolver,
					ESPMode::ThreadSafe> InWorldResolver)
				: WorldResolver(MoveTemp(InWorldResolver))
			{
			}

			bool TryResolveTarget(
				const FUeAgentOperationRequest& CanonicalRequest,
				const FUeAgentExecutionContextRef& Context,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				TOptional<FUeAgentCandidateBinding>& OutTarget,
				FUeAgentSimpleAuthoringProductionError&
					OutError) override
			{
				OutTarget.Reset();
				const FLandscapeProfileSpec* Profile =
					FindProfile(
						CanonicalRequest
							.GetDescriptorIdentity());
				TSharedPtr<FJsonObject> Payload;
				FString TargetRef;
				if (!Profile)
				{
					OutError = MissingProfile(CanonicalRequest.GetDescriptorIdentity());
					return false;
				}
				if (!Contract::Json::TryParseObject(CanonicalRequest.GetPayload().GetJsonValue(), Payload, OutError))
				{
					if (OutError.SourceKind.IsEmpty()) OutError.SourceKind = TEXT("command_input");
					return false;
				}
				if (!TryGetTargetRef(Payload, TargetRef, OutError, TEXT("command_input"))) return false;
				UWorld* World = nullptr;
				FExecutionMutationError Error;
				if (Cancellation.IsCancelled())
				{
					OutError = LandscapeFailure(TEXT("landscape_geometry_target_unresolved"),
						TEXT("Landscape target resolution was cancelled before resolving the world."), TEXT("cancellation"));
					OutError.TargetPath = TargetRef;
					return false;
				}
				const bool bResolved = WorldResolver->TryResolveWorld(Context, World, Error);
				if (!bResolved || !World || !World->PersistentLevel)
				{
					OutError = Error;
					if (OutError.IsEmpty())
					{
						OutError = LandscapeFailure(TEXT("landscape_geometry_target_unresolved"),
							!bResolved ? TEXT("The Landscape world resolver failed without a diagnostic.")
								: !World ? TEXT("The Landscape world resolver returned no world.")
								: TEXT("The resolved Landscape world has no persistent level."),
							World ? TEXT("world/persistent_level") : TEXT("world"));
						OutError.ActualValuePresent = false;
						OutError.ExpectedValuePresent = true;
						OutError.SubjectId = Context.GetWorldIdentity();
					}
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
					return false;
				}
				const FUeAgentCanonicalIdentity Identity(
					TEXT("uai.landscape.operation:")
						+ TargetRef,
					TEXT("landscape.geometry.operation"),
					Context.GetWorldIdentity(),
					TargetRef,
					TEXT("landscape_geometry"),
					Context.GetContextRevision());
				OutTarget.Emplace(
					TEXT("landscape-geometry:")
						+ CanonicalRequest.GetRequestId(),
					FUeAgentCandidateSummary(
						Identity,
						CanonicalRequest
							.GetDescriptorIdentity(),
						TEXT("landscape_geometry"),
						TargetRef,
						TEXT(
							"exact Landscape geometry operation target"),
						TArray<FString>{
							TEXT("Landscape"),
							TEXT("SimpleAuthoring"),
						}),
					FUeAgentCatalogRevision(
						TEXT(
							"landscape-geometry-exact-context"),
						Context.GetContextRevision(),
						World->GetUniqueID()),
					World->GetOutermost()->GetName(),
					PersistedStatePortId,
					TArray<EUeAgentCandidateOperation>{
						Profile->Operation,
					},
					FString(RequestEvidencePrefix)
						+ CanonicalRequest
							.GetPayload()
							.GetJsonValue());
				if (!OutTarget->IsValid())
				{
					OutError = *OutTarget->GetValidationDiagnostic();
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
					OutTarget.Reset();
					return false;
				}
				return true;
			}

		private:
			TSharedRef<
				FUeAgentWorldSimpleEffectExactWorldResolver,
				ESPMode::ThreadSafe> WorldResolver;
		};

		class FLandscapeStateReadPort final
			: public IUeAgentStateReadPort
		{
		public:
			FLandscapeStateReadPort(
				FString InPortId,
				const EUeAgentStatePlane InRequiredPlane,
				TSharedRef<
					IUeAgentWorldSimpleEffectWorldCatalog,
					ESPMode::ThreadSafe> InCatalog,
				TSharedRef<
					IUeAgentLandscapeGeometryEffectReferenceResolver,
					ESPMode::ThreadSafe> InReferenceResolver,
				const bool bInRequireSourceInstance)
				: PortId(MoveTemp(InPortId))
				, RequiredPlane(InRequiredPlane)
				, Catalog(MoveTemp(InCatalog))
				, ReferenceResolver(
					MoveTemp(InReferenceResolver))
				, bRequireSourceInstance(
					bInRequireSourceInstance)
			{
			}

			const FString& GetPortId() const override
			{
				return PortId;
			}

			bool TryRead(
				const FUeAgentStateReadRequest& Request,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				TSharedPtr<const FUeAgentCanonicalSnapshot>&
					OutSnapshot,
				FUeAgentDiscoveryError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = FUeAgentDiscoveryError();
				const FLandscapeProfileSpec* Profile =
					Request.IsValid()
					? FindProfile(
						Request.GetBinding()
							.GetSummary()
							.GetDescriptorIdentity())
					: nullptr;
				if (!Profile)
				{
					OutError = !Request.IsValid() ? *Request.GetBinding().GetValidationDiagnostic()
						: MissingProfile(Request.GetBinding().GetSummary().GetDescriptorIdentity());
					return false;
				}
				if (Request.GetStatePlane() != RequiredPlane)
				{
					OutError = LandscapeFailure(TEXT("landscape_geometry_state_request_invalid"),
						TEXT("The Landscape read port does not serve the requested state plane."), TEXT("state_plane"));
					FString Actual = Contract::LexToString(Request.GetStatePlane());
					if (Actual.IsEmpty()) Actual = FString::FromInt(static_cast<int32>(Request.GetStatePlane()));
					OutError.ActualValue = MakeShared<FJsonValueString>(Actual);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(Contract::LexToString(RequiredPlane));
					OutError.TargetPath = Request.GetBinding().GetPublicRef();
					return false;
				}
				if (Cancellation.IsCancelled())
				{
					OutError = LandscapeFailure(TEXT("landscape_geometry_state_request_cancelled"),
						TEXT("The Landscape state read was cancelled before resolving its target."), TEXT("cancellation"));
					OutError.TargetPath = Request.GetBinding().GetPublicRef();
					return false;
				}
				const FUeAgentCandidateBinding& Binding =
					Request.GetBinding();
				TSharedPtr<FJsonObject> Payload;
				FString TargetRef;
				if (!TryParseRequestEvidence(
						Binding,
						Payload,
						OutError)
					|| !TryGetTargetRef(Payload, TargetRef, OutError))
				{
					return false;
				}
				UWorld* World = nullptr;
				if (!TryResolveExactWorld(
						*Catalog,
						Binding,
						bRequireSourceInstance,
						World,
						OutError))
				{
					return false;
				}

				ALandscape* Landscape = nullptr;
				DomainAdapters::Landscape::
					FLandscapeHeightStateError TargetError;
				const bool bTargetResolved =
					DomainAdapters::Landscape::
						TryResolveLandscapeHeightTarget(
							*World,
							TargetRef,
							&ReferenceResolver.Get(),
							Landscape,
							TargetError);
				if (Profile->Mechanic
					== ELandscapeProfileMechanic::Create)
				{
					if (RequiredPlane
						== EUeAgentStatePlane::AuthoringMemory)
					{
						if (bTargetResolved)
						{
							OutError = LandscapeFailure(TEXT("landscape_geometry_create_conflict"),
								TEXT("A Landscape already exists at the requested create target."), TEXT("target_ref"));
							OutError.TargetPath = TargetRef;
							OutError.ActualValuePresent = true;
							OutError.ExpectedValuePresent = false;
							return false;
						}
						if (TargetError.Code
							!= TEXT("landscape_height_target_not_found"))
						{
							OutError = TargetError;
							OutError.SubjectId = TargetRef;
							return false;
						}
						Landscape = nullptr;
					}
					else if (!bTargetResolved)
					{
						OutError = TargetError.Code == TEXT("landscape_height_target_not_found")
							? FDiagnostic::WrapFailure(TEXT("landscape_geometry_created_target_missing"), TargetError,
								TEXT("simple_authoring"), TEXT("The created Landscape could not be found during persisted-state readback."))
							: TargetError;
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
						return false;
					}
				}
				else if (!bTargetResolved)
				{
					OutError = TargetError;
					OutError.SubjectId = TargetRef;
					return false;
				}

				FUeAgentCanonicalValuePtr DefaultValue;
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryObject(
						{},
						Resources,
						DefaultValue,
						StateError))
				{
					OutError = StateError;
					return false;
				}
				const FUeAgentStateRevision Revision(
					PortId,
					Binding.GetSummary()
						.GetIdentity()
						.GetGeneration(),
					World->GetUniqueID());
				TArray<FUeAgentStateEntry> Entries;
				if (Landscape)
				{
					FUeAgentCanonicalValuePtr Value;
					if (!TryBuildLiveState(
							*Profile,
							Payload,
							*Landscape,
							Resources,
							Value,
							OutError))
					{
						return false;
					}
					const bool bAggregate =
						Binding.GetSummary()
							.GetIdentity()
							.GetTargetKind()
						== TEXT(
							"landscape.geometry.operation");
					const FUeAgentCanonicalIdentity Identity =
						bAggregate
							? StateIdentity(
								*Profile,
								Binding,
								TargetRef)
							: Binding.GetSummary()
								.GetIdentity();
					Entries.Emplace(
						Identity,
						LandscapeStateType(),
						Value.ToSharedRef(),
						LandscapeStateDefault(
							DefaultValue.ToSharedRef()),
						LandscapeStateDetails(*Profile),
						Revision,
						EUeAgentAuthoredPresence::Present,
						EUeAgentOverrideState::Overridden);
				}

				FUeAgentCanonicalSnapshotPtr Snapshot;
				if (!FUeAgentCanonicalSnapshot::TryCreate(
						TEXT(
							"snapshot:landscape-geometry:")
							+ PortId
							+ TEXT(":")
							+ Binding.GetBindingId(),
						RequiredPlane,
						Revision,
						FUeAgentTemporalPoint(
							TEXT(
								"landscape-geometry-operation"),
							TEXT("1"),
							TEXT("1"),
							TEXT("1"),
							TEXT("observed")),
						FUeAgentStateProvenance(
							TEXT("independent_readback"),
							PortId,
							TargetRef),
						MoveTemp(Entries),
						Resources,
						Snapshot,
						StateError))
				{
					OutError = StateError;
					OutError.SubjectId = TargetRef;
					return false;
				}
				OutSnapshot = MoveTemp(Snapshot);
				return true;
			}

		private:
			FString PortId;
			EUeAgentStatePlane RequiredPlane =
				EUeAgentStatePlane::AuthoringMemory;
			TSharedRef<
				IUeAgentWorldSimpleEffectWorldCatalog,
				ESPMode::ThreadSafe> Catalog;
			TSharedRef<
				IUeAgentLandscapeGeometryEffectReferenceResolver,
				ESPMode::ThreadSafe> ReferenceResolver;
			bool bRequireSourceInstance = true;
		};

		class FLandscapeDraftBuilder final
			: public IUeAgentSimpleAuthoringDraftBuilder
		{
		public:
			bool TryBuildDraft(
				const FUeAgentOperationRequest& CanonicalRequest,
				const FUeAgentExecutionContextRef& Context,
				const FUeAgentCanonicalSnapshotRef& PreState,
				FUeAgentResourceTracker& Resources,
				TOptional<FChangeSetDraft>& OutDraft,
				FUeAgentSimpleAuthoringProductionError&
					OutError) override
			{
				FExecutionMutationError Error;
				if (!TryBuildLandscapeGeometryEffectChangeSetDraft(
						CanonicalRequest,
						Context,
						PreState,
						Resources,
						OutDraft,
						Error))
				{
					SetProductionError(OutError, Error, CanonicalRequest.GetRequestId());
					return false;
				}
				return true;
			}
		};

		class FLandscapeChangeTargetProjector final
			: public IUeAgentSimpleAuthoringChangeTargetProjector
		{
		public:
			bool TryProjectChangeTargets(
				const FUeAgentOperationRequest& CanonicalRequest,
				const FUeAgentExecutionContextRef& Context,
				const FChangeSetDraft& Draft,
				const FUeAgentCandidateBinding& ResolvedTarget,
				const FUeAgentCanonicalSnapshotRef& PreState,
				FUeAgentResourceTracker& Resources,
				TArray<FUeAgentCandidateBinding>& OutTargets,
				FUeAgentSimpleAuthoringProductionError&
					OutError) override
			{
				OutTargets.Reset();
				const TArray<FChangeDraft>& Changes =
					Draft.GetChanges();
				for (int32 Index = 0; Index < Changes.Num(); ++Index)
				{
					const FChangeDraft& Change = Changes[Index];
					OutTargets.Emplace(
						ResolvedTarget.GetBindingId()
							+ TEXT(":change:")
							+ FString::FromInt(Index),
						FUeAgentCandidateSummary(
							Change.GetIdentity(),
							CanonicalRequest
								.GetDescriptorIdentity(),
							TEXT("landscape_geometry"),
							Change.GetIdentity().GetSelector(),
							TEXT(
								"exact Landscape geometry change target"),
							TArray<FString>{
								TEXT("Landscape"),
								TEXT("SimpleAuthoring"),
							}),
						ResolvedTarget.GetCatalogRevision(),
						ResolvedTarget.GetPublicRef(),
						ResolvedTarget.GetReadPortId(),
						TArray<EUeAgentCandidateOperation>{
							CandidateOperation(
								Change.GetIntent()),
						},
						ResolvedTarget.GetBindingEvidence());
					if (!OutTargets.Last().IsValid())
					{
						OutError = *OutTargets.Last().GetValidationDiagnostic();
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = OutTargets.Last().GetPublicRef();
						OutTargets.Reset();
						return false;
					}
				}
				if (OutTargets.IsEmpty())
				{
					OutError = CountFailure(TEXT("landscape_geometry_change_target_invalid"),
						TEXT("At least one Landscape change target is required."), TEXT("change_targets"), 0, 1);
					return false;
				}
				return true;
			}
		};

		class FLandscapeMutationBindingProjector final
			: public IUeAgentSimpleAuthoringMutationBindingProjector
		{
		public:
			bool TryProjectMutationBindings(
				const FPlanningPreflightPlanRef& Plan,
				const TArray<FUeAgentCandidateBinding>& Targets,
				const FUeAgentCanonicalSnapshotRef& PreState,
				const FUeAgentExecutionContextRef& Context,
				FUeAgentResourceTracker& Resources,
				TArray<FUeAgentGenericEffectBinding>&
					OutBindings,
				FUeAgentSimpleAuthoringProductionError&
					OutError) override
			{
				OutBindings.Reset();
				const TArray<FPlannedChange>& Changes =
					Plan->GetChangeSet().GetChanges();
				if (Changes.Num() != Targets.Num())
				{
					OutError = CountFailure(TEXT("landscape_geometry_change_targets_missing"),
						TEXT("The Landscape target count differs from the planned change count."), TEXT("targets"), Targets.Num(), Changes.Num());
					OutError.SubjectId = Plan->GetRequestId();
					return false;
				}
				for (int32 Index = 0; Index < Changes.Num(); ++Index)
				{
					OutBindings.Emplace(
						Targets[Index],
						TEXT(
							"UAI.LandscapeGeometry:CanonicalObject"),
						FUeAgentCanonicalType(
							TEXT(
								"landscape_geometry_canonical_object"),
							EUeAgentValueKind::Object,
							false,
							TEXT("object")),
						TEXT(
							"landscape-geometry:typed-native-binding"));
					if (Targets[Index].GetSummary().GetIdentity() != Changes[Index].GetIdentity())
					{
						OutError = LandscapeFailure(TEXT("landscape_geometry_mutation_binding_invalid"),
							TEXT("The Landscape mutation target identity differs from its planned change."),
							FString::Printf(TEXT("targets/%d/identity"), Index));
						OutError.TargetPath = Targets[Index].GetPublicRef();
						OutError.ActualEvidence = MakeShared<FJsonValueString>(Targets[Index].GetSummary().GetIdentity().GetCanonicalKey());
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Changes[Index].GetIdentity().GetCanonicalKey());
						OutBindings.Reset();
						return false;
					}
					if (!OutBindings.Last().IsValid())
					{
						OutError = OutBindings.Last().GetValidationDiagnostic();
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Targets[Index].GetPublicRef();
						OutBindings.Reset();
						return false;
					}
				}
				if (OutBindings.IsEmpty())
				{
					OutError = CountFailure(TEXT("landscape_geometry_mutation_binding_invalid"),
						TEXT("At least one Landscape mutation binding is required."), TEXT("mutation_bindings"), 0, 1);
					return false;
				}
				return true;
			}
		};

		class FLandscapeDurableTargetProjector final
			: public IUeAgentSimpleAuthoringDurableTargetProjector
		{
		public:
			bool TryProjectDurableTargets(
				const FPlanningPreflightPlanRef& Plan,
				const FUeAgentCandidateBinding& Target,
				const FUeAgentCanonicalSnapshotRef& PreState,
				FUeAgentResourceTracker& Resources,
				TArray<FAssetDurableTarget>& OutTargets,
				FUeAgentSimpleAuthoringProductionError&
					OutError) override
			{
				OutTargets.Reset();
				const TArray<FPlannedChange>& Changes =
					Plan->GetChangeSet().GetChanges();
				if (Changes.IsEmpty())
				{
					OutError = CountFailure(TEXT("landscape_geometry_durable_target_missing"),
						TEXT("A Landscape durable target requires at least one planned change."), TEXT("changes"), 0, 1);
					OutError.SubjectId = Target.GetBindingId();
					return false;
				}
				if (Target.GetPublicRef().IsEmpty())
				{
					OutError = LandscapeFailure(TEXT("landscape_geometry_durable_target_missing"),
						TEXT("The Landscape durable target has an empty public reference."), TEXT("target_ref"));
					OutError.ActualValue = MakeShared<FJsonValueString>(Target.GetPublicRef());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty package reference"));
					OutError.SubjectId = Target.GetBindingId();
					return false;
				}
				OutTargets.Emplace(
					Changes[0].GetIdentity(),
					Target.GetPublicRef(),
					EPackageHeaderKind::Map,
					EAssetDurableIntent::Save,
					ERollbackGrade::DurableBackup);
				if (!OutTargets[0].IsValid())
				{
					OutError = OutTargets[0].GetValidationDiagnostic();
					return false;
				}
				return true;
			}
		};

		class FLandscapeExpectedStateProjector final
			: public IUeAgentSimpleAuthoringExpectedStateProjector
		{
		public:
			bool TryProjectExpectedState(
				const FPlanningPreflightPlanRef& Plan,
				const FUeAgentCandidateBinding& ResolvedTarget,
				const TArray<FUeAgentCandidateBinding>&
					ChangeTargets,
				const FUeAgentCanonicalSnapshotRef& PreState,
				const FDurableInventory& Inventory,
				FUeAgentResourceTracker& Resources,
				TArray<FVerificationTarget>& OutTargets,
				FUeAgentSimpleAuthoringProductionError&
					OutError) override
			{
				OutTargets.Reset();
				const FLandscapeProfileSpec* Profile =
					FindProfile(
						Plan->GetDescriptorIdentity());
				if (!Profile)
				{
					OutError = MissingProfile(Plan->GetDescriptorIdentity());
					return false;
				}
				FUeAgentCanonicalValuePtr DefaultValue;
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryObject(
						{},
						Resources,
						DefaultValue,
						StateError))
				{
					SetProductionError(OutError, StateError, Plan->GetRequestId());
					return false;
				}
				const FUeAgentStateRevision Revision(
					TEXT(
						"landscape-geometry-operation-expected"),
					Plan->GetFingerprint(),
					1);
				TArray<FString> ArtifactIds;
				for (const FDurableArtifact& Artifact
					: Inventory.GetArtifacts())
				{
					ArtifactIds.Add(Artifact.GetArtifactId());
				}
				for (const FPlannedChange& Change
					: Plan->GetChangeSet().GetChanges())
				{
					const FUeAgentCandidateBinding* ChangeTarget =
						ChangeTargets.FindByPredicate(
							[&Change](
								const FUeAgentCandidateBinding&
									Candidate)
							{
								return Candidate.GetSummary()
									.GetIdentity()
									== Change.GetIdentity();
							});
					if (!ChangeTarget)
					{
						OutError = LandscapeFailure(TEXT("landscape_geometry_verification_target_missing"),
							TEXT("No Landscape candidate binding matches the planned change for verification."), TEXT("change_targets"));
						OutError.SubjectId = Change.GetIdentity().GetCanonicalKey();
						OutError.ActualValuePresent = false;
						OutError.ExpectedValuePresent = true;
						return false;
					}
					FUeAgentCanonicalValuePtr ExpectedValue;
					const bool bBuilt =
						Profile->Mechanic
							== ELandscapeProfileMechanic::Create
						? TryBuildExpectedCreateState(
							Change,
							Resources,
							ExpectedValue,
							OutError)
						: TryBuildExpectedDeltaState(
							Change,
							PreState,
							Resources,
							ExpectedValue,
							OutError);
					if (!bBuilt)
					{
						return false;
					}
					TArray<FUeAgentStateEntry> Entries;
					Entries.Emplace(
						Change.GetIdentity(),
						LandscapeStateType(),
						ExpectedValue.ToSharedRef(),
						LandscapeStateDefault(
							DefaultValue.ToSharedRef()),
						LandscapeStateDetails(*Profile),
						Revision,
						EUeAgentAuthoredPresence::Present,
						EUeAgentOverrideState::Overridden);
					FUeAgentCanonicalSnapshotPtr Expected;
					if (!FUeAgentCanonicalSnapshot::
							TryCreateExpected(
								TEXT(
									"snapshot:landscape-geometry:"
									"expected:")
									+ Plan->GetRequestId()
									+ TEXT(":")
									+ Change.GetIdentity()
										.GetCanonicalKey(),
								EUeAgentStatePlane::Persisted,
								Revision,
								FUeAgentTemporalPoint(
									TEXT(
										"landscape-geometry-operation"),
									TEXT("1"),
									TEXT("1"),
									TEXT("1"),
									TEXT("expected")),
								FUeAgentStateProvenance(
									TEXT(
										"operation_expected_state"),
									Plan->GetRequestId(),
									Plan->GetFingerprint()),
								MoveTemp(Entries),
								Resources,
								Expected,
								StateError))
					{
						SetProductionError(OutError, StateError, Change.GetIdentity()
								.GetCanonicalKey());
						return false;
					}
					OutTargets.Emplace(
						Change.GetIdentity(),
						*ChangeTarget,
						EVerificationOracleKind::Persisted,
						Expected,
						ArtifactIds,
						TArray<FString>());
					if (!OutTargets.Last().IsValid())
					{
						OutError = *OutTargets.Last().GetValidationDiagnostic();
						return false;
					}
				}
				if (OutTargets.IsEmpty())
				{
					OutError = CountFailure(TEXT("landscape_geometry_verification_target_missing"),
						TEXT("At least one Landscape verification target is required."), TEXT("verification_targets"), 0, 1);
					return false;
				}
				return true;
			}
		};

		class FLandscapeUeHook final
			: public IUeAgentGenericEffectUeHook
		{
		public:
			FLandscapeUeHook(
				TSharedRef<
					FUeAgentWorldSimpleEffectExactWorldResolver,
					ESPMode::ThreadSafe> InWorldResolver,
				TSharedRef<
					IUeAgentLandscapeGeometryEffectNativeHook,
					ESPMode::ThreadSafe> InNativeHook)
				: WorldResolver(MoveTemp(InWorldResolver))
				, NativeHook(MoveTemp(InNativeHook))
			{
			}

			bool TryApply(
				const FExecutionMutationRequest& Request,
				const FLockFenceLease& Lease,
				const TArray<FUeAgentGenericEffectBinding>&
					Bindings,
				FUeAgentResourceTracker& Resources,
				FUeAgentGenericEffectApplyResult& OutResult,
				FExecutionMutationError& OutError) override
			{
				UWorld* World = nullptr;
				if (!WorldResolver->TryResolveWorld(
						Request.GetExecutionContext(),
						World,
						OutError)
					|| !World)
				{
					return false;
				}
				return NativeHook->TryApply(
					*World,
					Request,
					Lease,
					Bindings,
					Resources,
					OutResult,
					OutError);
			}

			bool TryRollback(
				const FExecutionMutationRequest& Request,
				const FUeAgentGenericEffectApplyResult& Applied,
				FUeAgentResourceTracker& Resources,
				bool& bOutConfirmed,
				FExecutionMutationError& OutError) override
			{
				UWorld* World = nullptr;
				if (!WorldResolver->TryResolveWorld(
						Request.GetExecutionContext(),
						World,
						OutError)
					|| !World)
				{
					return false;
				}
				return NativeHook->TryRollback(
					*World,
					Request,
					Applied,
					Resources,
					bOutConfirmed,
					OutError);
			}

			void ReleaseVerifiedOperation(
				const FString& OperationId) override
			{
				NativeHook->ReleaseVerifiedOperation(OperationId);
			}

			bool TryWaitForSettle(
				const FBarrierRequest& Request,
				FUeAgentResourceTracker& Resources,
				FBarrierResult& OutResult,
				FExecutionMutationError& OutError) override
			{
				return NativeHook->TryWaitForSettle(
					Request,
					Resources,
					OutResult,
					OutError);
			}

		private:
			TSharedRef<
				FUeAgentWorldSimpleEffectExactWorldResolver,
				ESPMode::ThreadSafe> WorldResolver;
			TSharedRef<
				IUeAgentLandscapeGeometryEffectNativeHook,
				ESPMode::ThreadSafe> NativeHook;
		};

		bool ValidateConfig(
			const FUeAgentLandscapeSimpleAuthoringProviderConfig&
				Config,
			FUeAgentLandscapeSimpleAuthoringError& OutError)
		{
			const TCHAR* Missing = !Config.DescriptorRegistry.IsValid() ? TEXT("descriptor_registry")
				: !Config.WorldCatalog.IsValid() ? TEXT("world_catalog")
				: !Config.ReferenceResolver.IsValid() ? TEXT("reference_resolver")
				: !Config.DurableService.IsValid() ? TEXT("durable_service")
				: !Config.AbsenceOracle.IsValid() ? TEXT("absence_oracle")
				: !Config.ReferenceOracle.IsValid() ? TEXT("reference_oracle")
				: !Config.Projection.IsSet() ? TEXT("projection") : nullptr;
			if (Missing)
			{
				OutError = LandscapeFailure(TEXT("landscape_simple_authoring_dependencies_missing"),
					FString::Printf(TEXT("The Landscape authoring provider has no '%s'."), Missing), Missing);
				OutError.ActualValuePresent = false;
				OutError.ExpectedValuePresent = true;
				return false;
			}
			if (!Config.Projection->IsValid())
			{
				OutError = Config.Projection->GetValidationDiagnostic();
				return false;
			}
			if (Config.OwnerLeaseId.IsEmpty())
			{
				OutError = LandscapeFailure(TEXT("landscape_simple_authoring_dependencies_missing"),
					TEXT("The Landscape authoring provider has an empty owner lease identifier."), TEXT("owner_lease_id"));
				OutError.ActualValue = MakeShared<FJsonValueString>(TEXT(""));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty identifier"));
				return false;
			}
			for (const FLandscapeProfileSpec& Profile
				: LandscapeProfiles)
			{
				const FUeAgentOperationDescriptor* Descriptor =
					Config.DescriptorRegistry
						->FindByDescriptorId(
							Profile.DescriptorId);
				if (!Descriptor)
				{
					OutError = LandscapeFailure(TEXT("landscape_simple_authoring_descriptor_missing"),
						TEXT("The required Landscape operation descriptor is not registered."), TEXT("descriptor_id"));
					OutError.CandidateName = Profile.DescriptorId;
					OutError.ActualValuePresent = false;
					OutError.ExpectedValue = MakeShared<FJsonValueString>(Profile.DescriptorId);
					return false;
				}
				if (Descriptor->GetCapabilityId() != Profile.CapabilityId || Descriptor->GetDomainId() != TEXT("landscape"))
				{
					const bool bCapabilityMismatch = Descriptor->GetCapabilityId() != Profile.CapabilityId;
					OutError = LandscapeFailure(TEXT("landscape_simple_authoring_descriptor_missing"),
						TEXT("The registered Landscape descriptor does not match its profile."),
						bCapabilityMismatch ? TEXT("capability_id") : TEXT("domain_id"));
					OutError.CandidateName = Profile.DescriptorId;
					OutError.ActualValue = MakeShared<FJsonValueString>(bCapabilityMismatch ? Descriptor->GetCapabilityId() : Descriptor->GetDomainId());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(bCapabilityMismatch ? Profile.CapabilityId : TEXT("landscape"));
					return false;
				}
			}
			return true;
		}
	}

	bool FUeAgentLandscapeSimpleAuthoringProviderFactory::TryCreate(
		const FUeAgentLandscapeSimpleAuthoringProviderConfig& Config,
		TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider>&
			OutProvider,
		FUeAgentLandscapeSimpleAuthoringError& OutError)
	{
		OutProvider.Reset();
		OutError = FUeAgentLandscapeSimpleAuthoringError();
		if (!ValidateConfig(Config, OutError))
		{
			return false;
		}

		const TSharedRef<
			IUeAgentWorldSimpleEffectWorldCatalog,
			ESPMode::ThreadSafe> Catalog =
				Config.WorldCatalog.ToSharedRef();
		const TSharedRef<
			IUeAgentWorldSimpleEffectWorldCatalog,
			ESPMode::ThreadSafe> PersistedCatalog =
				Config.PersistedWorldCatalog.IsValid()
					? Config.PersistedWorldCatalog.ToSharedRef()
					: Catalog;
		const TSharedRef<
			IUeAgentLandscapeGeometryEffectReferenceResolver,
			ESPMode::ThreadSafe> PersistedReferenceResolver =
				Config.PersistedReferenceResolver.IsValid()
					? Config.PersistedReferenceResolver
						.ToSharedRef()
					: Config.ReferenceResolver.ToSharedRef();
		const TSharedRef<
			FUeAgentWorldSimpleEffectExactWorldResolver,
			ESPMode::ThreadSafe> WorldResolver =
				MakeShared<
					FUeAgentWorldSimpleEffectExactWorldResolver,
					ESPMode::ThreadSafe>(Catalog);
		TSharedPtr<IUeAgentLandscapeGeometryEffectNativeHook>
			NativeHook;
		FExecutionMutationError NativeError;
		if (!FUeAgentLandscapeGeometryEffectNativeHookFactory::
				TryCreate(
					Config.ReferenceResolver.ToSharedRef(),
					NativeHook,
				NativeError))
		{
			OutError = NativeError;
			return false;
		}

		FUeAgentSimpleAuthoringProductionProviderConfig
			ProviderConfig;
		ProviderConfig.FamilyId = TEXT("landscape");
		ProviderConfig.OwnerLeaseId = Config.OwnerLeaseId;
		ProviderConfig.PreStatePlane =
			EUeAgentStatePlane::AuthoringMemory;
		ProviderConfig.AtomicityMode =
			EAtomicityMode::CompensatedSaga;
		ProviderConfig.Projection = Config.Projection;
		ProviderConfig.TargetResolver =
			MakeShared<FLandscapeTargetResolver>(
				WorldResolver);
		ProviderConfig.PreStateReadPort =
			MakeShared<FLandscapeStateReadPort>(
				PreStatePortId,
				EUeAgentStatePlane::AuthoringMemory,
				Catalog,
				Config.ReferenceResolver.ToSharedRef(),
				true);
		ProviderConfig.ChangeTargetProjector =
			MakeShared<FLandscapeChangeTargetProjector>();
		ProviderConfig.DraftBuilder =
			MakeShared<FLandscapeDraftBuilder>();
		ProviderConfig.MutationBindingProjector =
			MakeShared<FLandscapeMutationBindingProjector>();
		ProviderConfig.DurableTargetProjector =
			MakeShared<FLandscapeDurableTargetProjector>();
		ProviderConfig.DurableService = Config.DurableService;
		ProviderConfig.UeHook =
			MakeShared<FLandscapeUeHook>(
				WorldResolver,
				NativeHook.ToSharedRef());
		ProviderConfig.ExpectedStateProjector =
			MakeShared<FLandscapeExpectedStateProjector>();
		const TSharedRef<IUeAgentStateReadPort, ESPMode::ThreadSafe>
			PersistedStateReadPort = MakeShared<FLandscapeStateReadPort>(
				PersistedStatePortId,
				EUeAgentStatePlane::Persisted,
				PersistedCatalog,
				PersistedReferenceResolver,
				false);
		ProviderConfig.PersistedStateOracle = MakeShared<
			FStateReadPortPersistedStateOracle,
			ESPMode::ThreadSafe>(PersistedStateReadPort);
		ProviderConfig.AbsenceOracle = Config.AbsenceOracle;
		ProviderConfig.ReferenceOracle = Config.ReferenceOracle;

		FUeAgentSimpleAuthoringProductionError Error;
		if (!FUeAgentSimpleAuthoringProductionProvider::TryCreate(
				ProviderConfig,
				OutProvider,
				Error))
		{
			OutError = Error;
			return false;
		}
		return true;
	}
}
