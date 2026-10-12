// Copyright (c) 2026 Matteo La Barbera. MIT License.

#include "BABAutoLayout.h"
#include "BABContext.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_Knot.h"

namespace BAB
{
	namespace
	{
		struct FLayoutNode
		{
			UEdGraphNode* Node = nullptr;
			TArray<int32> Successors;
			TArray<int32> Predecessors;
			TArray<int32> ExecPredecessors;
			int32 Layer = 0;
			float Width = 0.0f;
			float Height = 0.0f;
			float Y = 0.0f;
			float PreferredY = 0.0f;
		};

		FVector2f EstimateNodeSize(UEdGraphNode* Node)
		{
			if (Node->IsA<UK2Node_Knot>())
			{
				return FVector2f(40.0f, 32.0f);
			}
			int32 Inputs = 0;
			int32 Outputs = 0;
			int32 LongestPinName = 0;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->bHidden) continue;
				(Pin->Direction == EGPD_Input ? Inputs : Outputs)++;
				LongestPinName = FMath::Max(LongestPinName, Pin->GetDisplayName().ToString().Len());
			}
			const int32 TitleLength = NodeTitle(Node).Len();
			const float Width = FMath::Clamp(140.0f + 7.5f * FMath::Max(TitleLength, LongestPinName * 2), 180.0f, 480.0f);
			const float Height = 56.0f + 26.0f * FMath::Max(Inputs, Outputs);
			return FVector2f(Width, Height);
		}

		float Snap(float Value)
		{
			return FMath::RoundToFloat(Value / 16.0f) * 16.0f;
		}
	}

	int32 AutoLayoutGraph(UEdGraph* Graph, const FLayoutOptions& Options)
	{
		if (!Graph) return 0;

		TArray<FLayoutNode> Nodes;
		TMap<UEdGraphNode*, int32> IndexOf;
		TArray<UEdGraphNode*> Source = Options.Subset;
		if (Source.Num() == 0)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node) Source.Add(Node);
			}
		}
		for (UEdGraphNode* Node : Source)
		{
			if (!Node || Node->IsA<UEdGraphNode_Comment>()) continue;
			FLayoutNode Entry;
			Entry.Node = Node;
			const FVector2f Size = EstimateNodeSize(Node);
			Entry.Width = Size.X;
			Entry.Height = Size.Y;
			IndexOf.Add(Node, Nodes.Num());
			Nodes.Add(Entry);
		}
		if (Nodes.Num() == 0) return 0;

		// Edges follow data and execution flow from outputs to inputs.
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			for (UEdGraphPin* Pin : Nodes[Index].Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output) continue;
				const bool bExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (!Linked || !Linked->GetOwningNodeUnchecked()) continue;
					if (const int32* Target = IndexOf.Find(Linked->GetOwningNode()))
					{
						if (*Target == Index) continue;
						Nodes[Index].Successors.AddUnique(*Target);
						Nodes[*Target].Predecessors.AddUnique(Index);
						if (bExec) Nodes[*Target].ExecPredecessors.AddUnique(Index);
					}
				}
			}
		}

		// Break cycles with a DFS: edges that point back to a node on the stack are ignored.
		TSet<TPair<int32, int32>> BackEdges;
		{
			TArray<int32> State;
			State.Init(0, Nodes.Num());
			TFunction<void(int32)> Visit = [&](int32 Index)
			{
				State[Index] = 1;
				for (int32 Next : Nodes[Index].Successors)
				{
					if (State[Next] == 1) BackEdges.Add(TPair<int32, int32>(Index, Next));
					else if (State[Next] == 0) Visit(Next);
				}
				State[Index] = 2;
			};
			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
			{
				if (State[Index] == 0) Visit(Index);
			}
		}

		// Longest-path layering on the acyclic edge set.
		bool bChanged = true;
		int32 Iterations = 0;
		while (bChanged && Iterations++ < Nodes.Num() + 2)
		{
			bChanged = false;
			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
			{
				for (int32 Next : Nodes[Index].Successors)
				{
					if (BackEdges.Contains(TPair<int32, int32>(Index, Next))) continue;
					if (Nodes[Next].Layer < Nodes[Index].Layer + 1)
					{
						Nodes[Next].Layer = Nodes[Index].Layer + 1;
						bChanged = true;
					}
				}
			}
		}

		// Pure data producers with a single consumer sit directly left of it.
		int32 LayerCount = 0;
		for (const FLayoutNode& Entry : Nodes) LayerCount = FMath::Max(LayerCount, Entry.Layer + 1);
		TArray<TArray<int32>> Layers;
		Layers.SetNum(LayerCount);
		for (int32 Index = 0; Index < Nodes.Num(); ++Index) Layers[Nodes[Index].Layer].Add(Index);

		const float OriginX = (float)Options.Origin.X;
		const float OriginY = (float)Options.Origin.Y;
		TArray<float> LayerX;
		LayerX.SetNum(LayerCount);
		float X = OriginX;
		for (int32 Layer = 0; Layer < LayerCount; ++Layer)
		{
			LayerX[Layer] = X;
			float MaxWidth = 0.0f;
			for (int32 Index : Layers[Layer]) MaxWidth = FMath::Max(MaxWidth, Nodes[Index].Width);
			X += MaxWidth + Options.ColumnGap;
		}

		// Vertical placement layer by layer.
		for (int32 Layer = 0; Layer < LayerCount; ++Layer)
		{
			TArray<int32>& Members = Layers[Layer];
			for (int32 Index : Members)
			{
				FLayoutNode& Entry = Nodes[Index];
				if (Layer == 0 || Entry.Predecessors.Num() == 0)
				{
					Entry.PreferredY = (float)Entry.Node->NodePosY;
				}
				else if (Entry.ExecPredecessors.Num() > 0)
				{
					// keep the execution wire straight
					float Sum = 0.0f;
					for (int32 Pred : Entry.ExecPredecessors) Sum += Nodes[Pred].Y;
					Entry.PreferredY = Sum / Entry.ExecPredecessors.Num();
				}
				else
				{
					float Sum = 0.0f;
					for (int32 Pred : Entry.Predecessors) Sum += Nodes[Pred].Y;
					Entry.PreferredY = Sum / Entry.Predecessors.Num() + 40.0f;
				}
			}
			Members.Sort([&Nodes](int32 A, int32 B)
			{
				if (Nodes[A].PreferredY != Nodes[B].PreferredY) return Nodes[A].PreferredY < Nodes[B].PreferredY;
				return Nodes[A].Node->NodePosX < Nodes[B].Node->NodePosX;
			});
			float Cursor = Layer == 0 ? OriginY : -FLT_MAX;
			for (int32 Index : Members)
			{
				FLayoutNode& Entry = Nodes[Index];
				const float Wanted = Layer == 0 ? FMath::Max(Cursor, OriginY) : Entry.PreferredY;
				Entry.Y = FMath::Max(Wanted, Cursor);
				Cursor = Entry.Y + Entry.Height + Options.RowGap;
			}
		}

		// Normalize so the top-most node sits at the origin.
		float MinY = FLT_MAX;
		for (const FLayoutNode& Entry : Nodes) MinY = FMath::Min(MinY, Entry.Y);
		const float Shift = OriginY - MinY;

		int32 Moved = 0;
		for (FLayoutNode& Entry : Nodes)
		{
			const int32 NewX = (int32)Snap(LayerX[Entry.Layer]);
			const int32 NewY = (int32)Snap(Entry.Y + Shift);
			if (Entry.Node->NodePosX != NewX || Entry.Node->NodePosY != NewY)
			{
				Entry.Node->Modify();
				Entry.Node->NodePosX = NewX;
				Entry.Node->NodePosY = NewY;
				Moved++;
			}
		}
		return Moved;
	}
}
