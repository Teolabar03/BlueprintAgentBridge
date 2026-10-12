// Copyright (c) 2026 Matteo La Barbera. MIT License.

#include "BABDispatcher.h"

void BABRegisterOperations_Assets();
void BABRegisterOperations_Graph();
void BABRegisterOperations_Members();
void BABRegisterOperations_Editor();
void BABRegisterOperations_Components();

void BABRegisterAllOperations()
{
	static bool bRegistered = false;
	if (bRegistered)
	{
		return;
	}
	bRegistered = true;
	BABRegisterOperations_Assets();
	BABRegisterOperations_Graph();
	BABRegisterOperations_Members();
	BABRegisterOperations_Editor();
	BABRegisterOperations_Components();

	// Everything else is an edit and runs inside an undo transaction when an editor is open.
	FBABDispatcher& D = FBABDispatcher::Get();
	for (const TCHAR* Name : { TEXT("bp_status"), TEXT("bp_list_operations"), TEXT("bp_list_assets"), TEXT("bp_describe_blueprint"), TEXT("bp_read_graph"), TEXT("bp_get_node"), TEXT("bp_search_nodes"), TEXT("bp_export_text"), TEXT("bp_get_component"), TEXT("bp_get_class_defaults"), TEXT("bp_get_timeline") })
	{
		D.SetKind(Name, EBABOpKind::ReadOnly);
	}
	for (const TCHAR* Name : { TEXT("bp_compile"), TEXT("bp_save"), TEXT("bp_create_blueprint") })
	{
		D.SetKind(Name, EBABOpKind::NonTransactional);
	}
}
