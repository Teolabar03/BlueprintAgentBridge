// Copyright (c) 2026 Matteo La Barbera. MIT License.

#include "Misc/AutomationTest.h"
#include "BABDispatcher.h"
#include "BABTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace BABTest
{
	static FBABResult Run(const TCHAR* Op, const FString& ParamsJson)
	{
		BABRegisterAllOperations();
		FString Error;
		TSharedPtr<FJsonObject> Params = BAB::JsonFromString(ParamsJson, Error);
		const FJsonObject Empty;
		return FBABDispatcher::Get().Dispatch(Op, Params.IsValid() ? *Params : Empty);
	}

	static FString UniqueName(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	}

	static FString Errors(const FBABResult& Result)
	{
		return FString::Join(Result.Errors, TEXT(" | "));
	}

	static FString ParamsFor(const FString& Blueprint, const FString& Extra)
	{
		if (Extra.IsEmpty())
		{
			return FString::Printf(TEXT("{\"blueprint\":\"%s\"}"), *Blueprint);
		}
		return FString::Printf(TEXT("{\"blueprint\":\"%s\",%s}"), *Blueprint, *Extra);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBABEndToEndTest, "BlueprintAgentBridge.EndToEnd.BeginPlayDelayPrint", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBABEndToEndTest::RunTest(const FString& Parameters)
{
	using namespace BABTest;

	const FString Name = UniqueName(TEXT("BP_BABTest"));
	const FString Path = TEXT("/Game/BABTests");
	const FString BP = Path + TEXT("/") + Name;

	// Create
	FBABResult Create = Run(TEXT("bp_create_blueprint"), FString::Printf(TEXT("{\"path\":\"%s\",\"name\":\"%s\",\"parent_class\":\"Actor\"}"), *Path, *Name));
	if (!TestTrue(TEXT("bp_create_blueprint succeeds"), Create.bOk)) { AddError(Errors(Create)); return false; }

	// Add nodes
	auto AddNode = [&](const FString& Spec, FString& OutId) -> bool
	{
		FBABResult R = Run(TEXT("bp_add_node"), ParamsFor(BP, FString::Printf(TEXT("\"graph\":\"EventGraph\",\"node\":%s"), *Spec)));
		if (!R.bOk) { AddError(FString::Printf(TEXT("bp_add_node %s failed: %s"), *Spec, *Errors(R))); return false; }
		OutId = R.Data->GetStringField(TEXT("id"));
		return !OutId.IsEmpty();
	};
	FString BeginPlay, Delay, Print;
	if (!AddNode(TEXT("{\"type\":\"event\",\"name\":\"BeginPlay\"}"), BeginPlay)) return false;
	if (!AddNode(TEXT("{\"type\":\"call_function\",\"function\":\"Delay\"}"), Delay)) return false;
	if (!AddNode(TEXT("{\"type\":\"call_function\",\"function\":\"KismetSystemLibrary.PrintString\"}"), Print)) return false;

	// Duplicate event must be refused (the first call enabled the template node that new Blueprints contain)
	FBABResult Duplicate = Run(TEXT("bp_add_node"), ParamsFor(BP, TEXT("\"graph\":\"EventGraph\",\"node\":{\"type\":\"event\",\"name\":\"BeginPlay\"}")));
	TestFalse(TEXT("duplicate BeginPlay is refused"), Duplicate.bOk);
	TestTrue(TEXT("duplicate error names the existing node"), Duplicate.Errors.Num() > 0 && Duplicate.Errors[0].Contains(BeginPlay));

	// Connect
	FBABResult C1 = Run(TEXT("bp_connect_pins"), ParamsFor(BP, FString::Printf(TEXT("\"from\":\"%s.then\",\"to\":\"%s.execute\""), *BeginPlay, *Delay)));
	if (!TestTrue(TEXT("BeginPlay.then -> Delay.execute"), C1.bOk)) AddError(Errors(C1));
	FBABResult C2 = Run(TEXT("bp_connect_pins"), ParamsFor(BP, FString::Printf(TEXT("\"from\":{\"node\":\"%s\",\"pin\":\"Completed\"},\"to\":{\"node\":\"%s\",\"pin\":\"execute\"}"), *Delay, *Print)));
	if (!TestTrue(TEXT("Delay.Completed -> PrintString.execute"), C2.bOk)) AddError(Errors(C2));

	// Invalid connection is refused with a message
	FBABResult Bad = Run(TEXT("bp_connect_pins"), ParamsFor(BP, FString::Printf(TEXT("\"from\":\"%s.Completed\",\"to\":\"%s.InString\""), *Delay, *Print)));
	TestFalse(TEXT("exec -> string connection is refused"), Bad.bOk);
	TestTrue(TEXT("refusal carries a message"), Bad.Errors.Num() > 0 && Bad.Errors[0].Len() > 10);

	// Defaults
	FBABResult D1 = Run(TEXT("bp_set_pin_default"), ParamsFor(BP, FString::Printf(TEXT("\"node\":\"%s\",\"pin\":\"Duration\",\"value\":2"), *Delay)));
	if (!TestTrue(TEXT("Delay.Duration = 2"), D1.bOk)) AddError(Errors(D1));
	FBABResult D2 = Run(TEXT("bp_set_pin_default"), ParamsFor(BP, FString::Printf(TEXT("\"node\":\"%s\",\"pin\":\"InString\",\"value\":\"Hello from the bridge\""), *Print)));
	if (!TestTrue(TEXT("PrintString.InString set"), D2.bOk)) AddError(Errors(D2));
	FBABResult D3 = Run(TEXT("bp_set_pin_default"), ParamsFor(BP, FString::Printf(TEXT("\"node\":\"%s\",\"pin\":\"TextColor\",\"value\":{\"R\":1,\"G\":0,\"B\":0,\"A\":1}"), *Print)));
	if (!TestTrue(TEXT("PrintString.TextColor set from object"), D3.bOk)) AddError(Errors(D3));
	FBABResult DBad = Run(TEXT("bp_set_pin_default"), ParamsFor(BP, FString::Printf(TEXT("\"node\":\"%s\",\"pin\":\"Duration\",\"value\":\"not a number\""), *Delay)));
	TestFalse(TEXT("invalid float default is refused"), DBad.bOk);

	// Variable + getter
	FBABResult V = Run(TEXT("bp_add_variable"), ParamsFor(BP, TEXT("\"name\":\"Health\",\"type\":\"float\",\"default\":100,\"instance_editable\":true")));
	if (!TestTrue(TEXT("bp_add_variable Health"), V.bOk)) AddError(Errors(V));
	FString Getter;
	if (!AddNode(TEXT("{\"type\":\"variable_get\",\"variable\":\"Health\"}"), Getter)) return false;
	FBABResult VDup = Run(TEXT("bp_add_variable"), ParamsFor(BP, TEXT("\"name\":\"Health\",\"type\":\"int\"")));
	TestFalse(TEXT("duplicate variable is refused"), VDup.bOk);

	// Function with parameters
	FBABResult F = Run(TEXT("bp_add_function"), ParamsFor(BP, TEXT("\"name\":\"Heal\",\"inputs\":[{\"name\":\"Amount\",\"type\":\"float\"}],\"outputs\":[{\"name\":\"NewHealth\",\"type\":\"float\"}]")));
	if (!TestTrue(TEXT("bp_add_function Heal"), F.bOk)) AddError(Errors(F));
	FBABResult FRead = Run(TEXT("bp_read_graph"), ParamsFor(BP, TEXT("\"graph\":\"Heal\"")));
	TestTrue(TEXT("Heal graph readable"), FRead.bOk && FRead.Data->GetStringField(TEXT("text")).Contains(TEXT("function_entry")));
	TestTrue(TEXT("Heal graph lists Amount input"), FRead.bOk && FRead.Data->GetStringField(TEXT("text")).Contains(TEXT("Amount")));

	// Call the new function from the event graph
	FString CallHeal;
	if (!AddNode(TEXT("{\"type\":\"call_function\",\"function\":\"self.Heal\"}"), CallHeal)) return false;

	// Search
	FBABResult S = Run(TEXT("bp_search_nodes"), ParamsFor(BP, TEXT("\"query\":\"print string\",\"limit\":5")));
	if (!TestTrue(TEXT("bp_search_nodes works"), S.bOk)) AddError(Errors(S));
	const TArray<TSharedPtr<FJsonValue>>* SearchResults = nullptr;
	const bool bHasResults = S.bOk && S.Data->TryGetArrayField(TEXT("results"), SearchResults) && SearchResults && SearchResults->Num() > 0;
	TestTrue(TEXT("search returns results"), bHasResults);
	if (bHasResults)
	{
		const FString FirstFunction = (*SearchResults)[0]->AsObject()->GetStringField(TEXT("function"));
		TestTrue(TEXT("first result is PrintString"), FirstFunction.Contains(TEXT("PrintString")));
	}

	// Compile: must succeed without errors
	FBABResult Compile = Run(TEXT("bp_compile"), ParamsFor(BP, TEXT("\"save\":false")));
	if (!TestTrue(TEXT("bp_compile ok"), Compile.bOk)) AddError(Errors(Compile));
	TestEqual(TEXT("no compile errors"), (int32)Compile.Data->GetNumberField(TEXT("num_errors")), 0);
	TestTrue(TEXT("status is up to date"), Compile.Data->GetStringField(TEXT("status")).StartsWith(TEXT("UpToDate")));

	// Read back
	FBABResult Read = Run(TEXT("bp_read_graph"), ParamsFor(BP, TEXT("\"graph\":\"EventGraph\",\"format\":\"both\"")));
	if (!TestTrue(TEXT("bp_read_graph ok"), Read.bOk)) AddError(Errors(Read));
	const FString Text = Read.Data->GetStringField(TEXT("text"));
	TestTrue(TEXT("text lists Print String"), Text.Contains(TEXT("Print String")));
	TestTrue(TEXT("text shows the Delay link"), Text.Contains(FString::Printf(TEXT("-> %s.execute"), *Print)));
	TestTrue(TEXT("text shows the string default"), Text.Contains(TEXT("Hello from the bridge")));
	for (const FString& Id : { BeginPlay, Delay, Print, Getter, CallHeal })
	{
		TestTrue(FString::Printf(TEXT("text lists node %s"), *Id), Text.Contains(FString::Printf(TEXT("[%s]"), *Id)));
	}

	// Describe
	FBABResult Describe = Run(TEXT("bp_describe_blueprint"), ParamsFor(BP, TEXT("\"format\":\"both\"")));
	TestTrue(TEXT("describe lists Health"), Describe.bOk && Describe.Data->GetStringField(TEXT("text")).Contains(TEXT("Health")));
	// After the full compile the default lives in the class default object; it must still be reported.
	TestTrue(TEXT("describe shows the Health default after compile"), Describe.bOk && Describe.Data->GetStringField(TEXT("text")).Contains(TEXT("Health : float = 100")));
	FBABResult UpdateDefault = Run(TEXT("bp_update_variable"), ParamsFor(BP, TEXT("\"name\":\"Health\",\"default\":250")));
	if (!TestTrue(TEXT("bp_update_variable default"), UpdateDefault.bOk)) AddError(Errors(UpdateDefault));
	FBABResult Describe2 = Run(TEXT("bp_describe_blueprint"), ParamsFor(BP, TEXT("\"format\":\"text\"")));
	TestTrue(TEXT("describe shows the updated default"), Describe2.bOk && Describe2.Data->GetStringField(TEXT("text")).Contains(TEXT("Health : float = 250")));
	FBABResult BadDefault = Run(TEXT("bp_update_variable"), ParamsFor(BP, TEXT("\"name\":\"Health\",\"default\":\"not a number\"")));
	TestFalse(TEXT("invalid variable default is refused"), BadDefault.bOk);
	TestTrue(TEXT("describe lists Heal"), Describe.bOk && Describe.Data->GetStringField(TEXT("text")).Contains(TEXT("Heal(")));

	// Layout
	FBABResult Layout = Run(TEXT("bp_auto_layout"), ParamsFor(BP, TEXT("\"graph\":\"EventGraph\"")));
	if (!TestTrue(TEXT("bp_auto_layout ok"), Layout.bOk)) AddError(Errors(Layout));

	// Export / import round trip
	FBABResult Export = Run(TEXT("bp_export_text"), ParamsFor(BP, FString::Printf(TEXT("\"graph\":\"EventGraph\",\"nodes\":[\"%s\"]"), *Print)));
	if (!TestTrue(TEXT("bp_export_text ok"), Export.bOk)) AddError(Errors(Export));
	FString ExportedText = Export.Data->GetStringField(TEXT("text"));
	ExportedText.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	ExportedText.ReplaceInline(TEXT("\""), TEXT("\\\""));
	ExportedText.ReplaceInline(TEXT("\r"), TEXT("\\r"));
	ExportedText.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	ExportedText.ReplaceInline(TEXT("\t"), TEXT("\\t"));
	FBABResult Import = Run(TEXT("bp_import_text"), ParamsFor(BP, FString::Printf(TEXT("\"graph\":\"EventGraph\",\"text\":\"%s\",\"pos\":[0,800]"), *ExportedText)));
	if (!TestTrue(TEXT("bp_import_text ok"), Import.bOk)) AddError(Errors(Import));
	TestEqual(TEXT("one node imported"), (int32)Import.Data->GetNumberField(TEXT("count")), 1);

	// Remove node and recompile
	FBABResult Remove = Run(TEXT("bp_remove_node"), ParamsFor(BP, FString::Printf(TEXT("\"node\":\"%s\""), *Print)));
	if (!TestTrue(TEXT("bp_remove_node ok"), Remove.bOk)) AddError(Errors(Remove));
	FBABResult Compile2 = Run(TEXT("bp_compile"), ParamsFor(BP, TEXT("")));
	TestTrue(TEXT("compile after removal ok"), Compile2.bOk && (int32)Compile2.Data->GetNumberField(TEXT("num_errors")) == 0);

	// Remove the variable that is still used -> refused, then forced
	FBABResult RemoveVar = Run(TEXT("bp_remove_variable"), ParamsFor(BP, TEXT("\"name\":\"Health\"")));
	TestFalse(TEXT("removing a used variable is refused"), RemoveVar.bOk);
	FBABResult RemoveVarForced = Run(TEXT("bp_remove_variable"), ParamsFor(BP, TEXT("\"name\":\"Health\",\"force\":true")));
	TestTrue(TEXT("forced removal works"), RemoveVarForced.bOk);

	// Unknown operation / missing blueprint produce clean errors
	FBABResult Unknown = Run(TEXT("bp_does_not_exist"), TEXT("{}"));
	TestFalse(TEXT("unknown op fails"), Unknown.bOk);
	FBABResult Missing = Run(TEXT("bp_read_graph"), TEXT("{\"blueprint\":\"/Game/Nope/BP_Missing\"}"));
	TestFalse(TEXT("missing blueprint fails"), Missing.bOk);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBABBranchAndMacroTest, "BlueprintAgentBridge.Nodes.BranchCastMacroSequence", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBABBranchAndMacroTest::RunTest(const FString& Parameters)
{
	using namespace BABTest;
	const FString Name = UniqueName(TEXT("BP_BABNodes"));
	const FString Path = TEXT("/Game/BABTests");
	const FString BP = Path + TEXT("/") + Name;
	FBABResult Create = Run(TEXT("bp_create_blueprint"), FString::Printf(TEXT("{\"path\":\"%s\",\"name\":\"%s\",\"parent_class\":\"Actor\"}"), *Path, *Name));
	if (!TestTrue(TEXT("create"), Create.bOk)) { AddError(Errors(Create)); return false; }

	auto Add = [&](const FString& Spec, const TCHAR* Label) -> FString
	{
		FBABResult R = Run(TEXT("bp_add_node"), ParamsFor(BP, FString::Printf(TEXT("\"node\":%s"), *Spec)));
		if (!TestTrue(Label, R.bOk)) AddError(Errors(R));
		return R.bOk ? R.Data->GetStringField(TEXT("id")) : FString();
	};
	const FString Tick = Add(TEXT("{\"type\":\"event\",\"name\":\"Tick\"}"), TEXT("event Tick"));
	const FString Branch = Add(TEXT("{\"type\":\"branch\"}"), TEXT("branch"));
	const FString Sequence = Add(TEXT("{\"type\":\"sequence\"}"), TEXT("sequence"));
	const FString Cast = Add(TEXT("{\"type\":\"cast\",\"class\":\"Pawn\"}"), TEXT("cast to Pawn"));
	const FString Loop = Add(TEXT("{\"type\":\"macro\",\"macro\":\"ForEachLoop\"}"), TEXT("ForEachLoop macro"));
	const FString Custom = Add(TEXT("{\"type\":\"custom_event\",\"name\":\"OnDamaged\",\"inputs\":[{\"name\":\"Amount\",\"type\":\"float\"}]}"), TEXT("custom event"));
	const FString Self = Add(TEXT("{\"type\":\"self\"}"), TEXT("self"));
	const FString Spawn = Add(TEXT("{\"type\":\"spawn_actor\",\"class\":\"Actor\"}"), TEXT("spawn actor"));
	const FString Comment = Add(TEXT("{\"type\":\"comment\",\"text\":\"Generated\"}"), TEXT("comment"));
	const FString MakeVec = Add(TEXT("{\"type\":\"make_struct\",\"struct\":\"Vector\"}"), TEXT("make Vector"));

	if (!Tick.IsEmpty() && !Branch.IsEmpty())
	{
		FBABResult C = Run(TEXT("bp_connect_pins"), ParamsFor(BP, FString::Printf(TEXT("\"from\":\"%s.then\",\"to\":\"%s.execute\""), *Tick, *Branch)));
		TestTrue(TEXT("tick -> branch"), C.bOk);
	}
	if (!Self.IsEmpty() && !Cast.IsEmpty())
	{
		FBABResult C = Run(TEXT("bp_connect_pins"), ParamsFor(BP, FString::Printf(TEXT("\"from\":\"%s.self\",\"to\":\"%s.Object\""), *Self, *Cast)));
		TestTrue(TEXT("self -> cast object"), C.bOk);
	}
	if (!Sequence.IsEmpty())
	{
		FBABResult AddPin = Run(TEXT("bp_add_pin"), ParamsFor(BP, FString::Printf(TEXT("\"node\":\"%s\""), *Sequence)));
		TestTrue(TEXT("sequence add pin"), AddPin.bOk);
	}
	if (!MakeVec.IsEmpty())
	{
		FBABResult Def = Run(TEXT("bp_set_pin_default"), ParamsFor(BP, FString::Printf(TEXT("\"node\":\"%s\",\"pin\":\"X\",\"value\":3.5"), *MakeVec)));
		TestTrue(TEXT("make vector X default"), Def.bOk);
	}
	FBABResult Compile = Run(TEXT("bp_compile"), ParamsFor(BP, TEXT("")));
	TestTrue(TEXT("compile ok"), Compile.bOk);
	if (Compile.bOk)
	{
		TestEqual(TEXT("no errors"), (int32)Compile.Data->GetNumberField(TEXT("num_errors")), 0);
	}

	// Undo / redo: edits made inside an editor session are one transaction each.
	const FString Knot = Add(TEXT("{\"type\":\"knot\"}"), TEXT("knot before undo"));
	FBABResult Undo = Run(TEXT("bp_undo"), TEXT("{}"));
	if (Undo.bOk)
	{
		TestTrue(TEXT("undo names the bridge transaction"), Undo.Data->GetStringField(TEXT("undone")).Contains(TEXT("bp_add_node")));
		FBABResult AfterUndo = Run(TEXT("bp_read_graph"), ParamsFor(BP, TEXT("")));
		TestFalse(TEXT("undone node is gone"), AfterUndo.Data->GetStringField(TEXT("text")).Contains(FString::Printf(TEXT("[%s]"), *Knot)));
		FBABResult Redo = Run(TEXT("bp_redo"), TEXT("{}"));
		TestTrue(TEXT("redo ok"), Redo.bOk);
		FBABResult AfterRedo = Run(TEXT("bp_read_graph"), ParamsFor(BP, TEXT("")));
		TestTrue(TEXT("redone node is back"), AfterRedo.Data->GetStringField(TEXT("text")).Contains(FString::Printf(TEXT("[%s]"), *Knot)));
	}
	else
	{
		AddInfo(FString::Printf(TEXT("undo not available in this context: %s"), *Errors(Undo)));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBABActorBuildingTest, "BlueprintAgentBridge.Actor.ComponentsDispatchersInterfacesTimelines", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBABActorBuildingTest::RunTest(const FString& Parameters)
{
	using namespace BABTest;
	const FString Path = TEXT("/Game/BABTests");
	const FString Name = UniqueName(TEXT("BP_BABActor"));
	const FString BP = Path + TEXT("/") + Name;
	FBABResult Create = Run(TEXT("bp_create_blueprint"), FString::Printf(TEXT("{\"path\":\"%s\",\"name\":\"%s\",\"parent_class\":\"Actor\"}"), *Path, *Name));
	if (!TestTrue(TEXT("create actor"), Create.bOk)) { AddError(Errors(Create)); return false; }

	// Components
	FBABResult Mesh = Run(TEXT("bp_add_component"), ParamsFor(BP, TEXT("\"class\":\"StaticMeshComponent\",\"name\":\"DoorMesh\",\"properties\":{\"RelativeLocation\":[0,0,50]}")));
	if (!TestTrue(TEXT("add StaticMeshComponent"), Mesh.bOk)) AddError(Errors(Mesh));
	FBABResult Trigger = Run(TEXT("bp_add_component"), ParamsFor(BP, TEXT("\"class\":\"BoxComponent\",\"name\":\"Trigger\",\"parent\":\"DoorMesh\",\"properties\":{\"BoxExtent\":[100,100,50],\"bGenerateOverlapEvents\":true}")));
	if (!TestTrue(TEXT("add BoxComponent under DoorMesh"), Trigger.bOk)) AddError(Errors(Trigger));
	TestEqual(TEXT("trigger parent"), Trigger.Data->GetStringField(TEXT("parent")), FString(TEXT("DoorMesh")));
	FBABResult GetTrigger = Run(TEXT("bp_get_component"), ParamsFor(BP, TEXT("\"component\":\"Trigger\"")));
	const TSharedPtr<FJsonObject>* TriggerProps = nullptr;
	const bool bHasExtent = GetTrigger.bOk && GetTrigger.Data->TryGetObjectField(TEXT("properties"), TriggerProps) && TriggerProps && (*TriggerProps)->HasField(TEXT("BoxExtent"));
	TestTrue(TEXT("BoxExtent reported as modified"), bHasExtent);
	if (bHasExtent) TestTrue(TEXT("BoxExtent value"), (*TriggerProps)->GetStringField(TEXT("BoxExtent")).StartsWith(TEXT("100.0")));
	FBABResult SetProp = Run(TEXT("bp_set_component_property"), ParamsFor(BP, TEXT("\"component\":\"DoorMesh\",\"property\":\"RelativeScale3D\",\"value\":[1,2,3]")));
	TestTrue(TEXT("set component property"), SetProp.bOk);
	FBABResult DupComponent = Run(TEXT("bp_add_component"), ParamsFor(BP, TEXT("\"class\":\"BoxComponent\",\"name\":\"Trigger\"")));
	TestFalse(TEXT("duplicate component name refused"), DupComponent.bOk);

	// Component bound event
	FBABResult Overlap = Run(TEXT("bp_add_node"), ParamsFor(BP, TEXT("\"node\":{\"type\":\"component_event\",\"component\":\"Trigger\",\"event\":\"OnComponentBeginOverlap\"}")));
	if (!TestTrue(TEXT("component event"), Overlap.bOk)) AddError(Errors(Overlap));
	TestTrue(TEXT("component event has OtherActor pin"), Overlap.bOk && Overlap.Data->GetStringField(TEXT("text")).Contains(TEXT("OtherActor")));
	FBABResult OverlapAgain = Run(TEXT("bp_add_node"), ParamsFor(BP, TEXT("\"node\":{\"type\":\"component_event\",\"component\":\"Trigger\",\"event\":\"OnComponentBeginOverlap\"}")));
	TestFalse(TEXT("duplicate component event refused"), OverlapAgain.bOk);

	// Event dispatcher + call/bind nodes
	FBABResult Dispatcher = Run(TEXT("bp_add_event_dispatcher"), ParamsFor(BP, TEXT("\"name\":\"OnOpened\",\"params\":[{\"name\":\"Opener\",\"type\":\"object:Actor\"}]")));
	if (!TestTrue(TEXT("add event dispatcher"), Dispatcher.bOk)) AddError(Errors(Dispatcher));
	FBABResult CallDispatcher = Run(TEXT("bp_add_node"), ParamsFor(BP, TEXT("\"node\":{\"type\":\"call_dispatcher\",\"dispatcher\":\"OnOpened\"}")));
	if (!TestTrue(TEXT("call dispatcher node"), CallDispatcher.bOk)) AddError(Errors(CallDispatcher));
	TestTrue(TEXT("call node exposes the Opener param"), CallDispatcher.bOk && CallDispatcher.Data->GetStringField(TEXT("text")).Contains(TEXT("Opener")));
	FBABResult Bind = Run(TEXT("bp_add_node"), ParamsFor(BP, TEXT("\"node\":{\"type\":\"bind_dispatcher\",\"dispatcher\":\"OnOpened\"}")));
	TestTrue(TEXT("bind dispatcher node"), Bind.bOk);
	FBABResult CustomEvent = Run(TEXT("bp_add_node"), ParamsFor(BP, TEXT("\"node\":{\"type\":\"custom_event\",\"name\":\"HandleOpened\",\"inputs\":[{\"name\":\"Opener\",\"type\":\"object:Actor\"}]}")));
	TestTrue(TEXT("custom event for binding"), CustomEvent.bOk);
	if (Bind.bOk && CustomEvent.bOk)
	{
		FBABResult Wire = Run(TEXT("bp_connect_pins"), ParamsFor(BP, FString::Printf(TEXT("\"from\":\"%s.OutputDelegate\",\"to\":\"%s.Delegate\""), *CustomEvent.Data->GetStringField(TEXT("id")), *Bind.Data->GetStringField(TEXT("id")))));
		if (!TestTrue(TEXT("custom event delegate -> bind"), Wire.bOk)) AddError(Errors(Wire));
	}

	// Interface created on the fly and implemented
	const FString InterfaceName = UniqueName(TEXT("BPI_Test"));
	FBABResult Interface = Run(TEXT("bp_create_blueprint"), FString::Printf(TEXT("{\"path\":\"%s\",\"name\":\"%s\",\"blueprint_type\":\"interface\"}"), *Path, *InterfaceName));
	if (!TestTrue(TEXT("create interface"), Interface.bOk)) AddError(Errors(Interface));
	FBABResult InterfaceFn = Run(TEXT("bp_add_function"), ParamsFor(Path + TEXT("/") + InterfaceName, TEXT("\"name\":\"Interact\",\"inputs\":[{\"name\":\"Instigator\",\"type\":\"object:Actor\"}]")));
	if (!TestTrue(TEXT("interface function"), InterfaceFn.bOk)) AddError(Errors(InterfaceFn));
	FBABResult CompileInterface = Run(TEXT("bp_compile"), ParamsFor(Path + TEXT("/") + InterfaceName, TEXT("")));
	TestTrue(TEXT("interface compiles"), CompileInterface.bOk);
	FBABResult Implement = Run(TEXT("bp_add_interface"), ParamsFor(BP, FString::Printf(TEXT("\"interface\":\"%s\""), *(Path + TEXT("/") + InterfaceName))));
	if (!TestTrue(TEXT("implement interface"), Implement.bOk)) AddError(Errors(Implement));
	FBABResult InteractEvent = Run(TEXT("bp_add_node"), ParamsFor(BP, TEXT("\"node\":{\"type\":\"event\",\"name\":\"Interact\"}")));
	if (!TestTrue(TEXT("interface event placed"), InteractEvent.bOk)) AddError(Errors(InteractEvent));

	// Timeline with a float track
	FBABResult Timeline = Run(TEXT("bp_add_node"), ParamsFor(BP, TEXT("\"node\":{\"type\":\"timeline\",\"name\":\"DoorTimeline\"}")));
	if (!TestTrue(TEXT("timeline node"), Timeline.bOk)) AddError(Errors(Timeline));
	FBABResult Tracks = Run(TEXT("bp_update_timeline"), ParamsFor(BP, TEXT("\"timeline\":\"DoorTimeline\",\"length\":1.5,\"tracks\":[{\"name\":\"Alpha\",\"type\":\"float\",\"keys\":[[0,0],[1.5,1]],\"interp\":\"cubic\"},{\"name\":\"Offset\",\"type\":\"vector\",\"keys\":[[0,[0,0,0]],[1.5,[0,0,100]]]}]")));
	if (!TestTrue(TEXT("timeline tracks"), Tracks.bOk)) AddError(Errors(Tracks));
	FBABResult TimelineNode = Run(TEXT("bp_get_node"), ParamsFor(BP, TEXT("\"node\":\"DoorTimeline\"")));
	TestTrue(TEXT("timeline node exposes Alpha pin"), TimelineNode.bOk && TimelineNode.Data->GetStringField(TEXT("text")).Contains(TEXT("> Alpha")));
	TestTrue(TEXT("timeline node exposes Offset pin"), TimelineNode.bOk && TimelineNode.Data->GetStringField(TEXT("text")).Contains(TEXT("> Offset")));
	FBABResult GetTimeline = Run(TEXT("bp_get_timeline"), ParamsFor(BP, TEXT("\"timeline\":\"DoorTimeline\"")));
	TestTrue(TEXT("timeline length"), GetTimeline.bOk && FMath::IsNearlyEqual((float)GetTimeline.Data->GetNumberField(TEXT("length")), 1.5f));

	// Class defaults
	FBABResult SetDefault = Run(TEXT("bp_set_class_default"), ParamsFor(BP, TEXT("\"property\":\"bReplicates\",\"value\":true")));
	if (!TestTrue(TEXT("set class default"), SetDefault.bOk)) AddError(Errors(SetDefault));
	FBABResult GetDefaults = Run(TEXT("bp_get_class_defaults"), ParamsFor(BP, TEXT("")));
	const TSharedPtr<FJsonObject>* Defaults = nullptr;
	TestTrue(TEXT("bReplicates listed as modified"), GetDefaults.bOk && GetDefaults.Data->TryGetObjectField(TEXT("defaults"), Defaults) && Defaults && (*Defaults)->HasField(TEXT("bReplicates")));

	// Everything must compile
	FBABResult Compile = Run(TEXT("bp_compile"), ParamsFor(BP, TEXT("")));
	if (!TestTrue(TEXT("actor compiles"), Compile.bOk)) AddError(Errors(Compile));
	if (Compile.bOk) TestEqual(TEXT("no errors"), (int32)Compile.Data->GetNumberField(TEXT("num_errors")), 0);

	// Describe mentions everything
	FBABResult Describe = Run(TEXT("bp_describe_blueprint"), ParamsFor(BP, TEXT("")));
	const FString Text = Describe.Data->GetStringField(TEXT("text"));
	TestTrue(TEXT("describe lists Trigger"), Text.Contains(TEXT("Trigger : BoxComponent")));
	TestTrue(TEXT("describe lists OnOpened"), Text.Contains(TEXT("OnOpened")));
	TestTrue(TEXT("describe lists the interface"), Text.Contains(InterfaceName));
	TestTrue(TEXT("describe lists the timeline track"), Text.Contains(TEXT("Alpha")));

	// Remove the component that the overlap event uses: refused, then forced
	FBABResult RemoveTrigger = Run(TEXT("bp_remove_component"), ParamsFor(BP, TEXT("\"component\":\"Trigger\"")));
	TestFalse(TEXT("removing a used component is refused"), RemoveTrigger.bOk);
	FBABResult RemoveTriggerForced = Run(TEXT("bp_remove_component"), ParamsFor(BP, TEXT("\"component\":\"Trigger\",\"force\":true")));
	TestTrue(TEXT("forced component removal"), RemoveTriggerForced.bOk);
	FBABResult Compile2 = Run(TEXT("bp_compile"), ParamsFor(BP, TEXT("")));
	TestTrue(TEXT("compiles after component removal"), Compile2.bOk && (int32)Compile2.Data->GetNumberField(TEXT("num_errors")) == 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
