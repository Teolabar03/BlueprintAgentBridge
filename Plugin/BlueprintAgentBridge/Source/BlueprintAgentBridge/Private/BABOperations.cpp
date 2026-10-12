// Copyright (c) 2026 Matteo La Barbera. MIT License.

#include "BABDispatcher.h"

void BABRegisterOperations_Assets();
void BABRegisterOperations_Graph();
void BABRegisterOperations_Members();

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
}
