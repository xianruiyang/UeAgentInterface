#include "Composition/DestructiveImport/UeAgentDestructiveImportActivation.h"
#include "Composition/DestructiveImport/UeAgentDestructiveImportExternalActivation.h"
#include "Composition/DestructiveImport/UeAgentDestructiveImportWorldActivation.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"

namespace UE::UAI::Composition::DestructiveImport
{
	using namespace CommandContract;

	bool TryExtendDestructiveImportProductionCommandContractRegistry(
		TSharedRef<const FUeAgentCommandContractRegistry> BaseRegistry,
		TSharedPtr<const FUeAgentCommandContractRegistry>& OutRegistry,
		FString& OutError)
	{
		TArray<FUeAgentCommandContract> Contracts = BaseRegistry->GetContracts();
		TArray<FUeAgentCommandParameter> Parameters;
		Parameters.Emplace(
			TEXT("target_ref"), EUeAgentCommandWireType::String, true, false);
		Contracts.Emplace(
			TEXT("asset.delete"),
			TEXT("asset"),
			TEXT("delete"),
			MoveTemp(Parameters),
			1,
			1,
			EUeAgentCommandStatus::Active);
		auto AddTwoStringCommand = [&Contracts](
			const TCHAR* CommandId,
			const TCHAR* Namespace,
			const TCHAR* Method,
			const TCHAR* First,
			const TCHAR* Second)
		{
			TArray<FUeAgentCommandParameter> CommandParameters;
			CommandParameters.Emplace(
				First, EUeAgentCommandWireType::String, true, false);
			CommandParameters.Emplace(
				Second, EUeAgentCommandWireType::String, true, false);
			Contracts.Emplace(
				CommandId, Namespace, Method, MoveTemp(CommandParameters),
				1, 1, EUeAgentCommandStatus::Active);
		};
		AddTwoStringCommand(
			TEXT("asset.move"), TEXT("asset"), TEXT("move"),
			TEXT("target_ref"), TEXT("destination_ref"));
		{
			TArray<FUeAgentCommandParameter> FixupParameters;
			FixupParameters.Emplace(
				TEXT("root_ref"), EUeAgentCommandWireType::String, true, false);
			Contracts.Emplace(
				TEXT("asset.fixup_redirectors"), TEXT("asset"),
				TEXT("fixup_redirectors"), MoveTemp(FixupParameters),
				1, 1, EUeAgentCommandStatus::Active);
		}
		for (const TCHAR* Family : {
			TEXT("texture"), TEXT("sound_wave"), TEXT("static_mesh"),
			TEXT("sparse_volume_texture")})
		{
			const FString Method = TEXT("reimport_") + FString(Family);
			AddTwoStringCommand(
				*(TEXT("asset.") + Method), TEXT("asset"), *Method,
				TEXT("target_ref"), TEXT("source_ref"));
		}
		AddTwoStringCommand(
			TEXT("skeletal_mesh.remove_morph_target"),
			TEXT("skeletal_mesh"), TEXT("remove_morph_target"),
			TEXT("target_ref"), TEXT("name"));
		AddTwoStringCommand(
			TEXT("skeletal_mesh.remove_skin_weight_profile"),
			TEXT("skeletal_mesh"), TEXT("remove_skin_weight_profile"),
			TEXT("target_ref"), TEXT("name"));
		AppendWorldDestructiveProductionCommandContracts(Contracts);
		AppendExternalDataImportProductionCommandContracts(Contracts);
		return FUeAgentCommandContractRegistry::TryCreate(
			MoveTemp(Contracts), OutRegistry, OutError);
	}

	bool TryCreateDestructiveImportProductionCommandContractRegistry(
		TSharedPtr<const FUeAgentCommandContractRegistry>& OutRegistry,
		FString& OutError)
	{
		TSharedPtr<const FUeAgentCommandContractRegistry> Base;
		if (!GraphCompound::
				TryCreateGraphCompoundProductionCommandContractRegistry(
					Base, OutError)
			|| !Base.IsValid())
		{
			return false;
		}
		return TryExtendDestructiveImportProductionCommandContractRegistry(
			Base.ToSharedRef(), OutRegistry, OutError);
	}
}
