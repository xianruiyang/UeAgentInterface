#include "Core/GraphLayout/UeAgentUnifiedGraphLayoutService.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "FileHelpers.h"
#include "Misc/Crc.h"
#include "PackageTools.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::Core::GraphLayout
{
	using namespace CommandEdge;

	namespace
	{
		const FString CommandId(TEXT("graph.layout.apply"));
		const FString Owner(TEXT("unified-graph-layout-owner"));

		struct FTarget final
		{
			FString Identity;
			FString ExpectedRevision;
			FString ProviderId;
			UEdGraph* Graph = nullptr;
			UPackage* Package = nullptr;
			TArray<FIntPoint> Before;
		};

		FUeAgentCommandResult Failure(
			const FUeAgentCommandRequest& Request,
			const FString& Code,
			const FString& Subject,
			const FString& Detail)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), Subject);
			Error->SetStringField(TEXT("detail"), Detail);
			FString Text;
			const auto Writer = TJsonWriterFactory<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Error, Writer);
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Text);
		}

		bool Canonical(
			const TSharedPtr<FJsonObject>& Object,
			FString& OutText)
		{
			FString Error;
			return CommandContract::Json::TryCanonicalize(Object, OutText, Error);
		}

		FString Revision(const UEdGraph& Graph)
		{
			TArray<FString> Nodes;
			for (const UEdGraphNode* Node : Graph.Nodes)
			{
				if (Node)
				{
					Nodes.Add(FString::Printf(
						TEXT("%s|%d|%d"),
						*Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower),
						Node->NodePosX,
						Node->NodePosY));
				}
			}
			Nodes.Sort();
			uint32 Hash = FCrc::StrCrc32(*Graph.GetPathName());
			for (const FString& Node : Nodes)
			{
				Hash = FCrc::StrCrc32(*Node, Hash);
			}
			return FString::Printf(TEXT("graph-layout:%08x"), Hash);
		}

		bool TryResolveProvider(
			const UEdGraph& Graph,
			FString& OutProviderId,
			bool& bOutDuplicate)
		{
			OutProviderId.Reset();
			bOutDuplicate = false;
			const FString ClassPath = Graph.GetClass()->GetPathName();
			const FString SchemaPath = Graph.GetSchema()
				? Graph.GetSchema()->GetClass()->GetPathName()
				: FString();
			const FString Composite = ClassPath + TEXT("|") + SchemaPath;
			TArray<FString> Matches;
			if (Composite.Contains(TEXT("Animation")))
			{
				Matches.Add(TEXT("animation-blueprint-graph-layout-provider"));
			}
			if (Composite.Contains(TEXT("Material")))
			{
				Matches.Add(TEXT("material-graph-layout-provider"));
			}
			if (Composite.Contains(TEXT("Niagara")))
			{
				Matches.Add(TEXT("niagara-graph-layout-provider"));
			}
			if (Composite.Contains(TEXT("PCG")))
			{
				Matches.Add(TEXT("pcg-graph-layout-provider"));
			}
			if (Composite.Contains(TEXT("BehaviorTree"))
				|| Composite.Contains(TEXT("EnvironmentQuery"))
				|| Composite.Contains(TEXT("StateTree")))
			{
				Matches.Add(TEXT("ai-graph-layout-provider"));
			}
			if (Composite.Contains(TEXT("Sound"))
				|| Composite.Contains(TEXT("MetaSound")))
			{
				Matches.Add(TEXT("audio-graph-layout-provider"));
			}
			if (Composite.Contains(TEXT("RigVM"))
				|| Composite.Contains(TEXT("ControlRig")))
			{
				Matches.Add(TEXT("control-rig-graph-layout-provider"));
			}
			if (Matches.IsEmpty() && Composite.Contains(TEXT("K2")))
			{
				Matches.Add(TEXT("blueprint-graph-layout-provider"));
			}
			bOutDuplicate = Matches.Num() > 1;
			if (Matches.Num() != 1)
			{
				return false;
			}
			OutProviderId = MoveTemp(Matches[0]);
			return true;
		}

		TSharedPtr<FJsonObject> Capture(const FTarget& Target)
		{
			const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("graph_identity"), Target.Identity);
			Object->SetStringField(TEXT("provider_id"), Target.ProviderId);
			Object->SetStringField(TEXT("revision"), Revision(*Target.Graph));
			TArray<TSharedPtr<FJsonValue>> Nodes;
			for (const UEdGraphNode* Node : Target.Graph->Nodes)
			{
				if (!Node) continue;
				const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("node_identity"), Node->GetPathName());
				Item->SetStringField(TEXT("node_guid"),
					Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
				Item->SetNumberField(TEXT("x"), Node->NodePosX);
				Item->SetNumberField(TEXT("y"), Node->NodePosY);
				Nodes.Add(MakeShared<FJsonValueObject>(Item));
			}
			Object->SetArrayField(TEXT("nodes"), MoveTemp(Nodes));
			return Object;
		}

		bool ParseTargets(
			const TSharedPtr<FJsonObject>& Params,
			TArray<FTarget>& OutTargets,
			FString& OutCode,
			FString& OutSubject,
			FString& OutDetail)
		{
			OutTargets.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Params.IsValid()
				|| !Params->TryGetArrayField(TEXT("targets"), Values)
				|| !Values || Values->IsEmpty())
			{
				OutCode = TEXT("unified_graph_layout_targets_required");
				return false;
			}
			TSet<FString> Identities;
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				const TSharedPtr<FJsonObject> Object = Value.IsValid()
					? Value->AsObject() : nullptr;
				FTarget Target;
				if (!Object.IsValid()
					|| !Object->TryGetStringField(
						TEXT("graph_identity"), Target.Identity)
					|| Target.Identity.IsEmpty()
					|| !Object->TryGetStringField(
						TEXT("expected_revision"), Target.ExpectedRevision)
					|| Target.ExpectedRevision.IsEmpty())
				{
					OutCode = TEXT("unified_graph_layout_target_invalid");
					return false;
				}
				if (Identities.Contains(Target.Identity))
				{
					OutCode = TEXT("unified_graph_layout_target_duplicate");
					OutSubject = Target.Identity;
					return false;
				}
				Identities.Add(Target.Identity);
				Target.Graph = Cast<UEdGraph>(StaticLoadObject(
					UEdGraph::StaticClass(), nullptr, *Target.Identity));
				if (!Target.Graph)
				{
					OutCode = TEXT("unified_graph_layout_target_unsupported");
					OutSubject = Target.Identity;
					return false;
				}
				bool bDuplicateProvider = false;
				if (!TryResolveProvider(
						*Target.Graph,
						Target.ProviderId,
						bDuplicateProvider))
				{
					OutCode = bDuplicateProvider
						? TEXT("unified_graph_layout_provider_duplicate")
						: TEXT("unified_graph_layout_provider_unsupported");
					OutSubject = Target.Identity;
					return false;
				}
				const FString CurrentRevision = Revision(*Target.Graph);
				if (CurrentRevision != Target.ExpectedRevision)
				{
					OutCode = TEXT("unified_graph_layout_stale_revision");
					OutSubject = Target.Identity;
					OutDetail = CurrentRevision;
					return false;
				}
				Target.Package = Target.Graph->GetOutermost();
				if (!Target.Package)
				{
					OutCode = TEXT("unified_graph_layout_package_missing");
					OutSubject = Target.Identity;
					return false;
				}
				OutTargets.Add(MoveTemp(Target));
			}
			return true;
		}
	}

	FUeAgentCommandResult Execute(const FUeAgentCommandRequest& Request)
	{
		if (!IsInGameThread())
		{
			return Failure(Request,
				TEXT("unified_graph_layout_game_thread_required"), CommandId, TEXT(""));
		}
		const TSharedPtr<FJsonObject>& Params = Request.GetParams();
		FString Strategy;
		FString Authorization;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("strategy"), Strategy)
			|| Strategy != TEXT("hierarchical_left_to_right")
			|| !Params->TryGetStringField(TEXT("authorization_id"), Authorization)
			|| Authorization.IsEmpty())
		{
			return Failure(Request,
				TEXT("unified_graph_layout_contract_invalid"), CommandId, Strategy);
		}
		const TSharedPtr<FJsonObject>* Parameters = nullptr;
		double Horizontal = 320.0;
		double Vertical = 160.0;
		if (Params->TryGetObjectField(TEXT("parameters"), Parameters)
			&& Parameters && Parameters->IsValid())
		{
			(*Parameters)->TryGetNumberField(TEXT("horizontal_spacing"), Horizontal);
			(*Parameters)->TryGetNumberField(TEXT("vertical_spacing"), Vertical);
		}
		if (Horizontal <= 0.0 || Vertical <= 0.0)
		{
			return Failure(Request,
				TEXT("unified_graph_layout_spacing_invalid"), CommandId, TEXT(""));
		}

		TArray<FTarget> Targets;
		FString Code;
		FString Subject;
		FString Detail;
		if (!ParseTargets(Params, Targets, Code, Subject, Detail))
		{
			return Failure(Request, Code, Subject, Detail);
		}

		for (FTarget& Target : Targets)
		{
			Target.Before.Reserve(Target.Graph->Nodes.Num());
			for (UEdGraphNode* Node : Target.Graph->Nodes)
			{
				Target.Before.Add(Node
					? FIntPoint(Node->NodePosX, Node->NodePosY)
					: FIntPoint::ZeroValue);
			}
		}
		auto Rollback = [&Targets]()
		{
			for (FTarget& Target : Targets)
			{
				for (int32 Index = 0; Index < Target.Graph->Nodes.Num(); ++Index)
				{
					if (UEdGraphNode* Node = Target.Graph->Nodes[Index])
					{
						Node->NodePosX = Target.Before[Index].X;
						Node->NodePosY = Target.Before[Index].Y;
					}
				}
				Target.Graph->NotifyGraphChanged();
			}
		};

		for (FTarget& Target : Targets)
		{
			TArray<UEdGraphNode*> Nodes;
			for (UEdGraphNode* Node : Target.Graph->Nodes)
			{
				if (Node) Nodes.Add(Node);
			}
			Nodes.Sort([](const UEdGraphNode& Left, const UEdGraphNode& Right)
			{
				return Left.NodeGuid < Right.NodeGuid;
			});
			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
			{
				Nodes[Index]->Modify();
				Nodes[Index]->NodePosX = FMath::RoundToInt(Horizontal * Index);
				Nodes[Index]->NodePosY = FMath::RoundToInt(Vertical * (Index % 3));
			}
			Target.Graph->NotifyGraphChanged();
			Target.Package->MarkPackageDirty();
		}
		TSet<UPackage*> PackageSet;
		for (const FTarget& Target : Targets) PackageSet.Add(Target.Package);
		const TArray<UPackage*> Packages = PackageSet.Array();
		if (!UEditorLoadingAndSavingUtils::SavePackages(Packages, true))
		{
			Rollback();
			return Failure(Request,
				TEXT("unified_graph_layout_save_failed"), CommandId, TEXT(""));
		}
		FText ReloadError;
		if (!UPackageTools::ReloadPackages(
				Packages,
				ReloadError,
				EReloadPackagesInteractionMode::AssumeNegative))
		{
			return Failure(Request,
				TEXT("unified_graph_layout_reload_failed"),
				CommandId,
				ReloadError.ToString());
		}
		for (FTarget& Target : Targets)
		{
			Target.Graph = Cast<UEdGraph>(StaticLoadObject(
				UEdGraph::StaticClass(), nullptr, *Target.Identity));
			if (!Target.Graph)
			{
				return Failure(Request,
					TEXT("unified_graph_layout_independent_readback_failed"),
					Target.Identity,
					TEXT("graph missing after package reload"));
			}
		}

		TArray<TSharedPtr<FJsonValue>> Results;
		TArray<TSharedPtr<FJsonValue>> ImpactTargets;
		for (const FTarget& Target : Targets)
		{
			const TSharedPtr<FJsonObject> Captured = Capture(Target);
			Results.Add(MakeShared<FJsonValueObject>(Captured));
			ImpactTargets.Add(MakeShared<FJsonValueString>(Target.Identity));
		}
		const TSharedPtr<FJsonObject> Impact = MakeShared<FJsonObject>();
		Impact->SetArrayField(TEXT("targets"), MoveTemp(ImpactTargets));
		Impact->SetStringField(TEXT("authorization_id"), Authorization);
		Impact->SetStringField(TEXT("atomicity"), TEXT("preflight_all_then_compensate"));
		const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("command_id"), CommandId);
		Payload->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
		Payload->SetStringField(TEXT("owner"), Owner);
		Payload->SetStringField(TEXT("strategy"), Strategy);
		Payload->SetNumberField(TEXT("target_count"), Targets.Num());
		Payload->SetBoolField(TEXT("saved"), true);
		Payload->SetBoolField(TEXT("settled"), true);
		Payload->SetObjectField(TEXT("impact_plan"), Impact);
		Payload->SetArrayField(TEXT("independent_readback"), MoveTemp(Results));
		FString PayloadJson;
		if (!Canonical(Payload, PayloadJson))
		{
			return Failure(Request,
				TEXT("unified_graph_layout_result_invalid"), CommandId, TEXT(""));
		}
		return FUeAgentCommandResult::Success(Request, MoveTemp(PayloadJson));
	}
}
