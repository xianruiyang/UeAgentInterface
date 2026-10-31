#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadNative.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "AssetRegistry/AssetData.h"
#include "AssetCompilingManager.h"
#include "Core/OriginResolve/UeAgentOriginResolveTypes.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/ContentDiscovery/UeAgentContentDiscovery.h"
#include "Utilities/AuthoringObjectApply/UeAgentOwnedObjectCreation.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTargetLocator.h"
#include "Utilities/UeAgentUObjectProperties.h"
#include "DomainAdapters/EditorProject/UeAgentEditorProjectMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataTable.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::AssetProjectRead
{
	using UE::UAI::ContextResource::FUeAgentOperationContext;
	using UE::UAI::Contract::FUeAgentContractValue;
	using UE::UAI::Discovery::FUeAgentCandidateBinding;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
            return false;
        }


        

		FString PointerPart(const FString& Value)
		{
			return Value.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}

		FString NativeField(const FString& Selector)
		{
			return TEXT("/fields/") + PointerPart(Selector);
		}

		TSharedPtr<FJsonValue> InputKind(const TSharedPtr<FJsonValue>& Value)
		{
			const TCHAR* Kind = !Value.IsValid() ? TEXT("missing")
				: Value->Type == EJson::Null ? TEXT("null")
				: Value->Type == EJson::String ? TEXT("string")
				: Value->Type == EJson::Number ? TEXT("number")
				: Value->Type == EJson::Boolean ? TEXT("boolean")
				: Value->Type == EJson::Array ? TEXT("array")
				: Value->Type == EJson::Object ? TEXT("object") : TEXT("none");
			return MakeShared<FJsonValueString>(Kind);
		}

		UE::UAI::Contract::FDiagnostic ReadFailure(const FString& Code, const FString& Reason,
			const FString& Field, const TSharedPtr<FJsonValue>& Actual,
			const TSharedPtr<FJsonValue>& Expected, const FString& InternalOwner = FString(),
			const FString& SourceKind = TEXT("native_result"))
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("asset_project_read"));
			Error.SourceKind = SourceKind;
			Error.PropertyPath = Field;
			Error.ActualValue = Actual;
			Error.ExpectedValue = Expected;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			Error.SubjectId = InternalOwner;
			return Error;
		}

		UE::UAI::Contract::FDiagnostic ReadCondition(const FString& Code, const FString& Reason,
			const FString& Field, const FString& InternalOwner = FString(),
			bool Actual = false, bool Expected = true, const FString& SourceKind = TEXT("native_result"))
		{
			return ReadFailure(Code, Reason, Field, MakeShared<FJsonValueBoolean>(Actual),
				MakeShared<FJsonValueBoolean>(Expected), InternalOwner, SourceKind);
		}

		bool ParseInput(
			const FUeAgentContractValue& Input,
			TSharedPtr<FJsonObject>& OutObject,
			FUeAgentAssetProjectReadError& OutError)
		{
			OutObject.Reset();
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Input.GetJsonValue());
			if (!FJsonSerializer::Deserialize(Reader, OutObject)
				|| !OutObject.IsValid())
			{
				OutError = ReadFailure(TEXT("asset_project_read_input_invalid"),
					TEXT("The canonical query input is not a valid JSON object."), TEXT("/input"),
					MakeShared<FJsonValueString>(TEXT("invalid JSON or non-object")), MakeShared<FJsonValueString>(TEXT("object")),
					FString(), TEXT("command_input"));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Reader->GetErrorMessage());
				return false;
			}
			return true;
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
				TEXT("asset-project-read"),
				Context.GetExecutionContext().GetContextRevision(),
				1);
		}

		FUeAgentStateDetails Details(const FString& DisplayName)
		{
			return FUeAgentStateDetails(
				DisplayName,
				TEXT(
					"Fact captured from an explicit "
					"AssetProjectRead environment owner"),
				{TEXT("AssetProjectRead")},
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
			FUeAgentAssetProjectReadError& OutError)
		{
			if (!Value.IsValid())
			{
				return Fail(OutError, ReadCondition(TEXT("asset_project_read_value_invalid"),
					TEXT("The captured field has no canonical value."), NativeField(Selector), OwnerId));
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
					TEXT("asset-project-read"),
					Value.ToSharedRef()),
				Details(Selector),
				StateRevision);
			if (!Entry.IsValid())
			{
				auto Error = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("asset_project_read_entry_invalid"), Entry.GetValidationDiagnostic());
				if (Error.SourceKind.IsEmpty()) Error.SourceKind = TEXT("native_result");
				if (Error.PropertyPath.IsEmpty()) Error.PropertyPath = NativeField(Selector);
				return Fail(OutError, MoveTemp(Error), OwnerId);
			}
			OutEntries.Add(Entry);
			return true;
		}

		bool AddString(
			const FString& SemanticId,
			const FString& TargetKind,
			const FString& OwnerId,
			const FString& Selector,
			const FString& Text,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentAssetProjectReadError& OutError)
		{
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
			bool bValue,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentAssetProjectReadError& OutError)
		{
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
			int64 Number,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentAssetProjectReadError& OutError)
		{
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryInteger(
					LexToString(Number),
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
			const double Number,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentAssetProjectReadError& OutError)
		{
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryDecimal(
					FString::SanitizeFloat(Number, 0),
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

		bool AddStringArray(
			const FString& SemanticId,
			const FString& TargetKind,
			const FString& OwnerId,
			const FString& Selector,
			const TArray<FString>& Strings,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentAssetProjectReadError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const FString& String : Strings)
			{
				FUeAgentCanonicalValuePtr Item;
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryString(
						String,
						Context.GetResourceTracker(),
						Item,
						StateError))
				{
					return Fail(OutError, StateError);
				}
				Items.Add(Item.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryArray(
					Items,
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
				TEXT("core.string_array"),
				EUeAgentValueKind::Array,
				Value,
				Context,
				OutEntries,
				OutError);
		}

		bool AddCodePaths(
			const FString& OwnerId,
			const TArray<UeAgentOriginResolve::FCodePath>& CodePaths,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentAssetProjectReadError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const UeAgentOriginResolve::FCodePath& CodePath : CodePaths)
			{
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
				auto AddStringField = [&](const TCHAR* Name, const FString& Text)
				{
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
					Fields.Add({Name, Value.ToSharedRef()});
					return true;
				};
				auto AddIntegerField = [&](const TCHAR* Name, int32 Number)
				{
					FUeAgentCanonicalValuePtr Value;
					FUeAgentStateSemanticError StateError;
					if (!FUeAgentCanonicalValue::TryInteger(
							LexToString(Number),
							Context.GetResourceTracker(),
							Value,
							StateError))
					{
						return Fail(OutError, StateError);
					}
					Fields.Add({Name, Value.ToSharedRef()});
					return true;
				};
				FUeAgentCanonicalValuePtr Item;
				FUeAgentStateSemanticError StateError;
				if (!AddStringField(TEXT("path"), CodePath.Path)
					|| !AddIntegerField(TEXT("line"), CodePath.Line)
					|| !AddIntegerField(TEXT("column"), CodePath.Column)
					|| !AddStringField(TEXT("symbol"), CodePath.Symbol)
					|| !AddStringField(TEXT("module"), CodePath.Module)
					|| !AddStringField(TEXT("status"), CodePath.Status)
					|| !FUeAgentCanonicalValue::TryObject(
						Fields,
						Context.GetResourceTracker(),
						Item,
						StateError))
				{
					if (OutError.Code.IsEmpty())
					{
						return Fail(OutError, StateError);
					}
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryArray(
					Items,
					Context.GetResourceTracker(),
					Value,
					StateError))
			{
				return Fail(OutError, StateError);
			}
			return AddValueEntry(
				TEXT("reference.origin:code_paths"),
				TEXT("reference_origin"),
				OwnerId,
				TEXT("origin.code_paths"),
				TEXT("reference.origin.code_path_array"),
				EUeAgentValueKind::Array,
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
			FUeAgentAssetProjectReadError& OutError)
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
							.GetProjectInstanceId()),
					Entries,
					Context.GetResourceTracker(),
					OutSnapshot,
					StateError))
			{
				return Fail(OutError, StateError);
			}
			return true;
		}

		TArray<FString> ReadStringArray(
			const TSharedPtr<FJsonObject>& Input,
			const TCHAR* Field)
		{
			TArray<FString> Result;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (Input.IsValid()
				&& Input->TryGetArrayField(Field, Values)
				&& Values)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Values)
				{
					if (Value.IsValid()
						&& Value->Type == EJson::String)
					{
						Result.Add(Value->AsString());
					}
				}
			}
			return Result;
		}

		const TCHAR* RelationDirectionText(
			const EUeAgentAssetRelationDirection Direction)
		{
			switch (Direction)
			{
			case EUeAgentAssetRelationDirection::Dependencies:
				return TEXT("dependencies");
			case EUeAgentAssetRelationDirection::Referencers:
				return TEXT("referencers");
			case EUeAgentAssetRelationDirection::Both:
				return TEXT("both");
			default:
				return TEXT("");
			}
		}

		bool TryParseRelationGraphRequest(
			const TSharedPtr<FJsonObject>& Input,
			FUeAgentAssetRelationGraphRequest& OutRequest,
			FUeAgentAssetProjectReadError& OutError)
		{
			OutRequest = {};
			const auto InvalidChoice = [&](const TCHAR* Code, const TCHAR* Field,
				const FString& Actual, const TArray<FString>& Allowed)
			{
				auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code,
					TEXT("Unsupported asset relation option."), TEXT("execution"));
				Diagnostic.PropertyPath = Field;
				Diagnostic.ActualValue = MakeShared<FJsonValueString>(Actual);
				TArray<TSharedPtr<FJsonValue>> Choices;
				for (const auto& Choice : Allowed) Choices.Add(MakeShared<FJsonValueString>(Choice));
				Diagnostic.ExpectedValue = MakeShared<FJsonValueArray>(MoveTemp(Choices));
				return Fail(OutError, MoveTemp(Diagnostic));
			};
			FString Direction;
			if (!Input.IsValid()
				|| !Input->TryGetStringField(TEXT("direction"), Direction))
			{
				auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_project_read_relation_direction_missing"),
					TEXT("Asset relation direction must be provided as a string."), TEXT("execution"));
				Diagnostic.PropertyPath = TEXT("direction");
				Diagnostic.ActualValue = Input.IsValid() ? Input->TryGetField(TEXT("direction")) : nullptr;
				Diagnostic.ActualValuePresent = Diagnostic.ActualValue.IsValid();
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			if (Direction == TEXT("dependencies"))
			{
				OutRequest.Direction =
					EUeAgentAssetRelationDirection::Dependencies;
			}
			else if (Direction == TEXT("referencers"))
			{
				OutRequest.Direction =
					EUeAgentAssetRelationDirection::Referencers;
			}
			else if (Direction == TEXT("both"))
			{
				OutRequest.Direction = EUeAgentAssetRelationDirection::Both;
			}
			else
			{
				return InvalidChoice(TEXT("asset_project_read_relation_input_invalid"), TEXT("direction"), Direction,
					{TEXT("dependencies"), TEXT("referencers"), TEXT("both")});
			}

			for (const FString& Kind : ReadStringArray(
				Input, TEXT("relation_kinds")))
			{
				if (Kind == TEXT("hard"))
				{
					OutRequest.RelationKinds.Add(
						EUeAgentAssetRelationKind::Hard);
				}
				else if (Kind == TEXT("soft"))
				{
					OutRequest.RelationKinds.Add(
						EUeAgentAssetRelationKind::Soft);
				}
				else if (Kind == TEXT("manage"))
				{
					OutRequest.RelationKinds.Add(
						EUeAgentAssetRelationKind::Manage);
				}
				else if (Kind == TEXT("searchable_name"))
				{
					OutRequest.RelationKinds.Add(
						EUeAgentAssetRelationKind::SearchableName);
				}
				else
				{
					return InvalidChoice(TEXT("asset_project_read_relation_kind_invalid"), TEXT("relation_kinds"), Kind,
						{TEXT("hard"), TEXT("soft"), TEXT("manage"), TEXT("searchable_name")});
				}
			}

			FString PackageScope = TEXT("all");
			Input->TryGetStringField(TEXT("package_scope"), PackageScope);
			if (PackageScope == TEXT("all"))
			{
				OutRequest.PackageScope =
					EUeAgentAssetRelationPackageScope::All;
			}
			else if (PackageScope == TEXT("game"))
			{
				OutRequest.PackageScope =
					EUeAgentAssetRelationPackageScope::Game;
			}
			else if (PackageScope == TEXT("editor_only"))
			{
				OutRequest.PackageScope =
					EUeAgentAssetRelationPackageScope::EditorOnly;
			}
			else if (PackageScope == TEXT("build"))
			{
				OutRequest.PackageScope =
					EUeAgentAssetRelationPackageScope::Build;
			}
			else
			{
				return InvalidChoice(TEXT("asset_project_read_relation_input_invalid"), TEXT("package_scope"), PackageScope,
					{TEXT("all"), TEXT("game"), TEXT("editor_only"), TEXT("build")});
			}

			return true;
		}

		FString RelationNodeReference(const FUeAgentAssetRelationNode& Node)
		{
			return !Node.ObjectPath.IsEmpty()
				? Node.ObjectPath
				: Node.PackageName;
		}

		TSharedPtr<FJsonObject> RelationNodeJson(
			const FUeAgentAssetRelationNode& Node,
			const FString& Reference)
		{
			const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("ref"), Reference);
			if (!Node.PackageName.IsEmpty())
			{
				Object->SetStringField(TEXT("package_name"), Node.PackageName);
			}
			if (!Node.ObjectPath.IsEmpty())
			{
				Object->SetStringField(TEXT("object_path"), Node.ObjectPath);
			}
			Object->SetBoolField(TEXT("resolved"), Node.bResolved);
			if (Node.bRedirector)
			{
				Object->SetBoolField(TEXT("redirector"), true);
				if (!Node.RedirectTarget.IsEmpty())
				{
					Object->SetStringField(
						TEXT("redirect_target"), Node.RedirectTarget);
				}
			}
			return Object;
		}

		bool AddCanonicalJsonEntry(
			const FString& SemanticId,
			const FString& OwnerId,
			const FString& Selector,
			const FString& TypeId,
			const TSharedPtr<FJsonValue>& JsonValue,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentAssetProjectReadError& OutError)
		{
			FUeAgentCanonicalValuePtr Value;
			UE::UAI::Contract::FDiagnostic ValueError;
			if (!JsonValue.IsValid()
				|| !CanonicalJson::TryFromJsonValue(
					JsonValue,
					Context.GetResourceTracker(),
					Value,
					ValueError))
			{
				auto Error = ValueError.IsValid() ? MoveTemp(ValueError) : ReadCondition(
					TEXT("asset_project_read_canonical_object_invalid"),
					!JsonValue.IsValid() ? TEXT("The captured field has no JSON value to convert.")
						: TEXT("The captured JSON field could not be converted to a canonical value."),
					NativeField(Selector), OwnerId);
				if (Error.PropertyPath.IsEmpty()) Error.PropertyPath = NativeField(Selector);
				return Fail(OutError, MoveTemp(Error), OwnerId);
			}
			return AddValueEntry(
				SemanticId,
				TEXT("asset"),
				OwnerId,
				Selector,
				TypeId,
				Value->GetKind(),
				Value,
				Context,
				Entries,
				OutError);
		}

		bool AddCanonicalObjectEntry(
			const FString& SemanticId,
			const FString& OwnerId,
			const FString& Selector,
			const FString& TypeId,
			const TSharedPtr<FJsonObject>& Object,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentAssetProjectReadError& OutError)
		{
			TSharedPtr<FJsonValue> JsonValue;
			if (Object.IsValid()) JsonValue = MakeShared<FJsonValueObject>(Object);
			return AddCanonicalJsonEntry(SemanticId, OwnerId, Selector, TypeId,
				JsonValue, Context, Entries, OutError);
		}

		TSharedPtr<FJsonObject> VectorJson(
			const double X,
			const double Y,
			const double Z)
		{
			const TSharedPtr<FJsonObject> Vector = MakeShared<FJsonObject>();
			Vector->SetNumberField(TEXT("x"), X);
			Vector->SetNumberField(TEXT("y"), Y);
			Vector->SetNumberField(TEXT("z"), Z);
			return Vector;
		}

		bool AddRelationGraph(
			FUeAgentAssetRelationGraphResult Graph,
			const FString& OwnerId,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentAssetProjectReadError& OutError)
		{
			Graph.Edges.Sort([](
				const FUeAgentAssetRelationEdge& Left,
				const FUeAgentAssetRelationEdge& Right)
			{
				return Left.TraversalDirection != Right.TraversalDirection
					? Left.TraversalDirection < Right.TraversalDirection
					: Left.Depth != Right.Depth
						? Left.Depth < Right.Depth
						: Left.SourceIdentifier != Right.SourceIdentifier
							? Left.SourceIdentifier < Right.SourceIdentifier
							: Left.TargetIdentifier != Right.TargetIdentifier
								? Left.TargetIdentifier < Right.TargetIdentifier
								: Left.Category != Right.Category
									? Left.Category < Right.Category
									: Left.PropertyFlags < Right.PropertyFlags;
			});

			Graph.Nodes.Sort([](
				const FUeAgentAssetRelationNode& Left,
				const FUeAgentAssetRelationNode& Right)
			{
				const FString LeftReference = RelationNodeReference(Left);
				const FString RightReference = RelationNodeReference(Right);
				return LeftReference != RightReference
					? LeftReference < RightReference
					: Left.StableIdentifier < Right.StableIdentifier;
			});

			TMap<FString, FString> References;
			TArray<TSharedPtr<FJsonValue>> Nodes;
			for (const FUeAgentAssetRelationNode& Node : Graph.Nodes)
			{
				const FString Reference = RelationNodeReference(Node);
				if (Node.StableIdentifier.IsEmpty() || Reference.IsEmpty())
				{
					auto Error = ReadCondition(TEXT("asset_project_read_relation_graph_invalid"),
						Node.StableIdentifier.IsEmpty() ? TEXT("A relation graph node has no stable identity.")
							: TEXT("A relation graph node has no public reference."),
						TEXT("/relation_graph/nodes/") + FString::FromInt(Nodes.Num())
							+ (Node.StableIdentifier.IsEmpty() ? TEXT("/identity_present") : TEXT("/reference_present")), OwnerId);
					Error.ActualEvidence = MakeShared<FJsonValueString>(Node.StableIdentifier);
					return Fail(OutError, MoveTemp(Error));
				}
				References.Add(Node.StableIdentifier, Reference);
				Nodes.Add(MakeShared<FJsonValueObject>(
					RelationNodeJson(Node, Reference)));
			}

			TArray<TSharedPtr<FJsonValue>> Edges;
			for (const FUeAgentAssetRelationEdge& Edge : Graph.Edges)
			{
				const FString* Source = References.Find(Edge.SourceIdentifier);
				const FString* Target = References.Find(Edge.TargetIdentifier);
				if (!Source || !Target)
				{
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetBoolField(TEXT("source_present"), Source != nullptr);
					Actual->SetBoolField(TEXT("target_present"), Target != nullptr);
					auto Expected = MakeShared<FJsonObject>();
					Expected->SetBoolField(TEXT("source_present"), true);
					Expected->SetBoolField(TEXT("target_present"), true);
					auto Error = ReadFailure(TEXT("asset_project_read_relation_graph_invalid"),
						TEXT("A relation edge refers to a source or target node missing from the graph."),
						TEXT("/relation_graph/edges/") + FString::FromInt(Edges.Num()),
						MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected), OwnerId);
					auto Identity = MakeShared<FJsonObject>();
					Identity->SetStringField(TEXT("source"), Edge.SourceIdentifier);
					Identity->SetStringField(TEXT("target"), Edge.TargetIdentifier);
					Error.ActualEvidence = MakeShared<FJsonValueObject>(Identity);
					return Fail(OutError, MoveTemp(Error));
				}
				const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
				Object->SetStringField(TEXT("source"), *Source);
				Object->SetStringField(TEXT("target"), *Target);
				Object->SetStringField(
					TEXT("direction"),
					Edge.TraversalDirection == TEXT("dependencies")
						? TEXT("outgoing") : TEXT("incoming"));
				Object->SetNumberField(TEXT("depth"), Edge.Depth);
				Object->SetStringField(TEXT("relation"), Edge.Category);
				TArray<TSharedPtr<FJsonValue>> Properties;
				if (Edge.Category == TEXT("package"))
				{
					Properties.Add(MakeShared<FJsonValueString>(
						Edge.bHard ? TEXT("hard") : TEXT("soft")));
					Properties.Add(MakeShared<FJsonValueString>(
						Edge.bGame ? TEXT("game") : TEXT("editor_only")));
					Properties.Add(MakeShared<FJsonValueString>(
						Edge.bBuild ? TEXT("build") : TEXT("not_build")));
				}
				else if (Edge.Category == TEXT("manage"))
				{
					Properties.Add(MakeShared<FJsonValueString>(
						Edge.bDirect ? TEXT("direct") : TEXT("indirect")));
				}
				Object->SetArrayField(TEXT("properties"), MoveTemp(Properties));
				TArray<TSharedPtr<FJsonValue>> Path;
				for (const FString& Identifier : Edge.TraversalPath)
				{
					const FString* Reference = References.Find(Identifier);
					if (!Reference)
					{
						auto Error = ReadCondition(TEXT("asset_project_read_relation_graph_invalid"),
							TEXT("A relation traversal path refers to a node missing from the graph."),
							TEXT("/relation_graph/edges/") + FString::FromInt(Edges.Num())
								+ TEXT("/path/") + FString::FromInt(Path.Num()) + TEXT("/node_present"), OwnerId);
						Error.ActualEvidence = MakeShared<FJsonValueString>(Identifier);
						return Fail(OutError, MoveTemp(Error));
					}
					Path.Add(MakeShared<FJsonValueString>(*Reference));
				}
				Object->SetArrayField(TEXT("path"), MoveTemp(Path));
				if (Edge.bCycle)
				{
					Object->SetBoolField(TEXT("cycle"), true);
				}
				Edges.Add(MakeShared<FJsonValueObject>(Object));
			}

			const TSharedPtr<FJsonObject> Coverage = MakeShared<FJsonObject>();
			Coverage->SetBoolField(TEXT("complete"), Graph.bComplete);
			Coverage->SetBoolField(
				TEXT("registry_loading"), Graph.bRegistryLoading);
			Coverage->SetStringField(
				TEXT("scope"), TEXT("on_disk_asset_registry"));
			Coverage->SetBoolField(
				TEXT("runtime_references_included"),
				Graph.bRuntimeReferencesIncluded);

			const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
			const FString* RootReference =
				References.Find(Graph.Root.StableIdentifier);
			if (!RootReference)
			{
				auto Error = ReadCondition(TEXT("asset_project_read_relation_graph_invalid"),
					TEXT("The relation graph root is missing from its node set."), TEXT("/relation_graph/root/node_present"), OwnerId);
				Error.ActualEvidence = MakeShared<FJsonValueString>(Graph.Root.StableIdentifier);
				return Fail(OutError, MoveTemp(Error));
			}
			Root->SetStringField(TEXT("root"), *RootReference);
			Root->SetObjectField(TEXT("coverage"), Coverage);
			Root->SetArrayField(TEXT("nodes"), MoveTemp(Nodes));
			Root->SetArrayField(TEXT("edges"), MoveTemp(Edges));

			return AddCanonicalObjectEntry(
				TEXT("asset.relations:graph"),
				OwnerId,
				TEXT("relation_graph"),
				TEXT("asset.relation_graph"),
				Root,
				Context,
				Entries,
				OutError);
		}

		bool AddObjectIdentity(
			UObject* Object,
			const FString& SemanticPrefix,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentAssetProjectReadError& OutError)
		{
			const FString OwnerId = Object->GetPathName();
			return AddString(
					SemanticPrefix + TEXT(":path"),
					TEXT("asset"),
					OwnerId,
					TEXT("path"),
					OwnerId,
					Context,
					Entries,
					OutError)
				&& AddString(
					SemanticPrefix + TEXT(":class"),
					TEXT("asset"),
					OwnerId,
					TEXT("class_path"),
					Object->GetClass()->GetPathName(),
					Context,
					Entries,
					OutError)
				&& AddString(
					SemanticPrefix + TEXT(":package"),
					TEXT("asset"),
					OwnerId,
					TEXT("package_name"),
					Object->GetOutermost()->GetName(),
					Context,
					Entries,
					OutError)
				&& AddBoolean(
					SemanticPrefix + TEXT(":dirty"),
					TEXT("asset"),
					OwnerId,
					TEXT("package_dirty"),
					Object->GetOutermost()->IsDirty(),
					Context,
					Entries,
					OutError);
		}

		bool AddRequestedProperties(
			UObject* Object,
			const TArray<FString>& FieldIds,
			const FString& SemanticPrefix,
			const FString& TargetKind,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentAssetProjectReadError& OutError)
		{
			if (FieldIds.IsEmpty())
			{
				int64 PropertyCount = 0;
				for (TFieldIterator<FProperty> It(Object->GetClass());
					It;
					++It)
				{
					++PropertyCount;
				}
				return AddInteger(
					SemanticPrefix + TEXT(":count"),
					TargetKind,
					Object->GetPathName(),
					TEXT("property_count"),
					PropertyCount,
					Context,
					Entries,
					OutError);
			}

			for (const FString& FieldId : FieldIds)
			{
				const FProperty* Property =
					FindFProperty<FProperty>(
						Object->GetClass(),
						*FieldId);
				if (!Property)
				{
				return Fail(OutError, ReadCondition(TEXT("asset_project_read_property_not_found"),
					TEXT("The selected object has no property with the requested field name."), NativeField(FieldId) + TEXT("/present"), Object->GetPathName()));
				}
				FString Text;
				Property->ExportText_InContainer(
					0,
					Text,
					Object,
					Object,
					Object,
					PPF_None);
				if (!AddString(
						SemanticPrefix + TEXT(":value:")
							+ FieldId,
						TargetKind,
						Object->GetPathName(),
						FieldId,
						Text,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
			}
			return true;
		}

		bool MatchesCollectionKey(
			const FString& Key,
			const TArray<FString>& RequestedKeys,
			const FString& KeyPrefix)
		{
			return (RequestedKeys.IsEmpty() || RequestedKeys.Contains(Key))
				&& (KeyPrefix.IsEmpty() || Key.StartsWith(KeyPrefix));
		}

		bool AddCollectionObject(
			UObject* Asset,
			const FString& CollectionKind,
			TArray<TSharedPtr<FJsonValue>> Values,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentAssetProjectReadError& OutError)
		{
			const TSharedPtr<FJsonObject> Collection = MakeShared<FJsonObject>();
			Collection->SetStringField(TEXT("kind"), CollectionKind);
			Collection->SetNumberField(TEXT("count"), Values.Num());
			Collection->SetArrayField(CollectionKind, MoveTemp(Values));
			return AddCanonicalObjectEntry(
				TEXT("asset.collection:result"),
				Asset->GetPathName(),
				TEXT("collection"),
				TEXT("asset.collection"),
				Collection,
				Context,
				Entries,
				OutError);
		}

		bool TryAddDataTableRows(
			UObject* Asset,
			const TSharedPtr<FJsonObject>& Input,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentAssetProjectReadError& OutError)
		{
			const UDataTable* Table = Cast<UDataTable>(Asset);
			if (!Table || !Table->GetRowStruct())
			{
				return Fail(OutError, !Table
					? ReadFailure(TEXT("asset_project_read_collection_kind_unsupported"),
						TEXT("Row collection requires a DataTable asset."), TEXT("/asset/type"),
						MakeShared<FJsonValueString>(Asset->GetClass()->GetPathName()), MakeShared<FJsonValueString>(TEXT("DataTable")), Asset->GetPathName())
					: ReadCondition(TEXT("asset_project_read_collection_kind_unsupported"),
						TEXT("The DataTable has no row struct."), TEXT("/row_struct/available"), Asset->GetPathName()));
			}
			const TArray<FString> RequestedKeys =
				ReadStringArray(Input, TEXT("keys"));
			const TArray<FString> FieldIds =
				ReadStringArray(Input, TEXT("field_ids"));
			FString KeyPrefix;
			Input->TryGetStringField(TEXT("key_prefix"), KeyPrefix);
			TSet<FString> MatchedKeys;
			TArray<TSharedPtr<FJsonValue>> Rows;
			TArray<FName> RowNames = Table->GetRowNames();
			RowNames.Sort(FNameLexicalLess());
			for (const FName RowName : RowNames)
			{
				const FString Key = RowName.ToString();
				if (!MatchesCollectionKey(Key, RequestedKeys, KeyPrefix))
				{
					continue;
				}
				uint8* const* RowData = Table->GetRowMap().Find(RowName);
				if (!RowData || !*RowData)
				{
					return Fail(OutError, ReadCondition(TEXT("asset_project_read_collection_row_unavailable"),
						TEXT("The DataTable row has no readable native row data."),
						TEXT("/rows/") + PointerPart(Key) + TEXT("/data_available"), Asset->GetPathName()));
				}
				MatchedKeys.Add(Key);
				const FString RowOwner = Asset->GetPathName()
					+ TEXT("#row:") + Key;
				const TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("key"), Key);
				const TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
				for (const FString& FieldId : FieldIds)
				{
					FProperty* Property = FindFProperty<FProperty>(
						Table->GetRowStruct(), *FieldId);
					if (!Property)
					{
						return Fail(OutError, ReadCondition(TEXT("asset_project_read_collection_field_not_found"),
							TEXT("The DataTable row struct has no property with the requested field name."),
							TEXT("/rows/") + PointerPart(Key) + TEXT("/fields/") + PointerPart(FieldId) + TEXT("/present"), RowOwner));
					}
					FString ValueText;
					const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(
						*RowData);
					Property->ExportTextItem_Direct(
						ValueText, ValuePtr, nullptr, Asset, PPF_None);
					Fields->SetStringField(FieldId, ValueText);
				}
				if (!FieldIds.IsEmpty())
				{
					Row->SetObjectField(TEXT("fields"), Fields);
				}
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			for (const FString& RequestedKey : RequestedKeys)
			{
				if (!MatchedKeys.Contains(RequestedKey))
				{
					return Fail(OutError, ReadCondition(TEXT("asset_project_read_collection_key_not_found"),
						TEXT("The requested row key did not match a row in the selected DataTable scope."),
						TEXT("/rows/") + PointerPart(RequestedKey) + TEXT("/matched"), Asset->GetPathName()));
				}
			}
			return AddCollectionObject(
				Asset, TEXT("rows"), MoveTemp(Rows),
				Context, Entries, OutError);
		}

		bool TryAddReflectedCollection(
			UObject* Asset,
			const FString& CollectionKind,
			const TSharedPtr<FJsonObject>& Input,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentAssetProjectReadError& OutError)
		{
			const TArray<FString> RequestedKeys =
				ReadStringArray(Input, TEXT("keys"));
			const TArray<FString> FieldIds =
				ReadStringArray(Input, TEXT("field_ids"));
			FString KeyPrefix;
			Input->TryGetStringField(TEXT("key_prefix"), KeyPrefix);
			TArray<FProperty*> Properties;
			if (FieldIds.IsEmpty())
			{
				for (TFieldIterator<FProperty> It(Asset->GetClass()); It; ++It)
				{
					FProperty* Property = *It;
					if ((CollectionKind == TEXT("items")
							&& (Property->IsA<FArrayProperty>()
								|| Property->IsA<FSetProperty>()))
						|| (CollectionKind == TEXT("entries")
							&& Property->IsA<FMapProperty>()))
					{
						Properties.Add(Property);
					}
				}
				Properties.Sort(
					[](const FProperty& Left, const FProperty& Right)
					{
						return Left.GetName() < Right.GetName();
					});
			}
			else
			{
				for (const FString& FieldId : FieldIds)
				{
					FProperty* Property = FindFProperty<FProperty>(
						Asset->GetClass(), *FieldId);
					const bool bCompatible = Property
						&& ((CollectionKind == TEXT("items")
								&& (Property->IsA<FArrayProperty>()
									|| Property->IsA<FSetProperty>()))
							|| (CollectionKind == TEXT("entries")
								&& Property->IsA<FMapProperty>()));
					if (!bCompatible)
					{
						return Fail(OutError, ReadFailure(TEXT("asset_project_read_collection_field_kind_mismatch"),
							TEXT("The requested field does not have the native property kind required by this collection query."),
							NativeField(FieldId) + TEXT("/type"),
							MakeShared<FJsonValueString>(Property ? Property->GetCPPType() : TEXT("missing")),
							MakeShared<FJsonValueString>(CollectionKind == TEXT("items") ? TEXT("array or set") : TEXT("map")), Asset->GetPathName()));
					}
					Properties.Add(Property);
				}
			}
			if (FieldIds.IsEmpty())
			{
				if (!RequestedKeys.IsEmpty() || !KeyPrefix.IsEmpty())
				{
					auto Selectors = MakeShared<FJsonObject>();
					Selectors->SetNumberField(TEXT("key_count"), RequestedKeys.Num());
					Selectors->SetStringField(TEXT("key_prefix"), KeyPrefix);
					return Fail(OutError, ReadFailure(TEXT("asset_project_read_collection_field_required"),
						TEXT("Key selection requires an explicit collection field."), TEXT("/field_ids"),
						MakeShared<FJsonValueObject>(Selectors), MakeShared<FJsonValueString>(TEXT("at least one collection field")),
						Asset->GetPathName(), TEXT("command_input")));
				}
				int64 TotalCount = 0;
				TArray<TSharedPtr<FJsonValue>> Fields;
				for (FProperty* Property : Properties)
				{
					void* Container =
						Property->ContainerPtrToValuePtr<void>(Asset);
					int32 Count = 0;
					if (FArrayProperty* Array = CastField<FArrayProperty>(Property))
					{
						Count = FScriptArrayHelper(Array, Container).Num();
					}
					else if (FSetProperty* Set = CastField<FSetProperty>(Property))
					{
						Count = FScriptSetHelper(Set, Container).Num();
					}
					else if (FMapProperty* Map = CastField<FMapProperty>(Property))
					{
						Count = FScriptMapHelper(Map, Container).Num();
					}
					TotalCount += Count;
					const TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
					Field->SetStringField(TEXT("field"), Property->GetName());
					Field->SetNumberField(TEXT("count"), Count);
					Fields.Add(MakeShared<FJsonValueObject>(Field));
				}
				const TSharedPtr<FJsonObject> Collection = MakeShared<FJsonObject>();
				Collection->SetStringField(TEXT("kind"), CollectionKind);
				Collection->SetNumberField(TEXT("count"), TotalCount);
				Collection->SetArrayField(TEXT("fields"), MoveTemp(Fields));
				return AddCanonicalObjectEntry(
					TEXT("asset.collection:result"),
					Asset->GetPathName(),
					TEXT("collection"),
					TEXT("asset.collection"),
					Collection,
					Context,
					Entries,
					OutError);
			}

			TArray<TSharedPtr<FJsonValue>> Values;
			for (FProperty* Property : Properties)
			{
				void* Container = Property->ContainerPtrToValuePtr<void>(Asset);
				if (FArrayProperty* Array = CastField<FArrayProperty>(Property))
				{
					FScriptArrayHelper Helper(Array, Container);
					for (int32 Index = 0; Index < Helper.Num(); ++Index)
					{
						const FString Key = LexToString(Index);
						if (!MatchesCollectionKey(Key, RequestedKeys, KeyPrefix))
						{
							continue;
						}
						FString ValueText;
						Array->Inner->ExportTextItem_Direct(
							ValueText, Helper.GetRawPtr(Index), nullptr,
							Asset, PPF_None);
						const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("field"), Property->GetName());
						Item->SetNumberField(TEXT("index"), Index);
						Item->SetStringField(TEXT("value"), ValueText);
						Values.Add(MakeShared<FJsonValueObject>(Item));
					}
				}
				else if (FSetProperty* Set = CastField<FSetProperty>(Property))
				{
					FScriptSetHelper Helper(Set, Container);
					int32 ItemIndex = 0;
					for (int32 InternalIndex = 0;
						InternalIndex < Helper.GetMaxIndex(); ++InternalIndex)
					{
						if (!Helper.IsValidIndex(InternalIndex))
						{
							continue;
						}
						FString ValueText;
						Set->ElementProp->ExportTextItem_Direct(
							ValueText, Helper.GetElementPtr(InternalIndex),
							nullptr, Asset, PPF_None);
						const FString Key = LexToString(ItemIndex++);
						if (!MatchesCollectionKey(Key, RequestedKeys, KeyPrefix))
						{
							continue;
						}
						const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("field"), Property->GetName());
						Item->SetNumberField(TEXT("index"), FCString::Atoi(*Key));
						Item->SetStringField(TEXT("value"), ValueText);
						Values.Add(MakeShared<FJsonValueObject>(Item));
					}
				}
				else if (FMapProperty* Map = CastField<FMapProperty>(Property))
				{
					FScriptMapHelper Helper(Map, Container);
					for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
					{
						if (!Helper.IsValidIndex(Index))
						{
							continue;
						}
						FString Key;
						Map->KeyProp->ExportTextItem_Direct(
							Key, Helper.GetKeyPtr(Index), nullptr,
							Asset, PPF_None);
						if (!MatchesCollectionKey(Key, RequestedKeys, KeyPrefix))
						{
							continue;
						}
						FString ValueText;
						Map->ValueProp->ExportTextItem_Direct(
							ValueText, Helper.GetValuePtr(Index), nullptr,
							Asset, PPF_None);
						const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("field"), Property->GetName());
						Entry->SetStringField(TEXT("key"), Key);
						Entry->SetStringField(TEXT("value"), ValueText);
						Values.Add(MakeShared<FJsonValueObject>(Entry));
					}
				}
			}
			Values.Sort(
				[&CollectionKind](
					const TSharedPtr<FJsonValue>& Left,
					const TSharedPtr<FJsonValue>& Right)
				{
					const TSharedPtr<FJsonObject> LeftObject =
						Left.IsValid() ? Left->AsObject() : nullptr;
					const TSharedPtr<FJsonObject> RightObject =
						Right.IsValid() ? Right->AsObject() : nullptr;
					if (!LeftObject.IsValid() || !RightObject.IsValid())
					{
						return LeftObject.IsValid();
					}
					const FString LeftField =
						LeftObject->GetStringField(TEXT("field"));
					const FString RightField =
						RightObject->GetStringField(TEXT("field"));
					if (LeftField != RightField)
					{
						return LeftField < RightField;
					}
					return CollectionKind == TEXT("items")
						? LeftObject->GetNumberField(TEXT("index"))
							< RightObject->GetNumberField(TEXT("index"))
						: LeftObject->GetStringField(TEXT("key"))
							< RightObject->GetStringField(TEXT("key"));
				});
			return AddCollectionObject(
				Asset, CollectionKind, MoveTemp(Values),
				Context, Entries, OutError);
		}

		bool TryAddAssetSample(
			UObject* Asset,
			const TSharedPtr<FJsonObject>& Input,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentAssetProjectReadError& OutError)
		{
			FString SampleKind;
			if (!Input->TryGetStringField(TEXT("sample_kind"), SampleKind))
			{
				return Fail(OutError, ReadFailure(TEXT("asset_project_read_sample_kind_missing"),
					TEXT("Asset sampling requires a string sample_kind."), TEXT("/sample_kind"),
					InputKind(Input->TryGetField(TEXT("sample_kind"))), MakeShared<FJsonValueString>(TEXT("string")),
					Asset->GetPathName(), TEXT("command_input")));
			}
			if (SampleKind == TEXT("bounds"))
			{
				if (Input->HasField(TEXT("coordinates"))
					|| Input->HasField(TEXT("index"))
					|| Input->HasField(TEXT("channel")))
				{
					const FString Field = Input->HasField(TEXT("coordinates")) ? TEXT("coordinates")
						: Input->HasField(TEXT("index")) ? TEXT("index") : TEXT("channel");
					return Fail(OutError, ReadCondition(TEXT("asset_project_read_sample_selector_unsupported"),
						TEXT("Bounds sampling does not accept coordinates, index or channel selectors."),
						TEXT("/") + Field, Asset->GetPathName(), true, false, TEXT("command_input")));
				}
				FBoxSphereBounds Bounds;
				if (const UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
				{
					Bounds = StaticMesh->GetBounds();
				}
				else if (const USkeletalMesh* SkeletalMesh =
					Cast<USkeletalMesh>(Asset))
				{
					Bounds = SkeletalMesh->GetBounds();
				}
				else
				{
					return Fail(OutError, ReadFailure(TEXT("asset_project_read_sample_target_unsupported"),
						TEXT("Bounds sampling requires a static mesh or skeletal mesh asset."), TEXT("/asset/type"),
						MakeShared<FJsonValueString>(Asset->GetClass()->GetPathName()),
						MakeShared<FJsonValueString>(TEXT("StaticMesh or SkeletalMesh")), Asset->GetPathName()));
				}
				const TSharedPtr<FJsonObject> Sample = MakeShared<FJsonObject>();
				Sample->SetStringField(TEXT("kind"), SampleKind);
				Sample->SetObjectField(
					TEXT("origin_cm"),
					VectorJson(Bounds.Origin.X, Bounds.Origin.Y, Bounds.Origin.Z));
				Sample->SetObjectField(
					TEXT("extent_cm"),
					VectorJson(
						Bounds.BoxExtent.X,
						Bounds.BoxExtent.Y,
						Bounds.BoxExtent.Z));
				Sample->SetNumberField(
					TEXT("sphere_radius_cm"), Bounds.SphereRadius);
				return AddCanonicalObjectEntry(
					TEXT("asset.sample:result"),
					Asset->GetPathName(),
					TEXT("sample"),
					TEXT("asset.sample"),
					Sample,
					Context,
					Entries,
					OutError);
			}
			if (SampleKind == TEXT("vertex"))
			{
				if (!Input->HasField(TEXT("index"))
					|| Input->HasField(TEXT("coordinates"))
					|| Input->HasField(TEXT("channel")))
				{
					const bool bIndexMissing = !Input->HasField(TEXT("index"));
					const FString Field = bIndexMissing ? TEXT("index")
						: Input->HasField(TEXT("coordinates")) ? TEXT("coordinates") : TEXT("channel");
					return Fail(OutError, ReadCondition(TEXT("asset_project_read_sample_selector_unsupported"),
						bIndexMissing ? TEXT("Vertex sampling requires an index selector.")
							: TEXT("Vertex sampling does not accept coordinates or channel selectors."),
						TEXT("/") + Field, Asset->GetPathName(), !bIndexMissing, bIndexMissing, TEXT("command_input")));
				}
				const UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset);
				const FMeshDescription* Description = StaticMesh
					? StaticMesh->GetMeshDescription(0) : nullptr;
				if (!Description)
				{
					return Fail(OutError, !StaticMesh
						? ReadFailure(TEXT("asset_project_read_sample_target_unsupported"),
							TEXT("Vertex sampling requires a static mesh asset."), TEXT("/asset/type"),
							MakeShared<FJsonValueString>(Asset->GetClass()->GetPathName()), MakeShared<FJsonValueString>(TEXT("StaticMesh")), Asset->GetPathName())
						: ReadCondition(TEXT("asset_project_read_sample_target_unsupported"),
							TEXT("The static mesh has no LOD 0 mesh description available for vertex sampling."),
							TEXT("/mesh/lods/0/description_available"), Asset->GetPathName()));
				}
				const int32 Index = static_cast<int32>(
					Input->GetIntegerField(TEXT("index")));
				const FVertexID VertexId(Index);
				if (!Description->IsVertexValid(VertexId))
				{
					return Fail(OutError, ReadFailure(TEXT("asset_project_read_sample_index_out_of_range"),
						TEXT("The requested vertex ID is not valid in the LOD 0 mesh description."), TEXT("/index"),
						MakeShared<FJsonValueNumber>(Index), MakeShared<FJsonValueString>(TEXT("existing LOD 0 vertex ID")), Asset->GetPathName()));
				}
				const FStaticMeshConstAttributes Attributes(*Description);
				const TVertexAttributesConstRef<FVector3f> Positions =
					Attributes.GetVertexPositions();
				const FVector3f Position = Positions[VertexId];
				const TSharedPtr<FJsonObject> Sample = MakeShared<FJsonObject>();
				Sample->SetStringField(TEXT("kind"), SampleKind);
				Sample->SetNumberField(TEXT("index"), Index);
				Sample->SetObjectField(
					TEXT("position_cm"),
					VectorJson(Position.X, Position.Y, Position.Z));
				return AddCanonicalObjectEntry(
					TEXT("asset.sample:result"),
					Asset->GetPathName(),
					TEXT("sample"),
					TEXT("asset.sample"),
					Sample,
					Context,
					Entries,
					OutError);
			}
			if (SampleKind == TEXT("section"))
			{
				auto* StaticMesh = Cast<UStaticMesh>(Asset);
				if (!StaticMesh)
					return Fail(OutError, ReadFailure(TEXT("asset_project_read_sample_target_unsupported"),
						TEXT("Section sampling requires a static mesh asset."), TEXT("/asset/type"),
						MakeShared<FJsonValueString>(Asset->GetClass()->GetPathName()),
						MakeShared<FJsonValueString>(TEXT("StaticMesh")), Asset->GetPathName()));
				TArray<UObject*> Objects{StaticMesh};
				FAssetCompilingManager::Get().FinishCompilationForObjects(Objects);
				const auto* RenderData = StaticMesh->GetRenderData();
				if (!RenderData || RenderData->LODResources.Num() == 0)
					return Fail(OutError, ReadCondition(TEXT("asset_project_read_sample_target_unsupported"),
						TEXT("The static mesh has no LOD 0 render data for section sampling."),
						TEXT("/mesh/lods/0/render_data_available"), Asset->GetPathName()));
				const int32 Index = Input->HasField(TEXT("index")) ? Input->GetIntegerField(TEXT("index")) : 0;
				const auto& Sections = RenderData->LODResources[0].Sections;
				if (!Sections.IsValidIndex(Index))
					return Fail(OutError, ReadFailure(TEXT("asset_project_read_sample_index_out_of_range"),
						TEXT("The requested section index is not valid in LOD 0."), TEXT("/index"),
						MakeShared<FJsonValueNumber>(Index), MakeShared<FJsonValueString>(TEXT("existing LOD 0 section index")), Asset->GetPathName()));
				const auto& Section = Sections[Index];
				const bool bValidMaterialIndex = StaticMesh->GetStaticMaterials().IsValidIndex(Section.MaterialIndex);
				const auto* Material = bValidMaterialIndex ? StaticMesh->GetMaterial(Section.MaterialIndex) : nullptr;
				const auto Sample = MakeShared<FJsonObject>();
				Sample->SetStringField(TEXT("kind"), SampleKind);
				Sample->SetNumberField(TEXT("index"), Index);
				Sample->SetNumberField(TEXT("section_count"), Sections.Num());
				Sample->SetNumberField(TEXT("material_slot_count"), StaticMesh->GetStaticMaterials().Num());
				Sample->SetNumberField(TEXT("material_index"), Section.MaterialIndex);
				Sample->SetBoolField(TEXT("material_index_valid"), bValidMaterialIndex);
				Sample->SetStringField(TEXT("material_path"), Material ? Material->GetPathName() : FString());
				Sample->SetNumberField(TEXT("triangle_count"), Section.NumTriangles);
				Sample->SetNumberField(TEXT("section_info_material_index"), StaticMesh->GetSectionInfoMap().Get(0, Index).MaterialIndex);
				Sample->SetNumberField(TEXT("original_section_info_material_index"), StaticMesh->GetOriginalSectionInfoMap().Get(0, Index).MaterialIndex);
				return AddCanonicalObjectEntry(TEXT("asset.sample:result"), Asset->GetPathName(),
					TEXT("sample"), TEXT("asset.sample"), Sample, Context, Entries, OutError);
			}
			return Fail(OutError, ReadFailure(TEXT("asset_project_read_sample_kind_unsupported"),
				TEXT("The requested asset sample kind is not supported."), TEXT("/sample_kind"),
				MakeShared<FJsonValueString>(SampleKind), MakeShared<FJsonValueString>(TEXT("bounds, vertex or section")),
				Asset->GetPathName(), TEXT("command_input")));
		}

		class FHookBase : public IUeAgentAssetProjectReadHook
		{
		public:
			FHookBase(
				FString InProviderBinding,
				TSharedRef<IUeAgentAssetProjectReadEnvironment>
					InEnvironment)
				: ProviderBinding(MoveTemp(InProviderBinding))
				, Environment(MoveTemp(InEnvironment))
			{
			}

		protected:
			bool ResolveAsset(
				const TOptional<FUeAgentCandidateBinding>& Target,
				UObject*& OutAsset,
				FUeAgentAssetProjectReadError& OutError) const
			{
				OutAsset = nullptr;
				if (!Target.IsSet())
				{
					return Fail(OutError, ReadCondition(TEXT("asset_project_read_target_required"),
						TEXT("This asset query requires a selected target."), TEXT("/target/present"), ProviderBinding, false, true, TEXT("command_input")));
				}
				if (!Environment->TryResolveAsset(
						Target.GetValue(),
						OutAsset,
						OutError))
				{
					return false;
				}
				return OutAsset
					? true
					: [&]()
					{
						OutError = ReadCondition(TEXT("asset_project_read_asset_unresolved"),
							TEXT("The native asset resolver returned no asset for the selected target."), TEXT("/asset/available"), Target.GetValue().GetBindingId());
						OutError.TargetPath = Target.GetValue().GetPublicRef();
						return false;
					}();
			}

			const FString ProviderBinding;
			const TSharedRef<IUeAgentAssetProjectReadEnvironment>
				Environment;
		};

		class FReferenceOriginHook final : public FHookBase
		{
		public:
			using FHookBase::FHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentAssetProjectReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				if (Target.IsSet())
				{
					return Fail(OutError, ReadCondition(TEXT("reference_origin_public_target_forbidden"),
						TEXT("Reference origin resolution accepts only input.target and does not accept a separate public target."),
						TEXT("/target/present"), ProviderBinding, true, false, TEXT("command_input")));
				}
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				const TSharedPtr<FJsonObject>* TargetObject = nullptr;
				FString ExplicitSource;
				if (!Input->TryGetObjectField(TEXT("target"), TargetObject)
					|| !TargetObject
					|| !TargetObject->IsValid()
					|| !(*TargetObject)->TryGetStringField(TEXT("source"), ExplicitSource)
					|| ExplicitSource.TrimStartAndEnd().IsEmpty())
				{
					const bool bObject = TargetObject && TargetObject->IsValid();
					const auto Value = bObject ? (*TargetObject)->TryGetField(TEXT("source")) : Input->TryGetField(TEXT("target"));
					return Fail(OutError, ReadFailure(TEXT("reference_origin_target_invalid"),
						TEXT("Reference origin resolution requires input.target.source to be a non-empty string."),
						bObject ? TEXT("/target/source") : TEXT("/target"),
						bObject && Value.IsValid() && Value->Type == EJson::String
							? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ExplicitSource)) : InputKind(Value),
						MakeShared<FJsonValueString>(bObject ? TEXT("non-empty string") : TEXT("object with a non-empty source")),
						ProviderBinding, TEXT("command_input")));
				}
				UeAgentOriginResolve::FResolveParams Params;
				UE::UAI::Contract::FDiagnostic ResolveError;
				if (!UeAgentOriginResolve::ParseResolveParams(
						Input,
						Params,
						ResolveError))
				{
					return Fail(OutError, ResolveError);
				}
				UeAgentOriginResolve::FResolveResult Result;
				if (!UeAgentOriginResolve::ResolveOrigin(
						Params,
						Result,
						ResolveError))
				{
					return Fail(OutError, ResolveError);
				}

				const FString OwnerId = TEXT("reference-origin:")
					+ Params.TargetSource;
				TArray<FUeAgentStateEntry> Entries;
				auto AddTargetString = [&](const TCHAR* Selector, const FString& Value)
				{
					return Value.IsEmpty()
						|| AddString(
							TEXT("reference.origin.target:") + FString(Selector),
							TEXT("reference_origin"),
							OwnerId,
							TEXT("target.") + FString(Selector),
							Value,
							Context,
							Entries,
							OutError);
				};
				if (!AddTargetString(TEXT("source"), Params.TargetSource)
					|| !AddTargetString(TEXT("candidate_id"), Params.CandidateId)
					|| !AddTargetString(TEXT("json_file"), Params.JsonFile)
					|| !AddTargetString(TEXT("json_pointer"), Params.JsonPointer)
					|| !AddTargetString(TEXT("kind"), Result.Kind.IsEmpty() ? Params.Kind : Result.Kind)
					|| !AddTargetString(TEXT("name"), Result.Name.IsEmpty() ? Params.Name : Result.Name)
					|| !AddTargetString(TEXT("full_name"), Result.FullName.IsEmpty() ? Params.FullName : Result.FullName)
					|| !AddTargetString(TEXT("asset_path"), Params.AssetPath)
					|| !AddTargetString(TEXT("graph_name"), Params.GraphName)
					|| !AddTargetString(TEXT("node_guid"), Params.NodeGuid)
					|| !AddStringArray(TEXT("reference.origin:asset_paths"), TEXT("reference_origin"), OwnerId, TEXT("origin.asset_paths"), Result.AssetPaths, Context, Entries, OutError)
					|| !AddStringArray(TEXT("reference.origin:package_paths"), TEXT("reference_origin"), OwnerId, TEXT("origin.package_paths"), Result.PackagePaths, Context, Entries, OutError)
					|| !AddStringArray(TEXT("reference.origin:package_files"), TEXT("reference_origin"), OwnerId, TEXT("origin.package_files"), Result.PackageFiles, Context, Entries, OutError))
				{
					return false;
				}

				const FString Status = Result.Status.IsEmpty()
					? FString(TEXT("resolved"))
					: Result.Status;
				if (!AddCodePaths(OwnerId, Result.CodePaths, Context, Entries, OutError)
					|| !AddBoolean(TEXT("reference.origin:resolved"), TEXT("reference_origin"), OwnerId, TEXT("resolved"), Result.bResolved, Context, Entries, OutError)
					|| !AddString(TEXT("reference.origin:status"), TEXT("reference_origin"), OwnerId, TEXT("status"), Status, Context, Entries, OutError)
					|| !AddStringArray(TEXT("reference.origin:warnings"), TEXT("reference_origin"), OwnerId, TEXT("warnings"), Result.Warnings, Context, Entries, OutError))
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

		class FAssetHook final : public FHookBase
		{
		public:
			using FHookBase::FHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentAssetProjectReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				TArray<FUeAgentStateEntry> Entries;
				UObject* Asset = nullptr;
				if (!ResolveAsset(Target, Asset, OutError))
				{
					if (ProviderBinding
							!= TEXT("asset_project.read.reference_asset")
						|| OutError.Code
							!= TEXT("asset_project_read_asset_unresolved"))
					{
						return false;
					}
					OutError = {};
					return BuildSnapshot(
						ProviderBinding,
						Context,
						MoveTemp(Entries),
						OutSnapshot,
						OutError);
				}
				if (ProviderBinding == TEXT("asset_project.read.asset_owned_candidates"))
				{
					FString Path, Query; UE::UAI::Contract::FDiagnostic Error;
					Input->TryGetStringField(TEXT("property_path"), Path);
					Input->TryGetStringField(TEXT("query"), Query);
					UeAgentAuthoringObjectApply::FNativeOwnedCreationContext Native;
					TArray<UeAgentAuthoringObjectApply::FOwnedClassCandidate> Candidates;
					if (!Native.Initialize(Asset, Path, true, Error) || !Native.Collect(Candidates, Error))
						return Fail(OutError, Error, Path);
					UE::UAI::ContentDiscovery::FContentDiscoveryRequest Search;
					Search.Query = Query;
					TArray<UE::UAI::ContentDiscovery::FNameSelectionEntry> SelectionEntries;
					TArray<TPair<int32, int32>> Matches;
					for (const auto& Candidate : Candidates)
					{
						const int32 Index = SelectionEntries.Add(Candidate.Selection);
						int32 Score = 0; FString Reason;
						if (UE::UAI::ContentDiscovery::TryScoreContentDiscoveryMatch(Search, Candidate.Selection.Identity,
							Candidate.Selection.Name, {}, Score, Reason)) Matches.Emplace(Score, Index);
					}
					Matches.StableSort([&SelectionEntries](const auto& A, const auto& B) {
						return A.Key == B.Key ? SelectionEntries[A.Value].Name < SelectionEntries[B.Value].Name : A.Key > B.Key;
					});
					TArray<int32> MatchedIndices;
					for (const auto& Match : Matches) MatchedIndices.Add(Match.Value);
					TArray<FString> Names;
					if (!UE::UAI::ContentDiscovery::TryBuildCandidateDiscoveryNamesForIndices(SelectionEntries, MatchedIndices,
						UE::UAI::ContentDiscovery::ENameComparison::CaseSensitive, Names, Error)) return Fail(OutError, Error, Path);
					if (!AddStringArray(TEXT("owned.candidates:names"), TEXT("asset"), TEXT("asset"), TEXT("names"), Names, Context, Entries, OutError)) return false;
					return BuildSnapshot(ProviderBinding, Context, MoveTemp(Entries), OutSnapshot, OutError);
				}
				if (!AddObjectIdentity(
						Asset,
						TEXT("asset"),
						Context,
						Entries,
						OutError))
				{
					return false;
				}

				if (ProviderBinding
						== TEXT("asset_project.read.asset_properties")
					|| ProviderBinding
						== TEXT("asset_project.read.reference_asset"))
				{
					if (!AddRequestedProperties(
							Asset,
							ReadStringArray(
								Input,
								TEXT("field_ids")),
							TEXT("asset.properties"),
							TEXT("asset"),
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				else if (ProviderBinding
					== TEXT("asset_project.read.asset_relations"))
				{
					FAssetData Record;
					if (!Target.IsSet()
						|| !Environment->TryResolveRegistryRecord(
							Target.GetValue(), Record, OutError)
						|| !Record.IsValid())
					{
						return OutError.Code.IsEmpty()
							? Fail(OutError, ReadCondition(TEXT("asset_project_read_registry_unresolved"),
								TEXT("No valid asset registry record was returned for relation traversal."),
								TEXT("/registry/record/available"), Asset->GetPathName()))
							: false;
					}
					FUeAgentAssetRelationGraphRequest Request;
					FUeAgentAssetRelationGraphResult Graph;
					if (!TryParseRelationGraphRequest(Input, Request, OutError))
					{
						if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Asset->GetPathName();
						return false;
					}
					if (!Environment->TryResolveRegistryRelations(
							Record,
							Request,
							Graph,
							OutError))
					{
						return false;
					}
					if (!AddRelationGraph(
							MoveTemp(Graph),
							Asset->GetPathName(),
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				else if (ProviderBinding
					== TEXT("asset_project.read.asset_validation"))
				{
					TArray<FString> RuleIds =
						ReadStringArray(Input, TEXT("rule_ids"));
					if (RuleIds.IsEmpty())
					{
						RuleIds = {TEXT("object.valid")};
					}
					bool bIncludePassed = false;
					Input->TryGetBoolField(
						TEXT("include_passed"), bIncludePassed);
					bool bOverallValid = true;
					for (const FString& RuleId : RuleIds)
					{
						bool bPassed = false;
						if (RuleId == TEXT("object.valid"))
						{
							bPassed = IsValid(Asset);
						}
						else if (RuleId == TEXT("package.present"))
						{
							bPassed = IsValid(Asset->GetOutermost());
						}
						else if (RuleId == TEXT("package.not_dirty"))
						{
							bPassed = IsValid(Asset->GetOutermost())
								&& !Asset->GetOutermost()->IsDirty();
						}
						else if (RuleId == TEXT("registry_record.present"))
						{
							FAssetData Record;
							FUeAgentAssetProjectReadError IgnoredError;
							bPassed = Target.IsSet()
								&& Environment->TryResolveRegistryRecord(
									Target.GetValue(), Record, IgnoredError)
								&& Record.IsValid();
						}
						else
						{
							return Fail(OutError, ReadFailure(TEXT("asset_project_read_validation_rule_unsupported"),
								TEXT("The requested asset validation rule is not supported."), TEXT("/rule_ids"),
								MakeShared<FJsonValueString>(RuleId), MakeShared<FJsonValueString>(TEXT("object.valid, package.present, package.not_dirty, registry_record.present")),
								Asset->GetPathName(), TEXT("command_input")));
						}
						bOverallValid = bOverallValid && bPassed;
						if (!bPassed || bIncludePassed)
						{
							const FString RuleOwner = Asset->GetPathName()
								+ TEXT("#validation:") + RuleId;
							if (!AddString(
									TEXT("asset.validation:rule:") + RuleId,
									TEXT("validation_rule"),
									RuleOwner,
									TEXT("validation_rule_id"),
									RuleId,
									Context,
									Entries,
									OutError)
								|| !AddBoolean(
									TEXT("asset.validation:passed:") + RuleId,
									TEXT("validation_rule"),
									RuleOwner,
									TEXT("validation_rule_passed"),
									bPassed,
									Context,
									Entries,
									OutError))
							{
								return false;
							}
						}
					}
					if (!AddBoolean(
							TEXT("asset.validation:valid"),
							TEXT("asset"),
							Asset->GetPathName(),
							TEXT("is_valid"),
							bOverallValid,
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				else if (ProviderBinding
					== TEXT("asset_project.read.asset_collection"))
				{
					FString CollectionKind;
					if (!Input->TryGetStringField(
							TEXT("collection_kind"), CollectionKind))
					{
						return Fail(OutError, ReadFailure(TEXT("asset_project_read_collection_kind_missing"),
							TEXT("Asset collection queries require a string collection_kind."), TEXT("/collection_kind"),
							InputKind(Input->TryGetField(TEXT("collection_kind"))), MakeShared<FJsonValueString>(TEXT("string")),
							Asset->GetPathName(), TEXT("command_input")));
					}
					const bool bCaptured = CollectionKind == TEXT("rows")
						? TryAddDataTableRows(
							Asset, Input, Context, Entries, OutError)
						: (CollectionKind == TEXT("items")
								|| CollectionKind == TEXT("entries"))
							? TryAddReflectedCollection(
								Asset, CollectionKind, Input,
								Context, Entries, OutError)
							: Fail(OutError, ReadFailure(TEXT("asset_project_read_collection_kind_unsupported"),
								TEXT("The requested asset collection kind is not supported."), TEXT("/collection_kind"),
								MakeShared<FJsonValueString>(CollectionKind), MakeShared<FJsonValueString>(TEXT("rows, items or entries")),
								Asset->GetPathName(), TEXT("command_input")));
					if (!bCaptured)
					{
						return false;
					}
				}
				else if (ProviderBinding
					== TEXT("asset_project.read.asset_sample"))
				{
					if (!TryAddAssetSample(
							Asset, Input, Context, Entries, OutError))
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

		class FRegistryHook final : public FHookBase
		{
		public:
			using FHookBase::FHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentAssetProjectReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				if (!Target.IsSet())
				{
					return Fail(OutError, ReadCondition(TEXT("asset_project_read_target_required"),
						TEXT("Registry queries require a selected target."), TEXT("/target/present"), ProviderBinding, false, true, TEXT("command_input")));
				}
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				FAssetData Record;
				if (!Environment->TryResolveRegistryRecord(
						Target.GetValue(),
						Record,
						OutError)
					|| !Record.IsValid())
				{
					return OutError.Code.IsEmpty()
						? Fail(OutError, ReadCondition(TEXT("asset_project_read_registry_unresolved"),
							TEXT("The native registry resolver returned no valid record for the selected asset."),
							TEXT("/registry/record/available"), Target.GetValue().GetBindingId()))
						: false;
				}
				const FString OwnerId = Record.GetObjectPathString();
				TArray<FUeAgentStateEntry> Entries;
				if (!AddString(
						TEXT("asset.registry:object_path"),
						TEXT("asset"),
						OwnerId,
						TEXT("object_path"),
						OwnerId,
						Context,
						Entries,
						OutError)
					|| !AddString(
						TEXT("asset.registry:class"),
						TEXT("asset"),
						OwnerId,
						TEXT("class_path"),
						Record.AssetClassPath.ToString(),
						Context,
						Entries,
						OutError)
					|| !AddString(
						TEXT("asset.registry:package"),
						TEXT("asset"),
						OwnerId,
						TEXT("package_name"),
						Record.PackageName.ToString(),
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				if (ProviderBinding
					== TEXT("asset_project.read.registry_record"))
				{
					bool bIncludeTags = false;
					bool bIncludeChunkIds = false;
					Input->TryGetBoolField(TEXT("include_tags"), bIncludeTags);
					Input->TryGetBoolField(TEXT("include_chunk_ids"), bIncludeChunkIds);
					if (bIncludeTags)
					{
						const TSharedPtr<FJsonObject> Tags = MakeShared<FJsonObject>();
						for (const auto Tag : Record.TagsAndValues)
						{
							Tags->SetStringField(Tag.Key.ToString(), Tag.Value.AsString());
						}
						if (!AddCanonicalObjectEntry(TEXT("asset.registry:tags"), OwnerId,
							TEXT("tags"), TEXT("asset.registry.tags"), Tags,
							Context, Entries, OutError)) return false;
					}
					if (bIncludeChunkIds)
					{
						TArray<TSharedPtr<FJsonValue>> ChunkIds;
						for (const int32 ChunkId : Record.GetChunkIDs())
						{
							ChunkIds.Add(MakeShared<FJsonValueNumber>(ChunkId));
						}
						if (!AddCanonicalJsonEntry(TEXT("asset.registry:chunk_ids"), OwnerId,
							TEXT("chunk_ids"), TEXT("core.integer_array"),
							MakeShared<FJsonValueArray>(ChunkIds), Context, Entries, OutError)) return false;
					}
				}
				if (ProviderBinding
					== TEXT(
						"asset_project.read.registry_relations"))
				{
					FUeAgentAssetRelationGraphRequest Request;
					FUeAgentAssetRelationGraphResult Graph;
					if (!TryParseRelationGraphRequest(Input, Request, OutError))
					{
						if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = OwnerId;
						return false;
					}
					if (!Environment->TryResolveRegistryRelations(
							Record,
							Request,
							Graph,
							OutError))
					{
						return false;
					}
					if (!AddString(
							TEXT("asset.registry:direction"),
							TEXT("asset"),
							OwnerId,
							TEXT("direction"),
							RelationDirectionText(Request.Direction),
							Context,
							Entries,
							OutError))
					{
						return false;
					}
					TSet<FString> Dependencies;
					TSet<FString> Referencers;
					for (const FUeAgentAssetRelationEdge& Edge : Graph.Edges)
					{
						if (Edge.Depth != 1)
						{
							continue;
						}
						if (Edge.TraversalDirection == TEXT("dependencies"))
						{
							Dependencies.Add(
								Edge.TargetPackageName.IsEmpty()
									? Edge.TargetIdentifier
									: Edge.TargetPackageName);
						}
						else if (Edge.TraversalDirection == TEXT("referencers"))
						{
							Referencers.Add(
								Edge.SourcePackageName.IsEmpty()
									? Edge.SourceIdentifier
									: Edge.SourcePackageName);
						}
					}
					TArray<FString> SortedDependencies = Dependencies.Array();
					TArray<FString> SortedReferencers = Referencers.Array();
					SortedDependencies.Sort();
					SortedReferencers.Sort();
					for (const FString& Dependency : SortedDependencies)
					{
						if (!AddString(
								TEXT("asset.registry:dependency:") + Dependency,
								TEXT("asset"),
								OwnerId,
								TEXT("dependency"),
								Dependency,
								Context,
								Entries,
								OutError))
						{
							return false;
						}
					}
					for (const FString& Referencer : SortedReferencers)
					{
						if (!AddString(
								TEXT("asset.registry:referencer:") + Referencer,
								TEXT("asset"),
								OwnerId,
								TEXT("referencer"),
								Referencer,
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

		bool TryCaptureEditorProjectCapability(
			const TSharedPtr<FJsonObject>& Input,
			const FString& ProviderBinding,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			bool& bOutHandled,
			FUeAgentAssetProjectReadError& OutError)
		{
			bOutHandled = false;
			FString Action;
			if (!Input.IsValid()
				|| !Input->TryGetStringField(
					TEXT("editor_project_capability"), Action))
			{
				return true;
			}
			bOutHandled = true;
			if (!EditorProject::IsEditorProjectQueryRoute(
					ProviderBinding, Action))
			{
				return Fail(OutError, ReadFailure(TEXT("editor_project_query_route_mismatch"),
					TEXT("The requested editor/project action is not registered for this query route."), TEXT("/editor_project_capability"),
					MakeShared<FJsonValueString>(Action), MakeShared<FJsonValueString>(TEXT("action registered for this query route")),
					ProviderBinding, TEXT("command_input")));
			}

			TSharedPtr<FJsonObject> Parameters = MakeShared<FJsonObject>();
			const TSharedPtr<FJsonObject>* SuppliedParameters = nullptr;
			if (Input->TryGetObjectField(
					TEXT("parameters"), SuppliedParameters)
				&& SuppliedParameters
				&& SuppliedParameters->IsValid())
			{
				Parameters = *SuppliedParameters;
			}

			static const TSharedRef<const EditorProject::
				IEditorProjectQueryMechanicsPort> Port =
				EditorProject::CreateEditorProjectQueryMechanicsPort();
			TSharedPtr<FJsonObject> Observation;
			UE::UAI::Contract::FDiagnostic NativeError;
			if (!Port->Execute(
					Action,
					{Context.GetRequestId(), Parameters},
					Observation,
					NativeError)
				|| !Observation.IsValid())
			{
				auto Error = NativeError.IsValid() ? MoveTemp(NativeError) : ReadCondition(
					TEXT("editor_project_query_observation_invalid"), TEXT("The editor/project query returned no observation."),
					TEXT("/observation/available"), ProviderBinding);
				if (Error.CandidateName.IsEmpty()) Error.CandidateName = Action;
				return Fail(OutError, MoveTemp(Error));
			}

			FString ObservationJson;
			const TSharedRef<TJsonWriter<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>::Create(
						&ObservationJson);
			if (!FJsonSerializer::Serialize(
					Observation.ToSharedRef(), Writer))
			{
				auto Error = ReadCondition(TEXT("editor_project_query_observation_invalid"),
					TEXT("The editor/project observation could not be serialized as JSON."),
					TEXT("/observation/serialization_succeeded"), ProviderBinding);
				Error.CandidateName = Action;
				return Fail(OutError, MoveTemp(Error));
			}
			return AddString(
				TEXT("editor_project.capability:") + Action,
				TEXT("editor_project"),
				Context.GetExecutionContext().GetProjectInstanceId(),
				TEXT("observation_json"),
				ObservationJson,
				Context,
				OutEntries,
				OutError);
		}

		class FEditorHook final : public FHookBase
		{
		public:
			using FHookBase::FHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentAssetProjectReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				TArray<FUeAgentStateEntry> Entries;
				bool bHandled = false;
				if (!TryCaptureEditorProjectCapability(
						Input,
						ProviderBinding,
						Context,
						Entries,
						bHandled,
						OutError))
				{
					return false;
				}
				if (bHandled)
				{
					return BuildSnapshot(
						ProviderBinding,
						Context,
						MoveTemp(Entries),
						OutSnapshot,
						OutError);
				}
				TArray<FUeAgentEditorResourceObservation> Resources;
				bool bIncludeEditorIdentity = false;
				Input->TryGetBoolField(
					TEXT("include_editor_identity"),
					bIncludeEditorIdentity);
				const bool bResolved = ProviderBinding
						== TEXT("asset_project.read.editor_open_assets")
					? Environment->TryResolveOpenAssets(
						Context.GetExecutionContext(),
						bIncludeEditorIdentity,
						Resources,
						OutError)
					: Environment->TryResolveDirtyResources(
						Context.GetExecutionContext(),
						ReadStringArray(Input, TEXT("resource_kinds")),
						Resources,
						OutError);
				if (!bResolved)
				{
					return false;
				}
				for (int32 ResourceIndex = 0; ResourceIndex < Resources.Num(); ++ResourceIndex)
				{
					const auto& Resource = Resources[ResourceIndex];
					ON_SCOPE_EXIT
					{
						if (OutError.IsValid() && OutError.TargetPath.IsEmpty() && !Resource.ResourceId.IsEmpty())
							OutError.TargetPath = Resource.ResourceId;
					};
					if (Resource.ResourceId.IsEmpty()
						|| Resource.ResourceKind.IsEmpty())
					{
						auto Error = ReadCondition(TEXT("asset_project_read_editor_resource_invalid"),
							Resource.ResourceId.IsEmpty() ? TEXT("An editor resource observation has no public resource reference.")
								: TEXT("An editor resource observation has no resource kind."),
							TEXT("/resources/") + FString::FromInt(ResourceIndex)
								+ (Resource.ResourceId.IsEmpty() ? TEXT("/resource_ref/present") : TEXT("/resource_kind/present")), ProviderBinding);
						Error.ActualEvidence = MakeShared<FJsonValueString>(Resource.ResourceId);
						return Fail(OutError, MoveTemp(Error));
					}
					const FString& OwnerId = Resource.ResourceId;
					if (!AddString(
							TEXT("editor.resource:ref:")
								+ OwnerId,
							TEXT("editor_resource"),
							OwnerId,
							TEXT("resource_ref"),
							OwnerId,
							Context,
							Entries,
							OutError)
						|| !AddString(
							TEXT("editor.resource:kind:") + OwnerId,
							TEXT("editor_resource"),
							OwnerId,
							TEXT("resource_kind"),
							Resource.ResourceKind,
							Context,
							Entries,
							OutError)
						|| !AddBoolean(
							TEXT("editor.resource:dirty:")
								+ OwnerId,
							TEXT("editor_resource"),
							OwnerId,
							TEXT("resource_dirty"),
							Resource.bDirty,
							Context,
							Entries,
							OutError))
					{
						return false;
					}
					auto AddOptional = [&](const TCHAR* Semantic,
						const TCHAR* Selector, const FString& Value)
					{
						return Value.IsEmpty()
							|| AddString(
								FString(Semantic) + OwnerId,
								TEXT("editor_resource"),
								OwnerId,
								Selector,
								Value,
								Context,
								Entries,
								OutError);
					};
					if (!AddOptional(
							TEXT("editor.resource:package:"),
							TEXT("package_name"),
							Resource.PackageName)
						|| !AddOptional(
							TEXT("editor.resource:object:"),
							TEXT("object_path"),
							Resource.ObjectPath)
						|| !AddOptional(
							TEXT("editor.resource:class:"),
							TEXT("class_path"),
							Resource.ClassPath)
						|| !AddOptional(
							TEXT("editor.resource:editor:"),
							TEXT("editor_identity"),
							Resource.EditorIdentity))
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

		class FProjectHook final : public FHookBase
		{
		public:
			using FHookBase::FHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentAssetProjectReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				ON_SCOPE_EXIT
				{
					if (OutError.IsValid() && OutError.TargetPath.IsEmpty()) OutError.TargetPath = TEXT("project");
				};
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				TArray<FUeAgentStateEntry> Entries;
				bool bHandled = false;
				if (!TryCaptureEditorProjectCapability(
						Input,
						ProviderBinding,
						Context,
						Entries,
						bHandled,
						OutError))
				{
					return false;
				}
				if (bHandled)
				{
					return BuildSnapshot(
						ProviderBinding,
						Context,
						MoveTemp(Entries),
						OutSnapshot,
						OutError);
				}
				if (ProviderBinding
					== TEXT("asset_project.read.project_platform"))
				{
					const TArray<FString> PlatformIds =
						ReadStringArray(Input, TEXT("platform_ids"));
					const TArray<FString> FieldIds =
						ReadStringArray(Input, TEXT("field_ids"));
					static const TArray<FString> SupportedFields = {
						TEXT("platform_name"),
						TEXT("ini_platform_name"),
						TEXT("display_name"),
						TEXT("running_platform"),
						TEXT("sdk_installed"),
						TEXT("sdk_documentation_path"),
					};
					for (const FString& FieldId : FieldIds)
					{
						if (!SupportedFields.Contains(FieldId))
						{
							return Fail(OutError, ReadFailure(TEXT("asset_project_read_platform_field_unsupported"),
								TEXT("The requested platform field is not supported."), TEXT("/field_ids"),
								MakeShared<FJsonValueString>(FieldId), MakeShared<FJsonValueString>(FString::Join(SupportedFields, TEXT(", "))),
								FString(), TEXT("command_input")));
						}
					}
					TArray<FUeAgentProjectPlatformObservation> Platforms;
					if (!Environment->TryResolveProjectPlatforms(
							Context.GetExecutionContext(),
							PlatformIds,
							Platforms,
							OutError))
					{
						return false;
					}
					bool bResolveInheritance = false;
					Input->TryGetBoolField(
						TEXT("resolve_inheritance"), bResolveInheritance);
					auto Includes = [&FieldIds](const TCHAR* Field)
					{
						return FStringView(Field) == TEXTVIEW("platform_name")
							|| FieldIds.Contains(Field);
					};
					for (int32 PlatformIndex = 0; PlatformIndex < Platforms.Num(); ++PlatformIndex)
					{
						const auto& Platform = Platforms[PlatformIndex];
						if (Platform.PlatformName.IsEmpty())
						{
							return Fail(OutError, ReadCondition(TEXT("asset_project_read_platform_observation_invalid"),
								TEXT("A native platform observation has no platform name."),
								TEXT("/platforms/") + FString::FromInt(PlatformIndex) + TEXT("/platform_name/present"),
								Context.GetExecutionContext().GetProjectInstanceId()));
						}
						const FString Owner = TEXT("platform:")
							+ Platform.PlatformName;
						ON_SCOPE_EXIT
						{
							if (OutError.IsValid() && OutError.TargetPath.IsEmpty()) OutError.TargetPath = Owner;
						};
						if (!AddBoolean(
								TEXT("project.platform:resolve_inheritance:") + Owner,
								TEXT("target_platform"), Owner,
								TEXT("resolve_inheritance"), bResolveInheritance,
								Context, Entries, OutError))
						{
							return false;
						}
						auto AddSelectedString = [&](const TCHAR* Field,
							const FString& Value)
						{
							return !Includes(Field)
								|| AddString(
									FString(TEXT("project.platform:"))
										+ Field + TEXT(":") + Owner,
									TEXT("target_platform"), Owner,
									Field, Value,
									Context, Entries, OutError);
						};
						auto AddSelectedBoolean = [&](const TCHAR* Field,
							const bool Value)
						{
							return !Includes(Field)
								|| AddBoolean(
									FString(TEXT("project.platform:"))
										+ Field + TEXT(":") + Owner,
									TEXT("target_platform"), Owner,
									Field, Value,
									Context, Entries, OutError);
						};
						if (!AddSelectedString(
								TEXT("platform_name"), Platform.PlatformName)
							|| !AddSelectedString(
								TEXT("ini_platform_name"), Platform.IniPlatformName)
							|| !AddSelectedString(
								TEXT("display_name"), Platform.DisplayName)
							|| !AddSelectedBoolean(
								TEXT("running_platform"), Platform.bRunningPlatform)
							|| !AddSelectedBoolean(
								TEXT("sdk_installed"), Platform.bSdkInstalled)
							|| !AddSelectedString(
								TEXT("sdk_documentation_path"),
								Platform.SdkDocumentationPath))
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
				UObject* ProjectObject = nullptr;
				const bool bProjectSettings = ProviderBinding == TEXT("asset_project.read.project_settings");
				FString SettingsSectionId;
				if (bProjectSettings) Input->TryGetStringField(TEXT("section_id"), SettingsSectionId);
				const bool bResolved = bProjectSettings
					? Environment->TryResolveProjectSettingsObject(
						Context.GetExecutionContext(),
						SettingsSectionId,
						ProjectObject, OutError)
					: Environment->TryResolveProjectObject(
						Context.GetExecutionContext(),
						TEXT("project"),
						ProjectObject,
						OutError);
				if (!bResolved || !ProjectObject)
				{
					return OutError.Code.IsEmpty()
						? [&]()
						{
							OutError = ReadCondition(TEXT("asset_project_read_project_unresolved"),
								bProjectSettings ? TEXT("The native resolver returned no object for the selected settings section.")
									: TEXT("The native resolver returned no general project settings object."),
								bProjectSettings ? TEXT("/settings/object/available") : TEXT("/project/object/available"),
								Context.GetExecutionContext().GetProjectInstanceId());
							OutError.CandidateName = bProjectSettings ? SettingsSectionId : TEXT("project");
							return false;
						}()
						: false;
				}
				if (!AddObjectIdentity(
						ProjectObject,
						TEXT("project"),
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				if (ProviderBinding
					== TEXT("asset_project.read.project_settings"))
				{
					const TArray<FString> FieldIds = ReadStringArray(Input, TEXT("field_ids"));
					if (!AddRequestedProperties(
							ProjectObject,
							FieldIds,
							TEXT("project.settings"),
							TEXT("project"),
							Context,
							Entries,
							OutError))
					{
						return false;
					}
					bool bIncludeDefaults = false;
					Input->TryGetBoolField(TEXT("include_defaults"), bIncludeDefaults);
					if (bIncludeDefaults && !FieldIds.IsEmpty())
					{
						TSharedRef<FJsonObject> Defaults = MakeShared<FJsonObject>();
						UeAgentUObjectProperties::FExportOptions Options;
						Options.Profile = UeAgentUObjectProperties::EExportProfile::Readback;
						for (const FString& FieldId : FieldIds)
						{
							FProperty* Property = FindFProperty<FProperty>(ProjectObject->GetClass(), *FieldId);
							const TSharedPtr<FJsonObject> Descriptor = UeAgentUObjectProperties::BuildPropertyDescriptor(ProjectObject, Property, Options);
							FString DefaultText;
							Defaults->SetField(FieldId, Descriptor.IsValid() && Descriptor->TryGetStringField(TEXT("default_value_text"), DefaultText)
								? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(DefaultText))
								: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()));
						}
						FString DefaultsJson;
						const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&DefaultsJson);
						if (!FJsonSerializer::Serialize(Defaults, Writer)
							|| !AddString(TEXT("project.settings:defaults"), TEXT("project"), ProjectObject->GetPathName(),
								TEXT("defaults"), DefaultsJson, Context, Entries, OutError)) return false;
					}
				}
				else if (ProviderBinding
					== TEXT("asset_project.read.project_validation"))
				{
					const TArray<FString> SectionIds =
						ReadStringArray(Input, TEXT("section_ids"));
					for (const FString& SectionId : SectionIds)
					{
						if (SectionId != TEXT("project"))
						{
							return Fail(OutError, ReadFailure(TEXT("asset_project_read_validation_section_unsupported"),
								TEXT("The requested project validation section is not supported."), TEXT("/section_ids"),
								MakeShared<FJsonValueString>(SectionId), MakeShared<FJsonValueString>(TEXT("project")),
								FString(), TEXT("command_input")));
						}
					}
					TArray<FString> RuleIds =
						ReadStringArray(Input, TEXT("rule_ids"));
					if (RuleIds.IsEmpty())
					{
						RuleIds = {TEXT("project_object.valid")};
					}
					bool bIncludePassed = false;
					Input->TryGetBoolField(
						TEXT("include_passed"), bIncludePassed);
					bool bOverallValid = true;
					for (const FString& RuleId : RuleIds)
					{
						if (RuleId != TEXT("project_object.valid"))
						{
							return Fail(OutError, ReadFailure(TEXT("asset_project_read_validation_rule_unsupported"),
								TEXT("The requested project validation rule is not supported."), TEXT("/rule_ids"),
								MakeShared<FJsonValueString>(RuleId), MakeShared<FJsonValueString>(TEXT("project_object.valid")),
								ProjectObject->GetPathName(), TEXT("command_input")));
						}
						const bool bPassed = IsValid(ProjectObject);
						bOverallValid = bOverallValid && bPassed;
						if (!bPassed || bIncludePassed)
						{
							const FString RuleOwner = ProjectObject->GetPathName()
								+ TEXT("#validation:") + RuleId;
							if (!AddString(
									TEXT("project.validation:rule:") + RuleId,
									TEXT("validation_rule"), RuleOwner,
									TEXT("validation_rule_id"), RuleId,
									Context, Entries, OutError)
								|| !AddBoolean(
									TEXT("project.validation:passed:") + RuleId,
									TEXT("validation_rule"), RuleOwner,
									TEXT("validation_rule_passed"), bPassed,
									Context, Entries, OutError))
							{
								return false;
							}
						}
					}
					if (!AddBoolean(
							TEXT("project.validation:valid"),
							TEXT("project"),
							ProjectObject->GetPathName(),
							TEXT("is_valid"),
							bOverallValid,
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

		class FConfigHook final : public FHookBase
		{
		public:
			using FHookBase::FHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentAssetProjectReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				TArray<FUeAgentStateEntry> Entries;
				bool bHandled = false;
				if (!TryCaptureEditorProjectCapability(
						Input,
						ProviderBinding,
						Context,
						Entries,
						bHandled,
						OutError))
				{
					return false;
				}
				if (bHandled)
				{
					return BuildSnapshot(
						ProviderBinding,
						Context,
						MoveTemp(Entries),
						OutSnapshot,
						OutError);
				}
				FString ConfigScope;
				FString SectionId;
				if (!Input->TryGetStringField(
						TEXT("config_scope"),
						ConfigScope)
					|| !Input->TryGetStringField(
						TEXT("section_id"),
						SectionId))
				{
					const auto ScopeValue = Input->TryGetField(TEXT("config_scope"));
					const bool bScopeString = ScopeValue.IsValid() && ScopeValue->Type == EJson::String;
					return Fail(OutError, ReadFailure(TEXT("asset_project_read_config_input_missing"),
						TEXT("Configuration queries require string config_scope and section_id selectors."),
						bScopeString ? TEXT("/section_id") : TEXT("/config_scope"),
						InputKind(bScopeString ? Input->TryGetField(TEXT("section_id")) : ScopeValue),
						MakeShared<FJsonValueString>(TEXT("string")), ProviderBinding, TEXT("command_input")));
				}
				const FConfigFile* Config = nullptr;
				if (!Environment->TryResolveConfig(
						Context.GetExecutionContext(),
						ConfigScope,
						Config,
						OutError)
					|| !Config)
				{
					return OutError.Code.IsEmpty()
						? [&]()
						{
							OutError = ReadCondition(TEXT("asset_project_read_config_unresolved"),
								TEXT("The native configuration resolver returned no file for the requested scope."),
								TEXT("/config/file/available"), ProviderBinding);
							OutError.CandidateName = ConfigScope;
							return false;
						}()
						: false;
				}
				ON_SCOPE_EXIT
				{
					if (OutError.IsValid())
					{
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TEXT("config:")
							+ (ConfigScope == TEXT("platform") ? FString(TEXT("game")) : ConfigScope);
						if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = SectionId;
					}
				};
				const FString OwnerId =
					Context.GetExecutionContext().GetProjectInstanceId()
					+ TEXT(":config:")
					+ ConfigScope
					+ TEXT(":")
					+ SectionId;
				if (!AddString(
						TEXT("config.section:id"),
						TEXT("config"),
						OwnerId,
						TEXT("section_id"),
						SectionId,
						Context,
						Entries,
						OutError))
				{
					return false;
				}

				if (ProviderBinding
					== TEXT(
						"asset_project.read.config_resolved_value"))
				{
					FString Key;
					if (!Input->TryGetStringField(TEXT("key"), Key))
					{
						return Fail(OutError, ReadFailure(TEXT("asset_project_read_config_key_missing"),
							TEXT("A resolved configuration value query requires a string key."), TEXT("/key"),
							InputKind(Input->TryGetField(TEXT("key"))), MakeShared<FJsonValueString>(TEXT("string")),
							SectionId, TEXT("command_input")));
					}
					FString Value;
					if (!Config->GetString(
							*SectionId,
							*Key,
							Value))
					{
						auto Error = ReadCondition(TEXT("asset_project_read_config_value_missing"),
							TEXT("The selected configuration section has no resolved string value for the requested key."),
							TEXT("/sections/") + PointerPart(SectionId) + TEXT("/keys/") + PointerPart(Key) + TEXT("/present"), OwnerId);
						Error.CandidateName = ConfigScope;
						return Fail(OutError, MoveTemp(Error));
					}
					if (!AddString(
							TEXT("config.value:") + Key,
							TEXT("config"),
							OwnerId,
							Key,
							Value,
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				else
				{
					const FConfigSection* Section =
						Config->FindSection(SectionId);
					if (!Section)
					{
						auto Error = ReadCondition(TEXT("asset_project_read_config_section_missing"),
							TEXT("The requested configuration section is absent from the resolved configuration."),
							TEXT("/sections/") + PointerPart(SectionId) + TEXT("/present"), OwnerId);
						Error.CandidateName = ConfigScope;
						return Fail(OutError, MoveTemp(Error));
					}
					TSet<FString> RequestedKeys;
					for (const FString& Key
						: ReadStringArray(Input, TEXT("keys")))
					{
						RequestedKeys.Add(Key);
					}
					if (RequestedKeys.IsEmpty())
					{
						TSet<FString> UniqueKeys;
						for (FConfigSection::TConstIterator It(*Section);
							It;
							++It)
						{
							UniqueKeys.Add(It.Key().ToString());
						}
						TArray<FString> KeyNames = UniqueKeys.Array();
						KeyNames.Sort();
						if (!AddStringArray(
								TEXT("config.section:keys"),
								TEXT("config"),
								OwnerId,
								TEXT("keys"),
								KeyNames,
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
					TMap<FString, TArray<FString>> ValuesByKey;
					for (FConfigSection::TConstIterator It(*Section);
						It;
						++It)
					{
						const FString Key = It.Key().ToString();
						if (!RequestedKeys.Contains(Key))
						{
							continue;
						}
						ValuesByKey.FindOrAdd(Key).Add(
							It.Value().GetSavedValue());
					}
					TArray<FString> ValueKeys;
					ValuesByKey.GetKeys(ValueKeys);
					ValueKeys.Sort();
					for (const FString& Key : ValueKeys)
					{
						const TArray<FString>& Values =
							ValuesByKey.FindChecked(Key);
						const bool bAdded = Values.Num() == 1
							? AddString(
								TEXT("config.value:") + Key,
								TEXT("config"),
								OwnerId,
								Key,
								Values[0],
								Context,
								Entries,
								OutError)
							: AddStringArray(
								TEXT("config.value:") + Key,
								TEXT("config"),
								OwnerId,
								Key,
								Values,
								Context,
								Entries,
								OutError);
						if (!bAdded)
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

	bool FUeAgentAssetProjectReadNativeHookFactory::TryCreate(
		const FString& ProviderBinding,
		TSharedRef<IUeAgentAssetProjectReadEnvironment> Environment,
		TSharedPtr<IUeAgentAssetProjectReadHook>& OutHook,
		FUeAgentAssetProjectReadError& OutError)
	{
		OutHook.Reset();
		OutError = {};
		if (IsOneOf(
				ProviderBinding,
				{
					TEXT("asset_project.read.reference_origin"),
				}))
		{
			OutHook = MakeShared<FReferenceOriginHook>(
				ProviderBinding,
				MoveTemp(Environment));
		}
		else if (IsOneOf(
				ProviderBinding,
				{
					TEXT("asset_project.read.asset_summary"),
					TEXT("asset_project.read.asset_owned_candidates"),
					TEXT("asset_project.read.asset_properties"),
					TEXT("asset_project.read.reference_asset"),
					TEXT("asset_project.read.asset_relations"),
					TEXT("asset_project.read.asset_validation"),
					TEXT("asset_project.read.asset_collection"),
					TEXT("asset_project.read.asset_sample"),
				}))
		{
			OutHook = MakeShared<FAssetHook>(
				ProviderBinding,
				MoveTemp(Environment));
		}
		else if (IsOneOf(
			ProviderBinding,
				{
					TEXT("asset_project.read.registry_record"),
					TEXT("asset_project.read.registry_relations"),
				}))
		{
			OutHook = MakeShared<FRegistryHook>(
				ProviderBinding,
				MoveTemp(Environment));
		}
		else if (IsOneOf(
			ProviderBinding,
				{
					TEXT("asset_project.read.editor_open_assets"),
					TEXT("asset_project.read.editor_dirty_resources"),
				}))
		{
			OutHook = MakeShared<FEditorHook>(
				ProviderBinding,
				MoveTemp(Environment));
		}
		else if (IsOneOf(
			ProviderBinding,
				{
					TEXT("asset_project.read.project_summary"),
					TEXT("asset_project.read.project_settings"),
					TEXT("asset_project.read.project_platform"),
					TEXT("asset_project.read.project_validation"),
				}))
		{
			OutHook = MakeShared<FProjectHook>(
				ProviderBinding,
				MoveTemp(Environment));
		}
		else if (IsOneOf(
			ProviderBinding,
				{
					TEXT("asset_project.read.config_section"),
					TEXT(
						"asset_project.read.config_resolved_value"),
				}))
		{
			OutHook = MakeShared<FConfigHook>(
				ProviderBinding,
				MoveTemp(Environment));
		}
		else
		{
			return Fail(OutError, ReadCondition(TEXT("asset_project_read_provider_unknown"),
				TEXT("No native asset/project query hook is registered for the requested provider."),
				TEXT("/provider/registered"), ProviderBinding));
		}
		return true;
	}
}
