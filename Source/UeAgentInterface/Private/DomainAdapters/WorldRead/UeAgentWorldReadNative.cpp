#include "DomainAdapters/WorldRead/UeAgentWorldReadNative.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "Components/PrimitiveComponent.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/Landscape/UeAgentLandscapeHeightState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/LevelStreaming.h"
#include "Engine/EngineTypes.h"
#include "Engine/OverlapResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Landscape.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/ScopeExit.h"
#include "StaticMeshAttributes.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerInstanceWithAsset.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/ErrorHandling/WorldPartitionStreamingGenerationErrorHandler.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstanceViewInterface.h"

namespace UE::UAI::DomainAdapters::WorldRead
{
	using UE::UAI::ContextResource::FUeAgentOperationContext;
	using UE::UAI::Contract::FUeAgentContractValue;
	using UE::UAI::Discovery::FUeAgentCandidateBinding;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = TEXT(""))
        { OutError = MoveTemp(Diagnostic); if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject; return false; }

		FString Number(const double Value)
		{
			return FString::SanitizeFloat(Value, 0);
		}

		bool ParseInput(
			const FUeAgentContractValue& Input,
			TSharedPtr<FJsonObject>& OutObject,
			FUeAgentWorldReadError& OutError)
		{
			OutObject.Reset();
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Input.GetJsonValue());
			if (!FJsonSerializer::Deserialize(Reader, OutObject)
				|| !OutObject.IsValid())
			{
				auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_read_input_invalid"),
					TEXT("Canonical world-read input must be object JSON. ") + Reader->GetErrorMessage(), TEXT("world_read"));
				Diagnostic.SourceKind = TEXT("native_result");
				Diagnostic.PropertyPath = TEXT("/canonical_input");
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object JSON"));
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			return true;
		}

		FString WorldReadJsonKind(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value) return TEXT("<missing>");
			switch (Value->Type)
			{
			case EJson::Null: return TEXT("null");
			case EJson::String: return TEXT("string");
			case EJson::Number: return TEXT("number");
			case EJson::Boolean: return TEXT("boolean");
			case EJson::Array: return TEXT("array");
			case EJson::Object: return TEXT("object");
			default: return TEXT("unknown");
			}
		}

		TSharedPtr<FJsonValue> WorldReadInputValue(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value) return MakeShared<FJsonValueString>(TEXT("<missing>"));
			if (Value->Type == EJson::Array || Value->Type == EJson::Object)
				return MakeShared<FJsonValueString>(WorldReadJsonKind(Value));
			return Value;
		}

		TSharedPtr<FJsonValue> WorldReadVectorValue(const FVector& Value)
		{
			return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
				MakeShared<FJsonValueNumber>(Value.X), MakeShared<FJsonValueNumber>(Value.Y), MakeShared<FJsonValueNumber>(Value.Z)});
		}

		FUeAgentWorldReadError WorldReadFact(const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const bool bInput = false)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("world_read"));
			Error.SourceKind = bInput ? TEXT("command_input") : TEXT("native_result");
			Error.PropertyPath = Path;
			if (bInput) Error.Source.JsonPath = Path;
			Error.ActualValue = MoveTemp(Actual);
			if (!Error.ActualValue) Error.ActualValue = MakeShared<FJsonValueString>(TEXT("<missing>"));
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		void SupplementWorldRead(FUeAgentWorldReadError& Error, const FString& Target, const FString& Field)
		{
			if (Error.IsEmpty()) return;
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = Target;
			if (Error.PropertyPath.IsEmpty()) Error.PropertyPath = Field;
			if (Error.SourceKind.IsEmpty() && Error.Source.SourceFile.IsEmpty()) Error.SourceKind = TEXT("native_result");
			for (auto& Child : Error.Diagnostics) SupplementWorldRead(Child, Target, Field);
			if (Error.PublicCause)
			{
				auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Error.PublicCause);
				SupplementWorldRead(*Cause, Target, Field);
				Error.PublicCause = MoveTemp(Cause);
			}
		}

		bool ReadComponents(const TSharedPtr<FJsonObject>& VectorObject, const FString& Path, FVector& OutVector,
			FUeAgentWorldReadError& OutError, const FString& Code)
		{
			if (!VectorObject.IsValid())
				return Fail(OutError, WorldReadFact(Code, TEXT("Spatial vector must be an object with three components."),
					Path, MakeShared<FJsonValueString>(TEXT("missing object")), MakeShared<FJsonValueString>(TEXT("vector object")), true));
			const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
			if (!VectorObject->TryGetArrayField(TEXT("components"), Components) || !Components)
				return Fail(OutError, WorldReadFact(Code, TEXT("Spatial vector components must be an array."),
					Path + TEXT("/components"), WorldReadInputValue(VectorObject->TryGetField(TEXT("components"))),
					MakeShared<FJsonValueString>(TEXT("array with three components")), true));
			if (Components->Num() != 3)
				return Fail(OutError, WorldReadFact(Code, TEXT("Spatial vector requires exactly three components."),
					Path + TEXT("/components"), MakeShared<FJsonValueNumber>(Components->Num()), MakeShared<FJsonValueNumber>(3), true));
			OutVector = FVector((*Components)[0]->AsNumber(), (*Components)[1]->AsNumber(), (*Components)[2]->AsNumber());
			return true;
		}

		bool ReadVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field,
			FVector& OutVector, FUeAgentWorldReadError& OutError)
		{
			const FString Path = TEXT("/") + FString(Field);
			const TSharedPtr<FJsonObject>* VectorObject = nullptr;
			if (!Object.IsValid() || !Object->TryGetObjectField(Field, VectorObject) || !VectorObject || !VectorObject->IsValid())
				return Fail(OutError, WorldReadFact(TEXT("world_read_spatial_input_invalid"),
					TEXT("Spatial input must be a vector object."), Path,
					WorldReadInputValue(Object.IsValid() ? Object->TryGetField(Field) : nullptr),
					MakeShared<FJsonValueString>(TEXT("object with three components")), true));
			return ReadComponents(*VectorObject, Path, OutVector, OutError, TEXT("world_read_spatial_input_invalid"));
		}

		bool ReadNumberArray3(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FVector& OutVector,
			FUeAgentWorldReadError& OutError, const FString& Code = TEXT("world_read_spatial_input_invalid"))
		{
			const FString Path = TEXT("/") + FString(Field);
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || !Values)
				return Fail(OutError, WorldReadFact(Code, TEXT("Vector extent input must be an array."),
					Path, WorldReadInputValue(Object.IsValid() ? Object->TryGetField(Field) : nullptr),
					MakeShared<FJsonValueString>(TEXT("array with three components")), true));
			if (Values->Num() != 3)
				return Fail(OutError, WorldReadFact(Code, TEXT("Vector extent input requires exactly three components."),
					Path, MakeShared<FJsonValueNumber>(Values->Num()), MakeShared<FJsonValueNumber>(3), true));
			OutVector = FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
			return true;
		}

		bool ReadVectorArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field,
			TArray<FVector>& OutVectors, FUeAgentWorldReadError& OutError)
		{
			OutVectors.Reset();
			const FString Path = TEXT("/") + FString(Field);
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || !Values)
				return Fail(OutError, WorldReadFact(TEXT("world_read_spatial_input_invalid"),
					TEXT("Spatial points must be an array of vector objects."), Path,
					WorldReadInputValue(Object.IsValid() ? Object->TryGetField(Field) : nullptr),
					MakeShared<FJsonValueString>(TEXT("array of vector objects")), true));
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				const auto& Value = (*Values)[Index];
				const TSharedPtr<FJsonObject> Point = Value->AsObject();
				if (!Point.IsValid())
					return Fail(OutError, WorldReadFact(TEXT("world_read_spatial_input_invalid"),
						TEXT("Each spatial point must be a vector object."), Path + TEXT("/") + FString::FromInt(Index),
						WorldReadInputValue(Value), MakeShared<FJsonValueString>(TEXT("vector object")), true));
				FVector Vector;
				FUeAgentWorldReadError WrappedError;
				if (!ReadVector(Point, TEXT("point"), Vector, WrappedError))
				{
					if (!ReadComponents(Point, Path + TEXT("/") + FString::FromInt(Index), Vector, OutError,
						TEXT("world_read_spatial_input_invalid"))) return false;
				}
				OutVectors.Add(Vector);
			}
			return true;
		}

		TArray<FString> ReadStringArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field)
		{
			TArray<FString> Result;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (Object.IsValid()
				&& Object->TryGetArrayField(Field, Values)
				&& Values)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Values)
				{
					Result.Add(Value->AsString());
				}
			}
			return Result;
		}

		ECollisionChannel ReadTraceChannel(
			const TSharedPtr<FJsonObject>& Input)
		{
			FString Channel;
			Input->TryGetStringField(TEXT("trace_channel"), Channel);
			Channel.ToLowerInline();
			if (Channel == TEXT("camera"))
			{
				return ECC_Camera;
			}
			if (Channel == TEXT("pawn"))
			{
				return ECC_Pawn;
			}
			if (Channel == TEXT("world_static"))
			{
				return ECC_WorldStatic;
			}
			if (Channel == TEXT("world_dynamic"))
			{
				return ECC_WorldDynamic;
			}
			return ECC_Visibility;
		}

		FUeAgentTemporalPoint ObservedAt(
			const FUeAgentOperationContext& Context)
		{
			return FUeAgentTemporalPoint(
				TEXT("ue.operation_context:")
					+ Context.GetExecutionContext()
						.GetContextRevision(),
				TEXT("1"));
		}

		FUeAgentStateRevision Revision(
			const FUeAgentOperationContext& Context)
		{
			return FUeAgentStateRevision(
				TEXT("world-read"),
				Context.GetExecutionContext().GetContextRevision(),
				1);
		}

		FUeAgentStateDetails Details(const FString& DisplayName)
		{
			return FUeAgentStateDetails(
				DisplayName,
				TEXT("Fact captured directly from the resolved UE world"),
				{TEXT("WorldRead")},
				false,
				TEXT("available"),
				{});
		}

		bool AddValueEntry(
			const FString& SemanticId,
			const FString& TargetKind,
			const FString& OwnerId,
			const FString& Selector,
			const FString& TypeId,
			EUeAgentValueKind Kind,
			const FUeAgentCanonicalValuePtr& Value,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentWorldReadError& OutError)
		{
			ON_SCOPE_EXIT { SupplementWorldRead(OutError, OwnerId, Selector); };
			if (!Value.IsValid())
			{
				return Fail(OutError, WorldReadFact(TEXT("world_read_value_invalid"),
					TEXT("Captured world-read field has no canonical value."), Selector,
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), OwnerId);
			}
			const FUeAgentStateRevision StateRevision =
				Revision(Context);
			const FUeAgentStateEntry Entry(
				FUeAgentCanonicalIdentity(
					SemanticId,
					TargetKind,
					OwnerId,
					Selector,
					TEXT("state"),
					StateRevision.GetGeneration()),
				FUeAgentCanonicalType(
					TypeId,
					Kind,
					false,
					Kind == EUeAgentValueKind::Object
						? TEXT("object")
						: Kind == EUeAgentValueKind::Array
							? TEXT("array")
							: TEXT("scalar")),
				Value.ToSharedRef(),
				FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::DomainNative,
					TEXT("ue-native"),
					TEXT("world-read"),
					Value.ToSharedRef()),
				Details(Selector),
				StateRevision);
			if (!Entry.IsValid())
			{
				return Fail(OutError, Entry.GetValidationDiagnostic(), OwnerId);
			}
			OutEntries.Add(Entry);
			return true;
		}

		TSharedRef<FJsonObject> WorldPointJson(const FVector& Point)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("frame_id"), TEXT("ue.world"));
			Object->SetStringField(TEXT("unit"), TEXT("centimeter"));
			Object->SetArrayField(
				TEXT("components"),
				{
					MakeShared<FJsonValueNumber>(Point.X),
					MakeShared<FJsonValueNumber>(Point.Y),
					MakeShared<FJsonValueNumber>(Point.Z),
				});
			return Object;
		}

		bool AddJsonValue(
			const FString& SemanticId,
			const FString& TargetKind,
			const FString& OwnerId,
			const FString& Selector,
			const TSharedPtr<FJsonValue>& JsonValue,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentWorldReadError& OutError)
		{
			ON_SCOPE_EXIT { SupplementWorldRead(OutError, OwnerId, Selector); };
			FUeAgentCanonicalValuePtr Value;
			UE::UAI::Contract::FDiagnostic JsonError;
			if (!CanonicalJson::TryFromJsonValue(JsonValue, Context.GetResourceTracker(), Value, JsonError))
				return Fail(OutError, MoveTemp(JsonError), Selector);
			if (Value->GetKind() != EUeAgentValueKind::Array && Value->GetKind() != EUeAgentValueKind::Object)
				return Fail(OutError, WorldReadFact(TEXT("world_read_value_invalid"),
					TEXT("Captured structured field must be an object or array."), Selector,
					WorldReadInputValue(JsonValue), MakeShared<FJsonValueString>(TEXT("object or array"))), OwnerId);
			return AddValueEntry(
				SemanticId,
				TargetKind,
				OwnerId,
				Selector,
				Value->GetKind() == EUeAgentValueKind::Array
					? FString(TEXT("core.array"))
					: FString(TEXT("core.object")),
				Value->GetKind(),
				Value,
				Context,
				OutEntries,
				OutError);
		}

		bool AddString(
			const FString& SemanticId,
			const FString& TargetKind,
			const FString& OwnerId,
			const FString& Selector,
			const FString& Text,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentWorldReadError& OutError)
		{
			ON_SCOPE_EXIT { SupplementWorldRead(OutError, OwnerId, Selector); };
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryString(
					Text,
					Context.GetResourceTracker(),
					Value,
					StateError))
			{
				return Fail(OutError, StateError);
			}
			return AddValueEntry(
				SemanticId,
				TargetKind,
				OwnerId,
				Selector,
				TEXT("core.string"),
				EUeAgentValueKind::String,
				Value,
				Context,
				OutEntries,
				OutError);
		}

		bool AddBoolean(
			const FString& SemanticId,
			const FString& TargetKind,
			const FString& OwnerId,
			const FString& Selector,
			const bool bValue,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentWorldReadError& OutError)
		{
			ON_SCOPE_EXIT { SupplementWorldRead(OutError, OwnerId, Selector); };
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryBoolean(
					bValue,
					Context.GetResourceTracker(),
					Value,
					StateError))
			{
				return Fail(OutError, StateError);
			}
			return AddValueEntry(
				SemanticId,
				TargetKind,
				OwnerId,
				Selector,
				TEXT("core.boolean"),
				EUeAgentValueKind::Boolean,
				Value,
				Context,
				OutEntries,
				OutError);
		}

		bool AddInteger(
			const FString& SemanticId,
			const FString& TargetKind,
			const FString& OwnerId,
			const FString& Selector,
			const int64 NumberValue,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentWorldReadError& OutError)
		{
			ON_SCOPE_EXIT { SupplementWorldRead(OutError, OwnerId, Selector); };
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryInteger(
					LexToString(NumberValue),
					Context.GetResourceTracker(),
					Value,
					StateError))
			{
				return Fail(OutError, StateError);
			}
			return AddValueEntry(
				SemanticId,
				TargetKind,
				OwnerId,
				Selector,
				TEXT("core.integer"),
				EUeAgentValueKind::Integer,
				Value,
				Context,
				OutEntries,
				OutError);
		}

		bool AddDecimal(
			const FString& SemanticId,
			const FString& TargetKind,
			const FString& OwnerId,
			const FString& Selector,
			const double NumberValue,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentWorldReadError& OutError)
		{
			ON_SCOPE_EXIT { SupplementWorldRead(OutError, OwnerId, Selector); };
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryDecimal(
					Number(NumberValue),
					Context.GetResourceTracker(),
					Value,
					StateError))
			{
				return Fail(OutError, StateError);
			}
			return AddValueEntry(
				SemanticId,
				TargetKind,
				OwnerId,
				Selector,
				TEXT("core.decimal"),
				EUeAgentValueKind::Decimal,
				Value,
				Context,
				OutEntries,
				OutError);
		}

		bool AddSpatial(
			const FString& SemanticId,
			const FString& OwnerId,
			const FString& Selector,
			const FString& Subject,
			const FVector& Point,
			const TArray<FString>& ConversionEvidence,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentWorldReadError& OutError)
		{
			ON_SCOPE_EXIT { SupplementWorldRead(OutError, OwnerId, Selector); };
			const FUeAgentTemporalPoint Time = ObservedAt(Context);
			const FUeAgentSpatialFrame Frame(
				TEXT("ue.world"),
				TEXT("ue.world"),
				3,
				{
					{TEXT("1"), TEXT("0"), TEXT("0")},
					{TEXT("0"), TEXT("1"), TEXT("0")},
					{TEXT("0"), TEXT("0"), TEXT("1")},
				},
				{TEXT("0"), TEXT("0"), TEXT("0")},
				TEXT("centimeter"),
				TEXT("UE world coordinate basis"),
				Time);
			const FUeAgentSpatialValue Spatial(
				Subject,
				EUeAgentSpatialQuantityKind::Point,
				Frame,
				{Number(Point.X), Number(Point.Y), Number(Point.Z)},
				Time,
				ConversionEvidence);
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TrySpatial(
					Spatial,
					Context.GetResourceTracker(),
					Value,
					StateError))
			{
				return Fail(OutError, StateError);
			}
			return AddValueEntry(
				SemanticId,
				TEXT("world_spatial"),
				OwnerId,
				Selector,
				TEXT("ue.spatial.point3"),
				EUeAgentValueKind::Spatial,
				Value,
				Context,
				OutEntries,
				OutError);
		}

		bool BuildSnapshot(
			const FString& ProviderBinding,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry> Entries,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentWorldReadError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					FString::Printf(
						TEXT("%s:%s"),
						*ProviderBinding,
						*Context.GetRequestId()),
					Context.GetExecutionContext().GetStatePlane(),
					Revision(Context),
					ObservedAt(Context),
					FUeAgentStateProvenance(
						TEXT("independent_readback"),
						ProviderBinding,
						Context.GetExecutionContext()
							.GetWorldIdentity()),
					Entries,
					Context.GetResourceTracker(),
					OutSnapshot,
					StateError))
			{
				return Fail(OutError, StateError);
			}
			return true;
		}

		class FWorldReadHookBase : public IUeAgentWorldReadHook
		{
		public:
			FWorldReadHookBase(
				FString InProviderBinding,
				TSharedRef<IUeAgentWorldReadEnvironment>
					InEnvironment)
				: ProviderBinding(MoveTemp(InProviderBinding))
				, Environment(MoveTemp(InEnvironment))
			{
			}

		protected:
			bool ResolveWorld(
				const FUeAgentOperationContext& Context,
				UWorld*& OutWorld,
				FUeAgentWorldReadError& OutError) const
			{
				OutWorld = nullptr;
				if (!Environment->TryResolveWorld(
						Context.GetExecutionContext(),
						OutWorld,
						OutError))
				{
					return false;
				}
				if (!OutWorld)
				{
					auto Diagnostic = WorldReadFact(TEXT("world_read_world_unresolved"),
						TEXT("World environment succeeded without a world object."), TEXT("/world"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					Diagnostic.SubjectId = Context.GetExecutionContext().GetWorldIdentity();
					return Fail(OutError, MoveTemp(Diagnostic));
				}
				return true;
			}

			bool ResolveTarget(
				const TOptional<FUeAgentCandidateBinding>& Target,
				UObject*& OutObject,
				FUeAgentWorldReadError& OutError) const
			{
				OutObject = nullptr;
				if (!Target.IsSet())
				{
					return Fail(OutError, WorldReadFact(TEXT("world_read_target_required"),
						TEXT("This world query requires a bound target."), TEXT("/target"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true), ProviderBinding);
				}
				return Environment->TryResolveTarget(
					Target.GetValue(),
					OutObject,
					OutError);
			}

			const FString ProviderBinding;
			const TSharedRef<IUeAgentWorldReadEnvironment>
				Environment;
		};

		class FObjectHook final : public FWorldReadHookBase
		{
		public:
			using FWorldReadHookBase::FWorldReadHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				UWorld* World = nullptr;
				if (!ResolveWorld(Context, World, OutError))
				{
					return false;
				}
				ON_SCOPE_EXIT { SupplementWorldRead(OutError, Target.IsSet() ? Target->GetPublicRef() : World->GetPathName(), FString()); };
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}

				TArray<FUeAgentStateEntry> Entries;
				if (ProviderBinding == TEXT("world.read.summary"))
				{
					const FString OwnerId = World->GetPathName();
					if (!AddString(
							TEXT("world.summary:path"),
							TEXT("world"),
							OwnerId,
							TEXT("path"),
							OwnerId,
							Context,
							Entries,
							OutError)
						|| !AddBoolean(
							TEXT("world.summary:dirty"),
							TEXT("world"),
							OwnerId,
							TEXT("package_dirty"),
							World->GetOutermost()->IsDirty(),
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				else if (ProviderBinding
					== TEXT("world.read.selection"))
				{
					const TArray<FString>& Selection =
						Context.GetExecutionContext()
							.GetSelectionIdentities();
					for (const FString& Identity : Selection)
					{
						if (!AddString(
								TEXT("world.selection:identity"),
								TEXT("object"),
								Identity,
								TEXT("selected_identity"),
								Identity,
								Context,
								Entries,
								OutError))
						{
							return false;
						}
					}
				}
				else if (ProviderBinding
					== TEXT("world.read.actors"))
				{
					const TArray<FString> ClassPaths =
						ReadStringArray(Input, TEXT("class_paths"));
					const TArray<FString> Tags =
						ReadStringArray(Input, TEXT("tags"));
					FString FolderPrefix;
					Input->TryGetStringField(
						TEXT("folder_path_prefix"),
						FolderPrefix);
					for (TActorIterator<AActor> It(World); It; ++It)
					{
						const AActor* Actor = *It;
						if (!ClassPaths.IsEmpty()
							&& !ClassPaths.Contains(
								Actor->GetClass()->GetPathName()))
						{
							continue;
						}
						bool bHasAllTags = true;
						for (const FString& Tag : Tags)
						{
							bHasAllTags &= Actor->ActorHasTag(FName(*Tag));
						}
						if (!bHasAllTags)
						{
							continue;
						}
#if WITH_EDITOR
						if (!FolderPrefix.IsEmpty()
							&& !Actor->GetFolderPath().ToString()
								.StartsWith(FolderPrefix))
						{
							continue;
						}
#endif
						const FString OwnerId = It->GetPathName();
						if (!AddString(
								TEXT("world.actor:class"),
								TEXT("actor"),
								OwnerId,
								TEXT("class_path"),
								It->GetClass()->GetPathName(),
								Context,
								Entries,
								OutError))
						{
							return false;
						}
					}
				}
				else
				{
					UObject* Object = nullptr;
					if (!ResolveTarget(Target, Object, OutError))
					{
						return false;
					}
					ON_SCOPE_EXIT { SupplementWorldRead(OutError, Object->GetPathName(), FString()); };
					if (ProviderBinding
						== TEXT("world.read.actor_components"))
					{
						const AActor* Actor = Cast<AActor>(Object);
						if (!Actor)
						{
							return Fail(OutError, WorldReadFact(TEXT("world_read_target_kind_mismatch"),
								TEXT("Target must be an Actor to query its components."), TEXT("/target_class"),
								MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()), MakeShared<FJsonValueString>(TEXT("Actor"))), Object->GetPathName());
						}
						TInlineComponentArray<UActorComponent*>
							Components(Actor);
						for (const UActorComponent* Component
							: Components)
						{
							if (!Component)
							{
								continue;
							}
							if (!AddString(
									TEXT("world.component:class"),
									TEXT("component"),
									Component->GetPathName(),
									TEXT("class_path"),
									Component->GetClass()->GetPathName(),
									Context,
									Entries,
									OutError))
							{
								return false;
							}
						}
					}
					else
					{
						FString PropertyPath;
						if (!Input->TryGetStringField(
								TEXT("property_path"),
								PropertyPath))
						{
							return Fail(OutError, WorldReadFact(TEXT("world_read_property_path_missing"),
								TEXT("Property query requires a string property_path."), TEXT("/property_path"),
								WorldReadInputValue(Input->TryGetField(TEXT("property_path"))), MakeShared<FJsonValueString>(TEXT("string")), true), Object->GetPathName());
						}
						const FProperty* Property =
							FindFProperty<FProperty>(
								Object->GetClass(),
								*PropertyPath);
						FString Text;
						if (Property)
						{
							Property->ExportText_InContainer(
								0,
								Text,
								Object,
								Object,
								Object,
								PPF_None);
						}
						else if (PropertyPath == TEXT("Bounds"))
						{
							const UPrimitiveComponent* Primitive =
								Cast<UPrimitiveComponent>(Object);
							if (!Primitive)
							{
								return Fail(OutError, WorldReadFact(TEXT("world_read_property_not_found"),
									TEXT("Bounds is available only for a primitive component when it is not a reflected property."), TEXT("/Bounds"),
									MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()), MakeShared<FJsonValueString>(TEXT("PrimitiveComponent"))), Object->GetPathName());
							}
							const FBoxSphereBounds& Bounds = Primitive->Bounds;
							Text = FString::Printf(
								TEXT("origin=(%s,%s,%s);extent=(%s,%s,%s);sphere_radius=%s"),
								*Number(Bounds.Origin.X),
								*Number(Bounds.Origin.Y),
								*Number(Bounds.Origin.Z),
								*Number(Bounds.BoxExtent.X),
								*Number(Bounds.BoxExtent.Y),
								*Number(Bounds.BoxExtent.Z),
								*Number(Bounds.SphereRadius));
						}
						else
						{
							return Fail(OutError, WorldReadFact(TEXT("world_read_property_not_found"),
								TEXT("Target class does not declare the requested property."), TEXT("/property_path"),
								MakeShared<FJsonValueString>(PropertyPath),
								MakeShared<FJsonValueString>(TEXT("property declared on ") + Object->GetClass()->GetPathName()), true), Object->GetPathName());
						}
						if (!AddString(
								TEXT("world.property:value"),
								TEXT("object"),
								Object->GetPathName(),
								PropertyPath,
								Text,
								Context,
								Entries,
								OutError))
						{
							return false;
						}
					}
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}
		};

		class FContentHook final : public FWorldReadHookBase
		{
		public:
			using FWorldReadHookBase::FWorldReadHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				UWorld* World = nullptr;
				if (!ResolveWorld(Context, World, OutError))
				{
					return false;
				}
				ON_SCOPE_EXIT { SupplementWorldRead(OutError, Target.IsSet() ? Target->GetPublicRef() : World->GetPathName(), FString()); };
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				bool bIncludeComponents = false;
				Input->TryGetBoolField(
					TEXT("include_components"),
					bIncludeComponents);

				TArray<FUeAgentStateEntry> Entries;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					const AActor* Actor = *It;
					if (!AddString(
							TEXT("world.content:actor"),
							TEXT("actor"),
							Actor->GetPathName(),
							TEXT("class_path"),
							Actor->GetClass()->GetPathName(),
							Context,
							Entries,
							OutError))
					{
						return false;
					}
					if (!bIncludeComponents)
					{
						continue;
					}
					TInlineComponentArray<UActorComponent*>
						Components(Actor);
					for (const UActorComponent* Component : Components)
					{
						if (Component
							&& !AddString(
								TEXT("world.content:component"),
								TEXT("component"),
								Component->GetPathName(),
								TEXT("class_path"),
								Component->GetClass()->GetPathName(),
								Context,
								Entries,
								OutError))
						{
							return false;
						}
					}
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}
		};

		class FSpatialHook final : public FWorldReadHookBase
		{
		public:
			using FWorldReadHookBase::FWorldReadHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				UWorld* World = nullptr;
				if (!ResolveWorld(Context, World, OutError))
				{
					return false;
				}
				ON_SCOPE_EXIT { SupplementWorldRead(OutError, Target.IsSet() ? Target->GetPublicRef() : World->GetPathName(), FString()); };
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}

				TArray<FUeAgentStateEntry> Entries;
				if (ProviderBinding == TEXT("world.read.ray_trace"))
				{
					FVector Start;
					FVector Direction;
					double Distance = 0.0;
					if (!ReadVector(Input, TEXT("start"), Start, OutError) || !ReadVector(Input, TEXT("direction"), Direction, OutError)) return false;
					if (!Input->TryGetNumberField(TEXT("distance_cm"), Distance))
						return Fail(OutError, WorldReadFact(TEXT("world_read_ray_input_invalid"), TEXT("Parameter 'distance_cm' must be a number."), TEXT("/distance_cm"), WorldReadInputValue(Input->TryGetField(TEXT("distance_cm"))), MakeShared<FJsonValueString>(TEXT("number")), true));
					if (Direction.IsNearlyZero())
						return Fail(OutError, WorldReadFact(TEXT("world_read_ray_input_invalid"), TEXT("Ray direction must not be nearly zero."), TEXT("/direction/components"), WorldReadVectorValue(Direction), MakeShared<FJsonValueString>(TEXT("non-nearly-zero vector")), true));
					if (Distance <= 0.0)
						return Fail(OutError, WorldReadFact(TEXT("world_read_ray_input_invalid"), TEXT("Ray distance must be greater than zero."), TEXT("/distance_cm"), MakeShared<FJsonValueNumber>(Distance), MakeShared<FJsonValueString>(TEXT("greater than zero")), true));
					FHitResult Hit;
					FCollisionQueryParams Params(
						SCENE_QUERY_STAT(UAIWorldReadRay),
						false);
					bool bTraceComplex = false;
					Input->TryGetBoolField(
						TEXT("trace_complex"),
						bTraceComplex);
					Params.bTraceComplex = bTraceComplex;
					const bool bHit = World->LineTraceSingleByChannel(
							Hit,
							Start,
							Start + Direction.GetSafeNormal()
								* Distance,
							ReadTraceChannel(Input),
							Params);
					if (!AddBoolean(
							TEXT("world.ray_trace:hit"),
							TEXT("world"),
							World->GetPathName(),
							TEXT("hit"),
							bHit,
							Context,
							Entries,
							OutError))
					{
						return false;
					}
					if (!bHit)
					{
						return BuildSnapshot(
							ProviderBinding,
							Context,
							MoveTemp(Entries),
							OutSnapshot,
							OutError);
					}
					const FString OwnerId = Hit.GetComponent()
						? Hit.GetComponent()->GetPathName()
						: World->GetPathName();
					if (!AddString(
							TEXT("world.ray_trace:target"),
							TEXT("world"),
							World->GetPathName(),
							TEXT("target_ref"),
							OwnerId,
							Context,
							Entries,
							OutError)
						|| !AddSpatial(
							TEXT("world.ray_trace:impact_point"),
							World->GetPathName(),
							TEXT("impact_point"),
							OwnerId,
							Hit.ImpactPoint,
							{},
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				else if (ProviderBinding == TEXT("world.read.overlap"))
				{
					FVector Center;
					if (!ReadVector(
							Input,
							TEXT("center"),
							Center,
							OutError))
					{
						return false;
					}
					FString ShapeName;
					Input->TryGetStringField(TEXT("shape"), ShapeName);
					FCollisionShape Shape;
					if (ShapeName == TEXT("box"))
					{
						FVector Extents;
						if (!ReadNumberArray3(Input, TEXT("half_extents_cm"), Extents, OutError, TEXT("world_read_overlap_shape_invalid"))) return false;
						Shape = FCollisionShape::MakeBox(Extents);
					}
					else
					{
						double Radius = 0.0;
						Input->TryGetNumberField(TEXT("radius_cm"), Radius);
						if (Radius <= 0.0)
						{
						return Fail(OutError, WorldReadFact(TEXT("world_read_overlap_shape_invalid"), TEXT("Overlap radius must be greater than zero."), TEXT("/radius_cm"), WorldReadInputValue(Input->TryGetField(TEXT("radius_cm"))), MakeShared<FJsonValueString>(TEXT("number greater than zero")), true));
						}
						if (ShapeName == TEXT("capsule"))
						{
							double HalfHeight = 0.0;
							Input->TryGetNumberField(
								TEXT("half_height_cm"),
								HalfHeight);
							Shape = FCollisionShape::MakeCapsule(
								Radius,
								HalfHeight);
						}
						else
						{
							Shape = FCollisionShape::MakeSphere(Radius);
						}
					}
					FCollisionQueryParams Params(
						SCENE_QUERY_STAT(UAIWorldReadOverlap),
						false);
					TArray<FOverlapResult> Results;
					World->OverlapMultiByChannel(
						Results,
						Center,
						FQuat::Identity,
						ReadTraceChannel(Input),
						Shape,
						Params);
					TArray<FString> Targets;
					for (const FOverlapResult& Result : Results)
					{
						const UPrimitiveComponent* Component =
							Result.Component.Get();
						if (Component)
						{
							Targets.AddUnique(Component->GetPathName());
						}
					}
					Targets.Sort();
					bool bIncludeTargets = false;
					Input->TryGetBoolField(TEXT("include_targets"), bIncludeTargets);
					if (bIncludeTargets)
					{
						TArray<TSharedPtr<FJsonValue>> TargetValues;
						TargetValues.Reserve(Targets.Num());
						for (const FString& TargetRef : Targets)
						{
							TargetValues.Add(
								MakeShared<FJsonValueString>(TargetRef));
						}
						if (!AddJsonValue(
							TEXT("world.overlap:targets"),
							TEXT("world"),
							World->GetPathName(),
							TEXT("targets"),
							MakeShared<FJsonValueArray>(MoveTemp(TargetValues)),
							Context,
							Entries,
							OutError))
						{
							return false;
						}
					}
					if (!AddInteger(
							TEXT("world.overlap:count"),
							TEXT("world"),
							World->GetPathName(),
							TEXT("overlap_count"),
							Targets.Num(),
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				else if (ProviderBinding
					== TEXT("world.read.capsule_sweep"))
				{
					FVector Start;
					FVector End;
					double Radius = 0.0;
					double HalfHeight = 0.0;
					if (!ReadVector(Input, TEXT("start"), Start, OutError) || !ReadVector(Input, TEXT("end"), End, OutError)) return false;
					if (!Input->TryGetNumberField(TEXT("radius_cm"), Radius))
						return Fail(OutError, WorldReadFact(TEXT("world_read_capsule_sweep_input_invalid"), TEXT("Parameter 'radius_cm' must be a number."), TEXT("/radius_cm"), WorldReadInputValue(Input->TryGetField(TEXT("radius_cm"))), MakeShared<FJsonValueString>(TEXT("number")), true));
					if (!Input->TryGetNumberField(TEXT("half_height_cm"), HalfHeight))
						return Fail(OutError, WorldReadFact(TEXT("world_read_capsule_sweep_input_invalid"), TEXT("Parameter 'half_height_cm' must be a number."), TEXT("/half_height_cm"), WorldReadInputValue(Input->TryGetField(TEXT("half_height_cm"))), MakeShared<FJsonValueString>(TEXT("number")), true));
					if (Radius <= 0.0)
						return Fail(OutError, WorldReadFact(TEXT("world_read_capsule_sweep_input_invalid"), TEXT("Capsule radius must be greater than zero."), TEXT("/radius_cm"), MakeShared<FJsonValueNumber>(Radius), MakeShared<FJsonValueString>(TEXT("greater than zero")), true));
					if (HalfHeight < Radius)
						return Fail(OutError, WorldReadFact(TEXT("world_read_capsule_sweep_input_invalid"), TEXT("Capsule half-height must be at least its radius."), TEXT("/half_height_cm"), MakeShared<FJsonValueNumber>(HalfHeight), MakeShared<FJsonValueNumber>(Radius), true));
					FCollisionQueryParams Params(
						SCENE_QUERY_STAT(UAIWorldReadCapsuleSweep),
						false);
					bool bTraceComplex = false;
					bool bFindInitialOverlaps = false;
					Input->TryGetBoolField(TEXT("trace_complex"), bTraceComplex);
					Input->TryGetBoolField(
						TEXT("find_initial_overlaps"),
						bFindInitialOverlaps);
					Params.bTraceComplex = bTraceComplex;
					Params.bFindInitialOverlaps = bFindInitialOverlaps;
					TArray<FHitResult> Hits;
					World->SweepMultiByChannel(
						Hits,
						Start,
						End,
						FQuat::Identity,
						ReadTraceChannel(Input),
						FCollisionShape::MakeCapsule(Radius, HalfHeight),
						Params);
					bool bIncludeHits = false;
					Input->TryGetBoolField(TEXT("include_hits"), bIncludeHits);
					if (bIncludeHits)
					{
						TArray<TSharedPtr<FJsonValue>> HitValues;
						HitValues.Reserve(Hits.Num());
						for (int32 Index = 0; Index < Hits.Num(); ++Index)
						{
							const FHitResult& Hit = Hits[Index];
							const FString OwnerId = Hit.GetComponent()
								? Hit.GetComponent()->GetPathName()
								: World->GetPathName();
							TSharedRef<FJsonObject> HitObject =
								MakeShared<FJsonObject>();
							HitObject->SetStringField(TEXT("target_ref"), OwnerId);
							HitObject->SetObjectField(
								TEXT("impact_point"),
								WorldPointJson(Hit.ImpactPoint));
							HitValues.Add(MakeShared<FJsonValueObject>(HitObject));
						}
						if (!AddJsonValue(
							TEXT("world.capsule_sweep:hits"),
							TEXT("world"),
							World->GetPathName(),
							TEXT("hits"),
							MakeShared<FJsonValueArray>(MoveTemp(HitValues)),
							Context,
							Entries,
							OutError))
						{
							return false;
						}
					}
					if (!AddInteger(
							TEXT("world.capsule_sweep:hit_count"),
							TEXT("world"),
							World->GetPathName(),
							TEXT("hit_count"),
							Hits.Num(),
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				else if (ProviderBinding
					== TEXT("world.read.capsule_path"))
				{
					TArray<FVector> Points;
					double Radius = 0.0;
					double HalfHeight = 0.0;
					double Step = 0.0;
					if (!ReadVectorArray(Input, TEXT("points"), Points, OutError)) return false;
					if (Points.Num() < 2)
						return Fail(OutError, WorldReadFact(TEXT("world_read_capsule_path_input_invalid"), TEXT("Capsule path requires at least two points."), TEXT("/points"), MakeShared<FJsonValueNumber>(Points.Num()), MakeShared<FJsonValueString>(TEXT("at least two points")), true));
					if (!Input->TryGetNumberField(TEXT("radius_cm"), Radius))
						return Fail(OutError, WorldReadFact(TEXT("world_read_capsule_path_input_invalid"), TEXT("Parameter 'radius_cm' must be a number."), TEXT("/radius_cm"), WorldReadInputValue(Input->TryGetField(TEXT("radius_cm"))), MakeShared<FJsonValueString>(TEXT("number")), true));
					if (!Input->TryGetNumberField(TEXT("half_height_cm"), HalfHeight))
						return Fail(OutError, WorldReadFact(TEXT("world_read_capsule_path_input_invalid"), TEXT("Parameter 'half_height_cm' must be a number."), TEXT("/half_height_cm"), WorldReadInputValue(Input->TryGetField(TEXT("half_height_cm"))), MakeShared<FJsonValueString>(TEXT("number")), true));
					if (!Input->TryGetNumberField(TEXT("step_cm"), Step))
						return Fail(OutError, WorldReadFact(TEXT("world_read_capsule_path_input_invalid"), TEXT("Parameter 'step_cm' must be a number."), TEXT("/step_cm"), WorldReadInputValue(Input->TryGetField(TEXT("step_cm"))), MakeShared<FJsonValueString>(TEXT("number")), true));
					if (Radius <= 0.0)
						return Fail(OutError, WorldReadFact(TEXT("world_read_capsule_path_input_invalid"), TEXT("Capsule radius must be greater than zero."), TEXT("/radius_cm"), MakeShared<FJsonValueNumber>(Radius), MakeShared<FJsonValueString>(TEXT("greater than zero")), true));
					if (HalfHeight < Radius)
						return Fail(OutError, WorldReadFact(TEXT("world_read_capsule_path_input_invalid"), TEXT("Capsule half-height must be at least its radius."), TEXT("/half_height_cm"), MakeShared<FJsonValueNumber>(HalfHeight), MakeShared<FJsonValueNumber>(Radius), true));
					if (Step <= 0.0)
						return Fail(OutError, WorldReadFact(TEXT("world_read_capsule_path_input_invalid"), TEXT("Capsule path step must be greater than zero."), TEXT("/step_cm"), MakeShared<FJsonValueNumber>(Step), MakeShared<FJsonValueString>(TEXT("greater than zero")), true));
					int32 BlockedSegments = 0;
					for (int32 Segment = 0; Segment + 1 < Points.Num(); ++Segment)
					{
						const FVector Delta = Points[Segment + 1] - Points[Segment];
						const int32 Steps = FMath::Max(
							1,
							FMath::CeilToInt(Delta.Length() / Step));
						for (int32 Index = 0; Index < Steps; ++Index)
						{
							const FVector Start = Points[Segment]
								+ Delta * (static_cast<double>(Index) / Steps);
							const FVector End = Points[Segment]
								+ Delta * (static_cast<double>(Index + 1) / Steps);
							FHitResult Hit;
							FCollisionQueryParams Params(
								SCENE_QUERY_STAT(UAIWorldReadCapsulePath),
								false);
							if (World->SweepSingleByChannel(
									Hit,
									Start,
									End,
									FQuat::Identity,
									ReadTraceChannel(Input),
									FCollisionShape::MakeCapsule(Radius, HalfHeight),
									Params))
							{
								++BlockedSegments;
								break;
							}
						}
					}
					if (!AddInteger(
							TEXT("world.capsule_path:blocked_segments"),
							TEXT("world"),
							World->GetPathName(),
							TEXT("blocked_segments"),
							BlockedSegments,
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				else
				{
					FVector Center;
					if (!ReadVector(Input, TEXT("center"), Center, OutError))
					{
						return false;
					}
					FVector HalfExtents;
					double Radius = 0.0;
					FUeAgentWorldReadError ExtentError;
					const bool bHasExtents = ReadNumberArray3(Input, TEXT("half_extents_cm"), HalfExtents, ExtentError);
					Input->TryGetNumberField(TEXT("radius_cm"), Radius);
					if (!bHasExtents && Radius <= 0.0)
					{
						auto Diagnostic = WorldReadFact(TEXT("world_read_nearby_shape_extent_missing"), TEXT("Nearby query requires three half-extents or a positive radius."), TEXT("/radius_cm"), WorldReadInputValue(Input->TryGetField(TEXT("radius_cm"))), MakeShared<FJsonValueString>(TEXT("positive radius or three half-extents")), true);
						Diagnostic.Diagnostics.Add(MoveTemp(ExtentError));
						return Fail(OutError, MoveTemp(Diagnostic));
					}
					FString FolderPrefix;
					FString IgnoreFolderPrefix;
					Input->TryGetStringField(TEXT("folder_path_prefix"), FolderPrefix);
					Input->TryGetStringField(
						TEXT("ignore_folder_path_prefix"),
						IgnoreFolderPrefix);
					struct FNearbyShape final
					{
						FString TargetRef;
						FVector BoundsCenter;
					};
					bool bIncludeShapes = false;
					Input->TryGetBoolField(TEXT("include_shapes"), bIncludeShapes);
					int32 NearbyShapeCount = 0;
					TArray<FNearbyShape> NearbyShapes;
					for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
					{
						const UPrimitiveComponent* Component = *It;
						if (!IsValid(Component)
							|| Component->GetWorld() != World
							|| Component->IsEditorOnly()
							|| Component->IsVisualizationComponent()
							|| Component->GetCollisionEnabled()
								== ECollisionEnabled::NoCollision)
						{
							continue;
						}
						const AActor* Actor = Component->GetOwner();
						if (!Actor
							|| Actor->IsEditorOnly()
							|| Actor->GetClass()->HasAnyClassFlags(CLASS_Transient))
						{
							continue;
						}
#if WITH_EDITOR
						const FString Folder = Actor
							? Actor->GetFolderPath().ToString()
							: FString();
						if ((!FolderPrefix.IsEmpty() && !Folder.StartsWith(FolderPrefix))
							|| (!IgnoreFolderPrefix.IsEmpty()
								&& Folder.StartsWith(IgnoreFolderPrefix)))
						{
							continue;
						}
#endif
						const FVector Delta = Component->Bounds.Origin - Center;
						const bool bNearby = bHasExtents
							? FMath::Abs(Delta.X) <= HalfExtents.X + Component->Bounds.BoxExtent.X
								&& FMath::Abs(Delta.Y) <= HalfExtents.Y + Component->Bounds.BoxExtent.Y
								&& FMath::Abs(Delta.Z) <= HalfExtents.Z + Component->Bounds.BoxExtent.Z
							: Delta.Length() <= Radius + Component->Bounds.SphereRadius;
						if (!bNearby)
						{
							continue;
						}
						++NearbyShapeCount;
						if (bIncludeShapes)
						{
							NearbyShapes.Add(
								{Component->GetPathName(), Component->Bounds.Origin});
						}
					}
					if (bIncludeShapes)
					{
						NearbyShapes.Sort(
						[](const FNearbyShape& Left, const FNearbyShape& Right)
						{
							return Left.TargetRef < Right.TargetRef;
						});
						TArray<TSharedPtr<FJsonValue>> ShapeValues;
						ShapeValues.Reserve(NearbyShapes.Num());
						for (const FNearbyShape& NearbyShape : NearbyShapes)
						{
							TSharedRef<FJsonObject> ShapeObject =
								MakeShared<FJsonObject>();
							ShapeObject->SetStringField(
								TEXT("target_ref"),
								NearbyShape.TargetRef);
							ShapeObject->SetObjectField(
								TEXT("bounds_center"),
								WorldPointJson(NearbyShape.BoundsCenter));
							ShapeValues.Add(
								MakeShared<FJsonValueObject>(ShapeObject));
						}
						if (!AddJsonValue(
							TEXT("world.nearby_shapes:shapes"),
							TEXT("world"),
							World->GetPathName(),
							TEXT("shapes"),
							MakeShared<FJsonValueArray>(MoveTemp(ShapeValues)),
							Context,
							Entries,
							OutError))
						{
							return false;
						}
					}
					if (!AddInteger(
							TEXT("world.nearby_shapes:count"),
							TEXT("world"),
							World->GetPathName(),
							TEXT("nearby_shape_count"),
							NearbyShapeCount,
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}
		};

		class FMeshHook final : public FWorldReadHookBase
		{
		public:
			using FWorldReadHookBase::FWorldReadHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				UWorld* World = nullptr;
				if (!ResolveWorld(Context, World, OutError))
				{
					return false;
				}
				ON_SCOPE_EXIT { SupplementWorldRead(OutError, Target.IsSet() ? Target->GetPublicRef() : World->GetPathName(), FString()); };
				UObject* Object = nullptr;
				if (!ResolveTarget(Target, Object, OutError))
				{
					return false;
				}
				ON_SCOPE_EXIT { SupplementWorldRead(OutError, Object->GetPathName(), FString()); };
				UPrimitiveComponent* Component = Cast<UPrimitiveComponent>(Object);
				TArray<FVector> Positions;
				FString TargetKind;
				bool bMeshAvailable = false;
				FUeAgentWorldReadError MeshError = WorldReadFact(TEXT("world_read_mesh_unavailable"),
					TEXT("Target must be a static or dynamic mesh component with readable vertex data."), TEXT("/target_class"),
					MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()),
					MakeShared<FJsonValueString>(TEXT("static or dynamic mesh component")));
				if (const UStaticMeshComponent* StaticComponent = Cast<UStaticMeshComponent>(Component))
				{
					const UStaticMesh* Mesh = StaticComponent->GetStaticMesh();
					const FMeshDescription* Description = Mesh ? Mesh->GetMeshDescription(0) : nullptr;
					if (!Description)
					{
						MeshError = WorldReadFact(TEXT("world_read_mesh_unavailable"),
							Mesh ? TEXT("Static mesh has no readable LOD 0 mesh description.") : TEXT("Static mesh component has no assigned mesh."),
							Mesh ? TEXT("/mesh_description/0") : TEXT("/static_mesh"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					}
					if (Description)
					{
						const FStaticMeshConstAttributes Attributes(*Description);
						const TVertexAttributesConstRef<FVector3f> Vertices = Attributes.GetVertexPositions();
						TArray<FVertexID> VertexIds;
						for (const FVertexID VertexId : Description->Vertices().GetElementIDs()) VertexIds.Add(VertexId);
						VertexIds.Sort([](const FVertexID Left, const FVertexID Right) { return Left.GetValue() < Right.GetValue(); });
						for (const FVertexID VertexId : VertexIds) Positions.Add(FVector(Vertices[VertexId]));
						bMeshAvailable = true;
						TargetKind = TEXT("static_mesh_component");
					}
				}
				else if (UDynamicMeshComponent* DynamicComponent = Cast<UDynamicMeshComponent>(Component))
				{
					if (UDynamicMesh* Mesh = DynamicComponent->GetDynamicMesh())
					{
						Mesh->ProcessMesh([&Positions](const UE::Geometry::FDynamicMesh3& Geometry)
						{
							for (const int32 VertexId : Geometry.VertexIndicesItr()) Positions.Add(FVector(Geometry.GetVertex(VertexId)));
						});
						bMeshAvailable = true;
						TargetKind = TEXT("dynamic_mesh_component");
					}
					else
					{
						MeshError = WorldReadFact(TEXT("world_read_mesh_unavailable"),
							TEXT("Dynamic mesh component has no readable dynamic mesh."), TEXT("/dynamic_mesh"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					}
				}
				if (!bMeshAvailable)
				{
					return Fail(OutError, MoveTemp(MeshError), Object->GetPathName());
				}

				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				if (Positions.IsEmpty())
				{
					return Fail(OutError, WorldReadFact(TEXT("world_read_mesh_has_no_vertices"),
						TEXT("Mesh contains no vertices to query."), TEXT("/vertices"),
						MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one vertex"))), Component->GetPathName());
				}

				int32 ChosenIndex = 0;
				if (ProviderBinding
					== TEXT("world.read.mesh_vertex_position"))
				{
					double RequestedIndex = 0.0;
					if (!Input->TryGetNumberField(TEXT("vertex_index"), RequestedIndex))
					{
						return Fail(OutError, WorldReadFact(TEXT("world_read_mesh_vertex_out_of_range"),
							TEXT("Mesh vertex index must be a number."), TEXT("/vertex_index"),
							WorldReadInputValue(Input->TryGetField(TEXT("vertex_index"))),
							MakeShared<FJsonValueString>(TEXT("number")), true), Component->GetPathName());
					}
					if (RequestedIndex < 0.0 || RequestedIndex >= static_cast<double>(Positions.Num()))
					{
						return Fail(OutError, WorldReadFact(TEXT("world_read_mesh_vertex_out_of_range"),
							TEXT("Mesh vertex index is outside the available vertex range."), TEXT("/vertex_index"),
							MakeShared<FJsonValueNumber>(RequestedIndex),
							MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= index < %d"), Positions.Num())), true), Component->GetPathName());
					}
					ChosenIndex = static_cast<int32>(RequestedIndex);
				}
				else
				{
					FVector WorldPoint;
					if (!ReadVector(
							Input,
							TEXT("world_point"),
							WorldPoint,
							OutError))
					{
						return false;
					}
					double BestDistanceSquared =
						TNumericLimits<double>::Max();
					for (int32 Index = 0;
						Index < Positions.Num();
						++Index)
					{
						const FVector& Local = Positions[Index];
						const double DistanceSquared =
							FVector::DistSquared(
								Component->GetComponentTransform()
									.TransformPosition(Local),
								WorldPoint);
						if (DistanceSquared < BestDistanceSquared)
						{
							BestDistanceSquared = DistanceSquared;
							ChosenIndex = Index;
						}
					}
				}

				const FVector& Local = Positions[ChosenIndex];
				const FVector WorldPosition =
					Component->GetComponentTransform()
						.TransformPosition(Local);
				TArray<FUeAgentStateEntry> Entries;
				if (!AddSpatial(
						TEXT("world.mesh:vertex_position"),
						Component->GetPathName(),
						TEXT("vertex_position"),
						Component->GetPathName(),
						WorldPosition,
						{
							TEXT("mesh.local"),
							TEXT("component.transform"),
							TEXT("ue.world"),
						},
						Context,
						Entries,
						OutError)
					|| !AddInteger(
						TEXT("world.mesh:vertex_index"),
						TargetKind,
						Component->GetPathName(),
						TEXT("vertex_index"),
						ChosenIndex,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}
		};

		class FNavigationHook final : public FWorldReadHookBase
		{
		public:
			using FWorldReadHookBase::FWorldReadHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				UWorld* World = nullptr;
				if (!ResolveWorld(Context, World, OutError))
				{
					return false;
				}
				ON_SCOPE_EXIT { SupplementWorldRead(OutError, Target.IsSet() ? Target->GetPublicRef() : World->GetPathName(), FString()); };
				UNavigationSystemV1* Navigation =
					FNavigationSystem::GetCurrent<UNavigationSystemV1>(
						World);
				if (!Navigation)
				{
					return Fail(OutError, WorldReadFact(TEXT("world_read_navigation_unavailable"),
						TEXT("World has no navigation system."), TEXT("/navigation_system"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), World->GetPathName());
				}
				if (!Navigation->GetDefaultNavDataInstance(FNavigationSystem::DontCreate))
				{
					return Fail(OutError, WorldReadFact(TEXT("world_read_navigation_unavailable"),
						TEXT("World navigation system has no default navigation data."), TEXT("/navigation_data"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), World->GetPathName());
				}

				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				FVector QueryExtent = INVALID_NAVEXTENT;
				FUeAgentWorldReadError QueryExtentError;
				const bool bHasQueryExtent = ReadNumberArray3(Input, TEXT("query_extent_cm"), QueryExtent, QueryExtentError);
				auto TryProjectPoint = [&](const FVector& Point,
					FVector& OutPoint)
				{
					FNavLocation Projected;
					const bool bProjected = Navigation->ProjectPointToNavigation(
						Point,
						Projected,
						bHasQueryExtent ? QueryExtent : INVALID_NAVEXTENT);
					OutPoint = bProjected ? Projected.Location : Point;
					return bProjected;
				};
				auto ProjectionFailure = [&](const FVector& Point, const FString& Path)
				{
					const auto Actual = MakeShared<FJsonObject>();
					Actual->SetField(TEXT("point_cm"), WorldReadVectorValue(Point));
					if (bHasQueryExtent) Actual->SetField(TEXT("query_extent_cm"), WorldReadVectorValue(QueryExtent));
					return WorldReadFact(TEXT("world_read_navigation_projection_failed"),
						TEXT("Navigation system could not project the point onto navigation data within the query extent."), Path,
						MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("point with a navigation projection")));
				};
				TArray<FUeAgentStateEntry> Entries;
				if (ProviderBinding
					== TEXT("world.read.nav_project_point"))
				{
					FVector Point;
					if (!ReadVector(Input, TEXT("point"), Point, OutError))
					{
						return false;
					}
					FVector Projected;
					if (!TryProjectPoint(Point, Projected))
					{
						return Fail(OutError, ProjectionFailure(Point, TEXT("/point")), World->GetPathName());
					}
					if (!AddSpatial(
							TEXT("world.navigation:projected_point"),
							World->GetPathName(),
							TEXT("projected_point"),
							ProviderBinding,
							Projected,
							{},
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				else
				{
					TArray<FVector> Points;
					TArray<TPair<int32, int32>> Pairs;
					const bool bFindPath = ProviderBinding
						== TEXT("world.read.nav_find_path");
					if (bFindPath)
					{
						FVector Start;
						FVector End;
						if (!ReadVector(Input, TEXT("start"), Start, OutError)
							|| !ReadVector(Input, TEXT("end"), End, OutError))
						{
							return false;
						}
						Points = {Start, End};
						Pairs.Add({0, 1});
					}
					else
					{
						if (!ReadVectorArray(Input, TEXT("points"), Points, OutError)) return false;
						if (Points.Num() < 2)
						{
							return Fail(OutError, WorldReadFact(TEXT("world_read_navigation_points_invalid"),
								TEXT("Navigation connectivity requires at least two points."), TEXT("/points"),
								MakeShared<FJsonValueNumber>(Points.Num()), MakeShared<FJsonValueString>(TEXT("at least two points")), true));
						}
						const TArray<TSharedPtr<FJsonValue>>* PairValues = nullptr;
						if (Input->TryGetArrayField(TEXT("pairs"), PairValues)
							&& PairValues)
						{
							for (const TSharedPtr<FJsonValue>& PairValue : *PairValues)
							{
								const TArray<TSharedPtr<FJsonValue>>* Indices = nullptr;
								if (!PairValue->TryGetArray(Indices)
									|| !Indices
									|| Indices->Num() != 2)
								{
								return Fail(OutError, WorldReadFact(TEXT("world_read_navigation_pairs_invalid"),
									TEXT("Each navigation pair must contain exactly two point indices."), TEXT("/pairs/") + FString::FromInt(Pairs.Num()),
									Indices ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Indices->Num())) : WorldReadInputValue(PairValue),
									MakeShared<FJsonValueString>(TEXT("array with two point indices")), true));
								}
								Pairs.Add({
									static_cast<int32>((*Indices)[0]->AsNumber()),
									static_cast<int32>((*Indices)[1]->AsNumber()),
								});
							}
						}
						else
						{
							for (int32 Index = 0; Index + 1 < Points.Num(); ++Index)
							{
								Pairs.Add({Index, Index + 1});
							}
						}
					}
					bool bProjectToNav = false;
					Input->TryGetBoolField(
						TEXT("project_to_nav"), bProjectToNav);
					bool bAllowProjectionFailure = false;
					Input->TryGetBoolField(
						TEXT("allow_projection_failure"),
						bAllowProjectionFailure);
					if (!bProjectToNav
						&& (bHasQueryExtent || bAllowProjectionFailure))
					{
						return Fail(OutError, WorldReadFact(TEXT("world_read_navigation_projection_policy_invalid"),
							TEXT("query_extent_cm or allow_projection_failure requires project_to_nav=true."), TEXT("/project_to_nav"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true));
					}
					if (bProjectToNav)
					{
						for (int32 PointIndex = 0;
							PointIndex < Points.Num(); ++PointIndex)
						{
							FVector ProjectedPoint;
							const bool bProjected = TryProjectPoint(
								Points[PointIndex], ProjectedPoint);
							if (!bProjected && !bAllowProjectionFailure)
							{
								return Fail(OutError, ProjectionFailure(Points[PointIndex], TEXT("/points/") + FString::FromInt(PointIndex)), World->GetPathName());
							}
							Points[PointIndex] = ProjectedPoint;
							if (!AddBoolean(
									FString::Printf(
										TEXT("world.navigation:projection:%d"),
										PointIndex),
									TEXT("world"),
									World->GetPathName(),
									FString::Printf(
										TEXT("point_%d_projection_succeeded"),
										PointIndex),
									bProjected,
									Context,
									Entries,
									OutError))
							{
								return false;
							}
						}
					}
					bool bAllowPartial = false;
					Input->TryGetBoolField(TEXT("allow_partial"), bAllowPartial);
					bool bIncludePathPoints = false;
					Input->TryGetBoolField(
						TEXT("include_path_points"),
						bIncludePathPoints);
					TArray<TSharedPtr<FJsonValue>> PairResults;
					PairResults.Reserve(Pairs.Num());
					for (int32 PairIndex = 0; PairIndex < Pairs.Num(); ++PairIndex)
					{
						const TPair<int32, int32>& Pair = Pairs[PairIndex];
						if (!Points.IsValidIndex(Pair.Key)
							|| !Points.IsValidIndex(Pair.Value))
						{
							return Fail(OutError, WorldReadFact(TEXT("world_read_navigation_pair_out_of_range"),
								TEXT("Navigation pair references a point index outside the supplied point array."), TEXT("/pairs/") + FString::FromInt(PairIndex),
								MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(Pair.Key), MakeShared<FJsonValueNumber>(Pair.Value)}),
								MakeShared<FJsonValueString>(FString::Printf(TEXT("both indices: 0 <= index < %d"), Points.Num())), true));
						}
						UNavigationPath* Path =
							UNavigationSystemV1::FindPathToLocationSynchronously(
								World,
								Points[Pair.Key],
								Points[Pair.Value]);
						const bool bReachable = IsValid(Path)
							&& Path->IsValid()
							&& (bAllowPartial || !Path->IsPartial());
						TArray<TSharedPtr<FJsonValue>> PathPointValues;
						if (bIncludePathPoints && IsValid(Path))
						{
							PathPointValues.Reserve(Path->PathPoints.Num());
							for (int32 PointIndex = 0;
								PointIndex < Path->PathPoints.Num();
								++PointIndex)
							{
								PathPointValues.Add(
									MakeShared<FJsonValueObject>(
										WorldPointJson(Path->PathPoints[PointIndex])));
							}
						}
						if (bFindPath)
						{
							if (!AddBoolean(
									TEXT("world.navigation:path:reachable"),
									TEXT("world"),
									World->GetPathName(),
									TEXT("reachable"),
									bReachable,
									Context,
									Entries,
									OutError)
								|| (bIncludePathPoints
									&& !AddJsonValue(
										TEXT("world.navigation:path:points"),
										TEXT("world"),
										World->GetPathName(),
										TEXT("path_points"),
										MakeShared<FJsonValueArray>(
											MoveTemp(PathPointValues)),
										Context,
										Entries,
										OutError)))
							{
								return false;
							}
						}
						else
						{
							TSharedRef<FJsonObject> PairObject =
								MakeShared<FJsonObject>();
							PairObject->SetNumberField(
								TEXT("from_index"),
								Pair.Key);
							PairObject->SetNumberField(
								TEXT("to_index"),
								Pair.Value);
							PairObject->SetBoolField(
								TEXT("reachable"),
								bReachable);
							if (bIncludePathPoints)
							{
								PairObject->SetArrayField(
									TEXT("path_points"),
									MoveTemp(PathPointValues));
							}
							PairResults.Add(
								MakeShared<FJsonValueObject>(PairObject));
						}
					}
					if (!bFindPath
						&& !AddJsonValue(
							TEXT("world.navigation:connectivity:pairs"),
							TEXT("world"),
							World->GetPathName(),
							TEXT("pairs"),
							MakeShared<FJsonValueArray>(MoveTemp(PairResults)),
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}
		};

		class FTopologyHook final : public FWorldReadHookBase
		{
		public:
			using FWorldReadHookBase::FWorldReadHookBase;

			class FPartitionIssueCollector final
				: public IStreamingGenerationErrorHandler
			{
			public:
				struct FIssue final
				{
					FString Code;
					FString Subject;
					FString Detail;
				};

				void Add(FString Code, FString Subject, FString Detail)
				{
					Issues.Add({MoveTemp(Code), MoveTemp(Subject), MoveTemp(Detail)});
				}

				FString ActorName(
					const IWorldPartitionActorDescInstanceView& Actor) const
				{
					return IStreamingGenerationErrorHandler::GetFullActorName(Actor);
				}

				void OnInvalidRuntimeGrid(
					const IWorldPartitionActorDescInstanceView& Actor,
					FName Grid) override
				{
					Add(TEXT("invalid_runtime_grid"), ActorName(Actor), Grid.ToString());
				}

				void OnInvalidReference(
					const IWorldPartitionActorDescInstanceView& Actor,
					const FGuid& ReferenceGuid,
					IWorldPartitionActorDescInstanceView*) override
				{
					Add(TEXT("invalid_reference"), ActorName(Actor),
						ReferenceGuid.ToString(EGuidFormats::DigitsWithHyphens));
				}

				void OnInvalidReferenceGridPlacement(
					const IWorldPartitionActorDescInstanceView& Actor,
					const IWorldPartitionActorDescInstanceView& Reference) override
				{
					Add(TEXT("invalid_reference_grid_placement"),
						ActorName(Actor), ActorName(Reference));
				}

				void OnInvalidReferenceDataLayers(
					const IWorldPartitionActorDescInstanceView& Actor,
					const IWorldPartitionActorDescInstanceView& Reference,
					EDataLayerInvalidReason) override
				{
					Add(TEXT("invalid_reference_data_layers"),
						ActorName(Actor), ActorName(Reference));
				}

				void OnInvalidReferenceRuntimeGrid(
					const IWorldPartitionActorDescInstanceView& Actor,
					const IWorldPartitionActorDescInstanceView& Reference) override
				{
					Add(TEXT("invalid_reference_runtime_grid"),
						ActorName(Actor), ActorName(Reference));
				}

				void OnDataLayersLoadFilterMismatch(
					const IWorldPartitionActorDescInstanceView& Actor) override
				{
					Add(TEXT("data_layers_load_filter_mismatch"), ActorName(Actor), {});
				}

				void OnInvalidWorldReference(
					const IWorldPartitionActorDescInstanceView& Actor,
					EWorldReferenceInvalidReason) override
				{
					Add(TEXT("invalid_world_reference"), ActorName(Actor), {});
				}

				void OnInvalidReferenceDataLayerAsset(
					const UDataLayerInstanceWithAsset* Instance) override
				{
					Add(TEXT("invalid_reference_data_layer_asset"),
						Instance ? Instance->GetPathName() : FString(), {});
				}

				void OnDataLayerHierarchyTypeMismatch(
					const UDataLayerInstance* Instance,
					const UDataLayerInstance*,
					EDataLayerHierarchyInvalidReason) override
				{
					Add(TEXT("data_layer_hierarchy_type_mismatch"),
						Instance ? Instance->GetPathName() : FString(), {});
				}

				void OnInvalidWorldDataLayersReference(
					const AWorldDataLayers* WorldDataLayers,
					const UDataLayerInstance* Instance,
					const FText& Reason) override
				{
					Add(TEXT("invalid_world_data_layers_reference"),
						WorldDataLayers ? WorldDataLayers->GetPathName() : FString(),
						FString::Printf(TEXT("%s %s"),
							Instance ? *Instance->GetPathName() : TEXT(""),
							*Reason.ToString()));
				}

				void OnDataLayerAssetConflict(
					const UDataLayerInstanceWithAsset* Instance,
					const UDataLayerInstanceWithAsset* Conflicting) override
				{
					Add(TEXT("data_layer_asset_conflict"),
						Instance ? Instance->GetPathName() : FString(),
						Conflicting ? Conflicting->GetPathName() : FString());
				}

				void OnInvalidDataLayerAssetType(
					const UDataLayerInstanceWithAsset* Instance,
					const UDataLayerAsset* Asset) override
				{
					Add(TEXT("invalid_data_layer_asset_type"),
						Instance ? Instance->GetPathName() : FString(),
						Asset ? Asset->GetPathName() : FString());
				}

				void OnActorNeedsResave(
					const IWorldPartitionActorDescInstanceView& Actor) override
				{
					Add(TEXT("actor_needs_resave"), ActorName(Actor), {});
				}

				void OnLevelInstanceInvalidWorldAsset(
					const IWorldPartitionActorDescInstanceView& Actor,
					FName WorldAsset,
					ELevelInstanceInvalidReason) override
				{
					Add(TEXT("level_instance_invalid_world_asset"),
						ActorName(Actor), WorldAsset.ToString());
				}

				void OnInvalidActorFilterReference(
					const IWorldPartitionActorDescInstanceView& Actor,
					const IWorldPartitionActorDescInstanceView& Reference) override
				{
					Add(TEXT("invalid_actor_filter_reference"),
						ActorName(Actor), ActorName(Reference));
				}

				void OnInvalidHLODLayer(
					const IWorldPartitionActorDescInstanceView& Actor) override
				{
					Add(TEXT("invalid_hlod_layer"), ActorName(Actor), {});
				}

				TArray<FIssue> Issues;
			};

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				UWorld* World = nullptr;
				if (!ResolveWorld(Context, World, OutError))
				{
					return false;
				}
				ON_SCOPE_EXIT { SupplementWorldRead(OutError, Target.IsSet() ? Target->GetPublicRef() : World->GetPathName(), FString()); };
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				TArray<FUeAgentStateEntry> Entries;
				if (ProviderBinding
					== TEXT("world.read.hlod_assignments"))
				{
					FString FolderPrefix;
					FString ManagedTag;
					Input->TryGetStringField(
						TEXT("folder_path_prefix"),
						FolderPrefix);
					Input->TryGetStringField(TEXT("managed_tag"), ManagedTag);
					for (TActorIterator<AActor> It(World); It; ++It)
					{
						const AActor* Actor = *It;
#if WITH_EDITOR
						if ((!FolderPrefix.IsEmpty()
								&& !Actor->GetFolderPath().ToString().StartsWith(FolderPrefix))
							|| (!ManagedTag.IsEmpty()
								&& !Actor->ActorHasTag(FName(*ManagedTag))))
						{
							continue;
						}
						const UObject* Layer = Actor->GetHLODLayer();
						if (!Layer)
						{
							continue;
						}
						const FString LayerPath = Layer->GetPathName();
						if (!AddString(
								TEXT("world.hlod:layer:") + Actor->GetPathName(),
								TEXT("actor"),
								Actor->GetPathName(),
								TEXT("hlod_layer"),
								LayerPath,
								Context,
								Entries,
								OutError))
						{
							return false;
						}
#endif
					}
					return BuildSnapshot(
						ProviderBinding,
						Context,
						MoveTemp(Entries),
						OutSnapshot,
						OutError);
				}
				if (ProviderBinding == TEXT("world.read.partition")
					|| ProviderBinding == TEXT("world.read.partition_health"))
				{
					UWorldPartition* Partition = World->GetWorldPartition();
					const bool bPartitionEnabled = Partition != nullptr;
					if (!AddBoolean(
							TEXT("world.partition:partition_enabled"),
							TEXT("world"), World->GetPathName(),
							TEXT("partition_enabled"), bPartitionEnabled,
							Context, Entries, OutError))
					{
						return false;
					}

					if (ProviderBinding == TEXT("world.read.partition_health"))
					{
						FPartitionIssueCollector Collector;
						if (Partition)
						{
							bool bEnableStreaming =
								Partition->IsStreamingEnabledInEditor();
							Input->TryGetBoolField(
								TEXT("enable_streaming"), bEnableStreaming);
							UWorldPartition::FCheckForErrorsParams Params;
							Params.SetErrorHandler(&Collector);
							Params.SetActorDescContainerInstanceCollection(Partition);
							Params.SetEnableStreaming(bEnableStreaming);
							UWorldPartition::CheckForErrors(Params);
						}
						if (!AddString(
								TEXT("world.partition_health:health_status"),
								TEXT("world"), World->GetPathName(),
								TEXT("health_status"),
								!Partition ? TEXT("not_enabled")
									: (Collector.Issues.IsEmpty()
										? TEXT("valid") : TEXT("invalid")),
								Context, Entries, OutError)
							|| !AddInteger(
								TEXT("world.partition_health:error_count"),
								TEXT("world"), World->GetPathName(),
								TEXT("error_count"), Collector.Issues.Num(),
								Context, Entries, OutError))
						{
							return false;
						}
						TArray<TSharedPtr<FJsonValue>> IssueValues;
						IssueValues.Reserve(Collector.Issues.Num());
						for (int32 Index = 0; Index < Collector.Issues.Num(); ++Index)
						{
							const FPartitionIssueCollector::FIssue& Issue =
								Collector.Issues[Index];
							TSharedRef<FJsonObject> IssueObject =
								MakeShared<FJsonObject>();
							IssueObject->SetStringField(TEXT("code"), Issue.Code);
							if (!Issue.Subject.IsEmpty())
							{
								IssueObject->SetStringField(
									TEXT("subject"),
									Issue.Subject);
							}
							IssueValues.Add(
								MakeShared<FJsonValueObject>(IssueObject));
						}
						if (!AddJsonValue(
								TEXT("world.partition_health:issues"),
								TEXT("world"),
								World->GetPathName(),
								TEXT("issues"),
								MakeShared<FJsonValueArray>(MoveTemp(IssueValues)),
								Context,
								Entries,
								OutError))
						{
							return false;
						}
						return BuildSnapshot(
							ProviderBinding, Context, MoveTemp(Entries),
							OutSnapshot, OutError);
					}

					const bool bStreamingEnabled = Partition
						&& Partition->IsStreamingEnabledInEditor();
					const int32 RegionCount = Partition
						? Partition->GetUserLoadedEditorRegions().Num() : 0;
					if (!AddBoolean(
							TEXT("world.partition:streaming_enabled"),
							TEXT("world"), World->GetPathName(),
							TEXT("streaming_enabled"), bStreamingEnabled,
							Context, Entries, OutError)
						|| !AddInteger(
							TEXT("world.partition:user_loaded_region_count"),
							TEXT("world"), World->GetPathName(),
							TEXT("user_loaded_region_count"), RegionCount,
							Context, Entries, OutError))
					{
						return false;
					}
					bool bIncludeLoadedRegions = false;
					Input->TryGetBoolField(
						TEXT("include_loaded_regions"),
						bIncludeLoadedRegions);
					if (Partition)
					{
						if (!AddString(
								TEXT("world.partition:editor_name"),
								TEXT("world_partition"), Partition->GetPathName(),
								TEXT("editor_name"),
								Partition->GetWorldPartitionEditorName().ToString(),
								Context, Entries, OutError))
						{
							return false;
						}
					}
					if (bIncludeLoadedRegions)
					{
						TArray<TSharedPtr<FJsonValue>> RegionValues;
						if (Partition)
						{
							RegionValues.Reserve(
								Partition->GetUserLoadedEditorRegions().Num());
							for (const FBox& Region
								: Partition->GetUserLoadedEditorRegions())
							{
								TSharedRef<FJsonObject> RegionObject =
									MakeShared<FJsonObject>();
								RegionObject->SetObjectField(
									TEXT("center"),
									WorldPointJson(Region.GetCenter()));
								RegionObject->SetObjectField(
									TEXT("extent"),
									WorldPointJson(Region.GetExtent()));
								RegionValues.Add(
									MakeShared<FJsonValueObject>(RegionObject));
							}
						}
						if (!AddJsonValue(
								TEXT("world.partition:user_regions"),
								TEXT("world"),
								World->GetPathName(),
								TEXT("loaded_regions"),
								MakeShared<FJsonValueArray>(MoveTemp(RegionValues)),
								Context,
								Entries,
								OutError))
						{
							return false;
						}
					}
					return BuildSnapshot(
						ProviderBinding, Context, MoveTemp(Entries),
						OutSnapshot, OutError);
				}
				if (ProviderBinding == TEXT("world.read.data_layers"))
				{
					FString Scope = TEXT("world");
					Input->TryGetStringField(TEXT("scope"), Scope);
					if (Scope != TEXT("world") && Scope != TEXT("selection"))
					{
						return Fail(OutError, WorldReadFact(TEXT("world_read_data_layer_scope_invalid"),
							TEXT("Data-layer scope must be world or selection."), TEXT("/scope"), MakeShared<FJsonValueString>(Scope),
							MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueString>(TEXT("world")), MakeShared<FJsonValueString>(TEXT("selection"))}), true));
					}
					TArray<AActor*> ActorsInScope;
					const TArray<FString>& SelectionIdentities =
						Context.GetExecutionContext().GetSelectionIdentities();
					for (TActorIterator<AActor> It(World); It; ++It)
					{
						AActor* Actor = *It;
						if (Scope == TEXT("world")
							|| SelectionIdentities.Contains(Actor->GetPathName()))
						{
							ActorsInScope.Add(Actor);
						}
					}

					TArray<const UDataLayerInstance*> Layers;
					if (Scope == TEXT("selection"))
					{
						TSet<const UDataLayerInstance*> UniqueLayers;
						for (const AActor* Actor : ActorsInScope)
						{
							UniqueLayers.Append(Actor->GetDataLayerInstances());
						}
						Layers = UniqueLayers.Array();
					}
					else if (const AWorldDataLayers* WorldDataLayers =
						World->GetWorldDataLayers())
					{
						WorldDataLayers->ForEachDataLayerInstance(
							[&Layers](UDataLayerInstance* Layer)
							{
								if (Layer)
								{
									Layers.Add(Layer);
								}
								return true;
							});
					}
					Layers.Sort(
						[](const UDataLayerInstance& Left,
							const UDataLayerInstance& Right)
						{
							return Left.GetDataLayerFullName()
								< Right.GetDataLayerFullName();
						});
					if (!AddInteger(
							TEXT("world.data_layers:count"),
							TEXT("world"),
							World->GetPathName(),
							TEXT("data_layer_count"),
							Layers.Num(),
							Context,
							Entries,
							OutError))
					{
						return false;
					}
					bool bIncludeMembership = false;
					Input->TryGetBoolField(
						TEXT("include_actor_membership"),
						bIncludeMembership);
					for (const UDataLayerInstance* Layer : Layers)
					{
						if (!Layer)
						{
							continue;
						}
						const FString Owner = Layer->GetPathName();
						const UDataLayerAsset* Asset = Layer->GetAsset();
						if (!AddString(
								TEXT("world.data_layers:short_name:") + Owner,
								TEXT("data_layer"), Owner,
								TEXT("data_layer_short_name"),
								Layer->GetDataLayerShortName(),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("world.data_layers:full_name:") + Owner,
								TEXT("data_layer"), Owner,
								TEXT("data_layer_full_name"),
								Layer->GetDataLayerFullName(),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("world.data_layers:asset:") + Owner,
								TEXT("data_layer"), Owner,
								TEXT("data_layer_asset_path"),
								Asset ? Asset->GetPathName() : FString(),
								Context, Entries, OutError)
							|| !AddBoolean(
								TEXT("world.data_layers:runtime:") + Owner,
								TEXT("data_layer"), Owner,
								TEXT("data_layer_runtime"), Layer->IsRuntime(),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("world.data_layers:runtime_state:") + Owner,
								TEXT("data_layer"), Owner,
								TEXT("data_layer_runtime_state"),
								LexToString(static_cast<int32>(Layer->GetRuntimeState())),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("world.data_layers:effective_runtime_state:") + Owner,
								TEXT("data_layer"), Owner,
								TEXT("data_layer_effective_runtime_state"),
								LexToString(static_cast<int32>(
									Layer->GetEffectiveRuntimeState())),
								Context, Entries, OutError))
						{
							return false;
						}
#if WITH_EDITOR
						if (!AddBoolean(
								TEXT("world.data_layers:loaded:") + Owner,
								TEXT("data_layer"), Owner,
								TEXT("data_layer_loaded_in_editor"),
								Layer->IsLoadedInEditor(),
								Context, Entries, OutError)
							|| !AddBoolean(
								TEXT("world.data_layers:effective_loaded:") + Owner,
								TEXT("data_layer"), Owner,
								TEXT("data_layer_effective_loaded_in_editor"),
								Layer->IsEffectiveLoadedInEditor(),
								Context, Entries, OutError)
							|| !AddBoolean(
								TEXT("world.data_layers:visible:") + Owner,
								TEXT("data_layer"), Owner,
								TEXT("data_layer_visible"), Layer->IsVisible(),
								Context, Entries, OutError)
							|| !AddBoolean(
								TEXT("world.data_layers:effective_visible:") + Owner,
								TEXT("data_layer"), Owner,
								TEXT("data_layer_effective_visible"),
								Layer->IsEffectiveVisible(),
								Context, Entries, OutError))
						{
							return false;
						}
#endif
						if (bIncludeMembership)
						{
							TArray<TSharedPtr<FJsonValue>> Members;
							for (const AActor* Actor : ActorsInScope)
							{
								if (!Actor->ContainsDataLayer(Layer))
								{
									continue;
								}
								Members.Add(
									MakeShared<FJsonValueString>(Actor->GetPathName()));
							}
							if (!AddJsonValue(
									TEXT("world.data_layers:actors:") + Owner,
									TEXT("data_layer"), Owner,
									TEXT("data_layer_actor_members"),
									MakeShared<FJsonValueArray>(Members),
									Context, Entries, OutError)
								|| !AddInteger(
									TEXT("world.data_layers:membership_count:") + Owner,
									TEXT("data_layer"), Owner,
									TEXT("data_layer_actor_membership_count"),
									Members.Num(),
									Context, Entries, OutError))
							{
								return false;
							}
						}
					}
					return BuildSnapshot(
						ProviderBinding, Context, MoveTemp(Entries),
						OutSnapshot, OutError);
				}
				const TArray<ULevelStreaming*>& StreamingLevels =
					World->GetStreamingLevels();
				const TArray<FString> RequestedPackages =
					ReadStringArray(Input, TEXT("level_packages"));
				int32 MatchingLevelCount = 0;
				for (const ULevelStreaming* Level : StreamingLevels)
				{
					const FString PackageName = Level
						? Level->GetWorldAssetPackageName()
						: FString();
					if (Level
						&& (RequestedPackages.IsEmpty()
							|| RequestedPackages.Contains(PackageName)))
					{
						++MatchingLevelCount;
						if (!AddString(
								TEXT("world.topology:streaming_level"),
								TEXT("streaming_level"),
								Level->GetPathName(),
								TEXT("package_name"),
								PackageName,
								Context,
								Entries,
								OutError))
						{
							return false;
						}
					}
				}
				if (ProviderBinding
					!= TEXT("world.read.streaming_levels"))
				{
					if (!AddInteger(
							TEXT("world.topology:streaming_level_count"),
							TEXT("world"),
							World->GetPathName(),
							TEXT("streaming_level_count"),
							MatchingLevelCount,
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}
		};

		class FPhysicsHook final : public FWorldReadHookBase
		{
		public:
			using FWorldReadHookBase::FWorldReadHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				UWorld* World = nullptr;
				if (!ResolveWorld(Context, World, OutError))
				{
					return false;
				}
				ON_SCOPE_EXIT { SupplementWorldRead(OutError, Target.IsSet() ? Target->GetPublicRef() : World->GetPathName(), FString()); };
				UObject* Object = nullptr;
				if (!ResolveTarget(Target, Object, OutError))
				{
					return false;
				}
				ON_SCOPE_EXIT { SupplementWorldRead(OutError, Object->GetPathName(), FString()); };
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				TArray<FUeAgentStateEntry> Entries;
				if (ProviderBinding == TEXT("world.read.physics_constraint"))
				{
					UPhysicsConstraintComponent* Constraint =
						Cast<UPhysicsConstraintComponent>(Object);
					if (!Constraint)
					{
						return Fail(OutError, WorldReadFact(TEXT("world_read_target_kind_mismatch"),
							TEXT("Target must be a physics constraint component."), TEXT("/target_class"),
							MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()), MakeShared<FJsonValueString>(TEXT("PhysicsConstraintComponent"))), Object->GetPathName());
					}
					const FString Owner = Constraint->GetPathName();
					if (!AddString(
							TEXT("world.physics_constraint:path"),
							TEXT("physics_constraint_component"), Owner,
							TEXT("constraint_path"), Owner,
							Context, Entries, OutError))
					{
						return false;
					}
					bool bIncludeEndpoints = false;
					bool bIncludeLimits = false;
					bool bIncludeRuntimeState = false;
					Input->TryGetBoolField(
						TEXT("include_endpoints"), bIncludeEndpoints);
					Input->TryGetBoolField(
						TEXT("include_limits"), bIncludeLimits);
					Input->TryGetBoolField(
						TEXT("include_runtime_state"), bIncludeRuntimeState);
					if (bIncludeEndpoints)
					{
						UPrimitiveComponent* Component1 = nullptr;
						UPrimitiveComponent* Component2 = nullptr;
						FName Bone1;
						FName Bone2;
						Constraint->GetConstrainedComponents(
							Component1, Bone1, Component2, Bone2);
						if (!AddString(
								TEXT("world.physics_constraint:component_1"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("constraint_component_1"),
								Component1 ? Component1->GetPathName() : FString(),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("world.physics_constraint:bone_1"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("constraint_bone_1"), Bone1.ToString(),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("world.physics_constraint:component_2"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("constraint_component_2"),
								Component2 ? Component2->GetPathName() : FString(),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("world.physics_constraint:bone_2"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("constraint_bone_2"), Bone2.ToString(),
								Context, Entries, OutError))
						{
							return false;
						}
					}
					const FConstraintInstance& Instance =
						Constraint->ConstraintInstance;
					if (bIncludeLimits
						&& (!AddDecimal(
								TEXT("world.physics_constraint:linear_limit"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("linear_limit_cm"), Instance.GetLinearLimit(),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("world.physics_constraint:linear_x_motion"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("linear_x_motion"),
								LexToString(static_cast<int32>(Instance.GetLinearXMotion())),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("world.physics_constraint:linear_y_motion"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("linear_y_motion"),
								LexToString(static_cast<int32>(Instance.GetLinearYMotion())),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("world.physics_constraint:linear_z_motion"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("linear_z_motion"),
								LexToString(static_cast<int32>(Instance.GetLinearZMotion())),
								Context, Entries, OutError)
							|| !AddDecimal(
								TEXT("world.physics_constraint:swing_1_limit"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("angular_swing_1_limit_degrees"),
								Instance.GetAngularSwing1Limit(),
								Context, Entries, OutError)
							|| !AddDecimal(
								TEXT("world.physics_constraint:swing_2_limit"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("angular_swing_2_limit_degrees"),
								Instance.GetAngularSwing2Limit(),
								Context, Entries, OutError)
							|| !AddDecimal(
								TEXT("world.physics_constraint:twist_limit"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("angular_twist_limit_degrees"),
								Instance.GetAngularTwistLimit(),
								Context, Entries, OutError)))
					{
						return false;
					}
					if (bIncludeRuntimeState
						&& (!AddBoolean(
								TEXT("world.physics_constraint:broken"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("constraint_broken"), Constraint->IsBroken(),
								Context, Entries, OutError)
							|| !AddDecimal(
								TEXT("world.physics_constraint:current_twist"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("current_twist_degrees"),
								Constraint->GetCurrentTwist(),
								Context, Entries, OutError)
							|| !AddDecimal(
								TEXT("world.physics_constraint:current_swing_1"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("current_swing_1_degrees"),
								Constraint->GetCurrentSwing1(),
								Context, Entries, OutError)
							|| !AddDecimal(
								TEXT("world.physics_constraint:current_swing_2"),
								TEXT("physics_constraint_component"), Owner,
								TEXT("current_swing_2_degrees"),
								Constraint->GetCurrentSwing2(),
								Context, Entries, OutError)))
					{
						return false;
					}
				}
				else
				{
					const UPrimitiveComponent* Primitive =
						Cast<UPrimitiveComponent>(Object);
					if (!Primitive)
					{
						return Fail(OutError, WorldReadFact(TEXT("world_read_target_kind_mismatch"),
							TEXT("Target must be a primitive component to query physics state."), TEXT("/target_class"),
							MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()), MakeShared<FJsonValueString>(TEXT("PrimitiveComponent"))), Object->GetPathName());
					}
					const FString Owner = Primitive->GetPathName();
					if (!AddString(
							TEXT("world.physics:collision_enabled"),
							TEXT("primitive_component"), Owner,
							TEXT("collision_enabled"),
							LexToString(static_cast<int32>(
								Primitive->GetCollisionEnabled())),
							Context, Entries, OutError))
					{
						return false;
					}
					bool bIncludeCollisionResponses = false;
					bool bIncludeMaterial = false;
					bool bIncludeRuntimeState = false;
					Input->TryGetBoolField(
						TEXT("include_collision_responses"),
						bIncludeCollisionResponses);
					Input->TryGetBoolField(
						TEXT("include_material"), bIncludeMaterial);
					Input->TryGetBoolField(
						TEXT("include_runtime_state"), bIncludeRuntimeState);
					if (bIncludeCollisionResponses)
					{
						const auto& Responses = Primitive->GetCollisionResponseToChannels();
						for (int32 Channel = 0; Channel < UE_ARRAY_COUNT(Responses.EnumArray); ++Channel)
						{
							if (!AddString(
									FString::Printf(
										TEXT("world.physics:response:%d"), Channel),
									TEXT("primitive_component"), Owner,
									FString::Printf(
										TEXT("collision_response_%d"), Channel),
									LexToString(static_cast<int32>(
										Primitive->GetCollisionResponseToChannel(
											static_cast<ECollisionChannel>(Channel)))),
									Context, Entries, OutError))
							{
								return false;
							}
						}
					}
					if (bIncludeMaterial)
					{
						const UMaterialInterface* Material =
							Primitive->GetMaterial(0);
						if (!AddString(
								TEXT("world.physics:material"),
								TEXT("primitive_component"), Owner,
								TEXT("material_path"),
								Material ? Material->GetPathName() : FString(),
								Context, Entries, OutError))
						{
							return false;
						}
					}
					if (bIncludeRuntimeState
						&& (!AddBoolean(
								TEXT("world.physics:gravity_enabled"),
								TEXT("primitive_component"), Owner,
								TEXT("gravity_enabled"),
								Primitive->IsGravityEnabled(),
								Context, Entries, OutError)
							|| !AddBoolean(
								TEXT("world.physics:simulating"),
								TEXT("primitive_component"), Owner,
								TEXT("simulating_physics"),
								Primitive->IsSimulatingPhysics(),
								Context, Entries, OutError)))
					{
						return false;
					}
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}
		};

		class FLandscapeHeightHook final : public FWorldReadHookBase
		{
		public:
			using FWorldReadHookBase::FWorldReadHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				UWorld* World = nullptr;
				if (!ResolveWorld(Context, World, OutError))
				{
					return false;
				}
				ON_SCOPE_EXIT { SupplementWorldRead(OutError, Target.IsSet() ? Target->GetPublicRef() : World->GetPathName(), FString()); };
				if (!Target.IsSet())
				{
					return Fail(OutError, WorldReadFact(TEXT("landscape_height_target_required"),
						TEXT("Landscape height query requires a target."), TEXT("/target"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("landscape target")), true), ProviderBinding);
				}

				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				const TSharedPtr<FJsonObject>* Region = nullptr;
				const TSharedPtr<FJsonObject>* SpaceContract = nullptr;
				if (!Input->TryGetObjectField(TEXT("region"), Region) || !Region || !Region->IsValid())
				{
					return Fail(OutError, WorldReadFact(TEXT("landscape_height_space_contract_invalid"),
						TEXT("Landscape region must be an object."), TEXT("/region"), WorldReadInputValue(Input->TryGetField(TEXT("region"))),
						MakeShared<FJsonValueString>(TEXT("object")), true), Target->GetPublicRef());
				}
				if (!Input->TryGetObjectField(TEXT("space_contract"), SpaceContract) || !SpaceContract || !SpaceContract->IsValid())
				{
					return Fail(OutError, WorldReadFact(TEXT("landscape_height_space_contract_invalid"),
						TEXT("Landscape space contract must be an object."), TEXT("/space_contract"), WorldReadInputValue(Input->TryGetField(TEXT("space_contract"))),
						MakeShared<FJsonValueString>(TEXT("object")), true), Target->GetPublicRef());
				}
				FString RegionFrame;
				FString SpaceFrame;
				FString Unit;
				FString BasisEvidence;
				(*Region)->TryGetStringField(
					TEXT("frame_id"),
					RegionFrame);
				(*SpaceContract)->TryGetStringField(
					TEXT("frame_id"),
					SpaceFrame);
				(*SpaceContract)->TryGetStringField(
					TEXT("unit"),
					Unit);
				(*SpaceContract)->TryGetStringField(
					TEXT("basis_evidence"),
					BasisEvidence);
				if (RegionFrame != TEXT("ue.landscape.local_sample_grid"))
				{
					return Fail(OutError, WorldReadFact(TEXT("landscape_height_space_contract_invalid"),
						TEXT("Landscape region must use the local sample-grid frame."), TEXT("/region/frame_id"), WorldReadInputValue((*Region)->TryGetField(TEXT("frame_id"))),
						MakeShared<FJsonValueString>(TEXT("ue.landscape.local_sample_grid")), true), Target->GetPublicRef());
				}
				if (SpaceFrame != TEXT("ue.landscape.local_sample_grid"))
				{
					return Fail(OutError, WorldReadFact(TEXT("landscape_height_space_contract_invalid"),
						TEXT("Landscape space contract must use the local sample-grid frame."), TEXT("/space_contract/frame_id"), WorldReadInputValue((*SpaceContract)->TryGetField(TEXT("frame_id"))),
						MakeShared<FJsonValueString>(TEXT("ue.landscape.local_sample_grid")), true), Target->GetPublicRef());
				}
				if (Unit != TEXT("sample"))
				{
					return Fail(OutError, WorldReadFact(TEXT("landscape_height_space_contract_invalid"),
						TEXT("Landscape height region coordinates must use sample units."), TEXT("/space_contract/unit"), WorldReadInputValue((*SpaceContract)->TryGetField(TEXT("unit"))),
						MakeShared<FJsonValueString>(TEXT("sample")), true), Target->GetPublicRef());
				}
				if (BasisEvidence.TrimStartAndEnd().IsEmpty())
				{
					return Fail(OutError, WorldReadFact(TEXT("landscape_height_space_contract_invalid"),
						TEXT("Landscape space contract requires non-empty basis evidence."), TEXT("/space_contract/basis_evidence"), WorldReadInputValue((*SpaceContract)->TryGetField(TEXT("basis_evidence"))),
						MakeShared<FJsonValueString>(TEXT("non-empty string")), true), Target->GetPublicRef());
				}

				FIntPoint MinInclusive;
				FIntPoint MaxInclusive;
				if (!TryReadIntegerPoint(*Region, TEXT("min_inclusive"), MinInclusive, OutError)
					|| !TryReadIntegerPoint(*Region, TEXT("max_inclusive"), MaxInclusive, OutError))
				{
					return false;
				}

				ALandscape* Landscape = nullptr;
				DomainAdapters::Landscape::
					FLandscapeHeightStateError StateError;
				if (!DomainAdapters::Landscape::TryResolveLandscapeHeightTarget(
						*World, Target->GetPublicRef(), nullptr, Landscape, StateError))
				{
					return Fail(OutError, StateError);
				}
				ON_SCOPE_EXIT { SupplementWorldRead(OutError, Landscape->GetPathName(), FString()); };

				DomainAdapters::Landscape::FLandscapeHeightState State;
				if (!DomainAdapters::Landscape::
					TryReadLandscapeHeightState(
						*Landscape,
						MinInclusive,
						MaxInclusive,
						State,
						StateError))
				{
					return Fail(OutError, StateError);
				}

				FUeAgentCanonicalValuePtr Value;
				UE::UAI::Contract::FDiagnostic CanonicalError;
				if (!CanonicalJson::TryFromJsonValue(
						MakeShared<FJsonValueObject>(
							DomainAdapters::Landscape::
								BuildLandscapeHeightQueryObject(
									State)),
						Context.GetResourceTracker(),
						Value,
						CanonicalError))
				{
					return Fail(OutError, CanonicalError, Target->GetPublicRef());
				}
				TArray<FUeAgentStateEntry> Entries;
				if (!AddValueEntry(
						TEXT("landscape.height.read:")
							+ State.ActorPath,
						TEXT("landscape_height_state"),
						State.ActorPath,
						TEXT("height_state"),
						TEXT("ue.landscape.height_state"),
						EUeAgentValueKind::Object,
						Value,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}

		private:
			static bool TryReadIntegerPoint(
				const TSharedPtr<FJsonObject>& Object,
				const TCHAR* Field,
				FIntPoint& OutPoint,
				FUeAgentWorldReadError& OutError)
			{
				const FString Path = TEXT("/region/") + FString(Field);
				const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
				if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || !Values)
				{
					return Fail(OutError, WorldReadFact(TEXT("landscape_height_region_invalid"),
						TEXT("Landscape region corner must be an array of two integer coordinates."), Path,
						WorldReadInputValue(Object.IsValid() ? Object->TryGetField(Field) : nullptr),
						MakeShared<FJsonValueString>(TEXT("array with two integer coordinates")), true));
				}
				if (Values->Num() != 2)
				{
					return Fail(OutError, WorldReadFact(TEXT("landscape_height_region_invalid"),
						TEXT("Landscape region corner requires exactly two coordinates."), Path,
						MakeShared<FJsonValueNumber>(Values->Num()), MakeShared<FJsonValueNumber>(2), true));
				}
				int32 Components[2] = {};
				for (int32 Index = 0; Index < 2; ++Index)
				{
					const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
					const FString ComponentPath = Path + TEXT("/") + FString::FromInt(Index);
					if (!Value.IsValid() || Value->Type != EJson::Number)
					{
						return Fail(OutError, WorldReadFact(TEXT("landscape_height_region_invalid"),
							TEXT("Landscape region coordinate must be numeric."), ComponentPath, WorldReadInputValue(Value),
							MakeShared<FJsonValueString>(TEXT("32-bit integer")), true));
					}
					FString Lexeme;
					int64 NumberValue = 0;
					if (!StateSemantic::CanonicalJson::TryGetNumberLexeme(*Value, Lexeme)
						|| !StateSemantic::TryParseInt64Number(Lexeme, NumberValue)
						|| NumberValue < MIN_int32 || NumberValue > MAX_int32)
					{
						return Fail(OutError, WorldReadFact(TEXT("landscape_height_region_invalid"),
							TEXT("Landscape region coordinate must be an exact integer in the signed 32-bit range."), ComponentPath, Value,
							MakeShared<FJsonValueString>(TEXT("integer from -2147483648 to 2147483647")), true));
					}
					Components[Index] = static_cast<int32>(NumberValue);
				}
				OutPoint = FIntPoint(Components[0], Components[1]);
				return true;
			}
		};

		bool IsOneOf(
			const FString& Value,
			std::initializer_list<const TCHAR*> Candidates)
		{
			for (const TCHAR* Candidate : Candidates)
			{
				if (Value == Candidate)
				{
					return true;
				}
			}
			return false;
		}
	}

	bool FUeAgentWorldReadNativeHookFactory::TryCreate(
		const FString& ProviderBinding,
		TSharedRef<IUeAgentWorldReadEnvironment> Environment,
		TSharedPtr<IUeAgentWorldReadHook>& OutHook,
		FUeAgentWorldReadError& OutError)
	{
		OutHook.Reset();
		OutError = {};
		if (IsOneOf(
				ProviderBinding,
				{
					TEXT("world.read.summary"),
					TEXT("world.read.actors"),
					TEXT("world.read.actor_components"),
					TEXT("world.read.selection"),
					TEXT("world.read.property"),
				}))
		{
			OutHook =
				MakeShared<FObjectHook>(
					ProviderBinding,
					MoveTemp(Environment));
		}
		else if (ProviderBinding == TEXT("world.read.content"))
		{
			OutHook =
				MakeShared<FContentHook>(
					ProviderBinding,
					MoveTemp(Environment));
		}
		else if (IsOneOf(
				ProviderBinding,
				{
					TEXT("world.read.ray_trace"),
					TEXT("world.read.overlap"),
					TEXT("world.read.capsule_sweep"),
					TEXT("world.read.capsule_path"),
					TEXT("world.read.nearby_shapes"),
				}))
		{
			OutHook =
				MakeShared<FSpatialHook>(
					ProviderBinding,
					MoveTemp(Environment));
		}
		else if (IsOneOf(
				ProviderBinding,
				{
					TEXT("world.read.mesh_closest_vertex"),
					TEXT("world.read.mesh_vertex_position"),
				}))
		{
			OutHook =
				MakeShared<FMeshHook>(
					ProviderBinding,
					MoveTemp(Environment));
		}
		else if (IsOneOf(
				ProviderBinding,
				{
					TEXT("world.read.nav_project_point"),
					TEXT("world.read.nav_find_path"),
					TEXT("world.read.nav_connectivity"),
				}))
		{
			OutHook =
				MakeShared<FNavigationHook>(
					ProviderBinding,
					MoveTemp(Environment));
		}
		else if (IsOneOf(
				ProviderBinding,
				{
					TEXT("world.read.data_layers"),
					TEXT("world.read.hlod_assignments"),
					TEXT("world.read.streaming_levels"),
					TEXT("world.read.partition"),
					TEXT("world.read.partition_health"),
				}))
		{
			OutHook =
				MakeShared<FTopologyHook>(
					ProviderBinding,
					MoveTemp(Environment));
		}
		else if (IsOneOf(
				ProviderBinding,
				{
					TEXT("world.read.physics_component"),
					TEXT("world.read.physics_constraint"),
				}))
		{
			OutHook =
				MakeShared<FPhysicsHook>(
					ProviderBinding,
					MoveTemp(Environment));
		}
		else if (ProviderBinding == TEXT("landscape.read.height"))
		{
			OutHook =
				MakeShared<FLandscapeHeightHook>(
					ProviderBinding,
					MoveTemp(Environment));
		}
		else
		{
			return Fail(OutError, WorldReadFact(TEXT("world_read_provider_unknown"),
				TEXT("No native world-read provider is registered for the requested binding."), TEXT("/provider_binding"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("registered world-read provider"))), ProviderBinding);
		}
		return true;
	}
}
