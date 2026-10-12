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
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
