// Copyright (c) 2026 Matteo La Barbera. MIT License.

#pragma once

#include "CoreMinimal.h"

/** File system details the engine does not expose: private files and the physical location of a path. */
namespace BAB
{
	/**
	 * Writes Contents (UTF-8) to a new file that only the current user can read: on Windows an ACL
	 * granting the current user and SYSTEM alone, elsewhere mode 0600. An existing file is replaced.
	 */
	bool WritePrivateFile(const FString& Path, const FString& Contents, FString& OutError);

	/**
	 * Absolute path with symbolic links and junctions resolved. Components that do not exist yet
	 * are appended to the resolved path of their deepest existing parent. Returns false when the
	 * operating system cannot resolve it; callers deciding about writes must then refuse.
	 */
	bool ResolveRealPath(const FString& Path, FString& OutResolved);
}
