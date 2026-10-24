#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

class UPCGGraph;

namespace UE::UAI::PcgCompilation
{
	/** Refresh the native compiler cache and preserve synchronous native diagnostics.
	 * UE 5.6's Recompile return value describes change, not compilation success. */
	bool Compile(UPCGGraph& Graph, Contract::FDiagnostic& OutError);
}
