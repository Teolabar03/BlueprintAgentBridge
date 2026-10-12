// Copyright (c) 2026 Matteo La Barbera. MIT License.

#pragma once

#include "CoreMinimal.h"

class UEdGraph;
class UEdGraphNode;

/**
 * Simple layered layout: nodes are ranked left-to-right along their connections
 * (execution and data), then stacked top-to-bottom inside each rank with the
 * exec chain kept as straight as possible. Good enough to keep generated graphs
 * readable by humans; it is not a replacement for a hand-made layout.
 */
namespace BAB
{
	struct FLayoutOptions
	{
		FVector2D Origin = FVector2D(0.0, 0.0);
		float ColumnGap = 96.0f;
		float RowGap = 48.0f;
		/** When empty every non-comment node of the graph is laid out. */
		TArray<UEdGraphNode*> Subset;
	};

	/** Returns the number of nodes that were moved. */
	int32 AutoLayoutGraph(UEdGraph* Graph, const FLayoutOptions& Options);
}
