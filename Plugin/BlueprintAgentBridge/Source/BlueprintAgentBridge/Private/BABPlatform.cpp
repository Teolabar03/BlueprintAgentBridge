// Copyright (c) 2026 Matteo La Barbera. MIT License.

#include "BABPlatform.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <sddl.h>
#include "Windows/HideWindowsPlatformTypes.h"
#else
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#endif

namespace BAB
{
#if PLATFORM_WINDOWS
	/** "D:P(A;;FA;;;<current user SID>)(A;;FA;;;SY)": full access for the user and SYSTEM, nothing inherited. */
	static bool CurrentUserOnlySddl(FString& OutSddl, FString& OutError)
	{
		HANDLE Token = nullptr;
		if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &Token))
		{
			OutError = FString::Printf(TEXT("OpenProcessToken failed (%u)."), ::GetLastError());
			return false;
		}
		DWORD Size = 0;
		::GetTokenInformation(Token, TokenUser, nullptr, 0, &Size);
		TArray<uint8> Buffer;
		Buffer.SetNumZeroed(Size);
		const bool bGotUser = Size > 0 && ::GetTokenInformation(Token, TokenUser, Buffer.GetData(), Size, &Size);
		::CloseHandle(Token);
		if (!bGotUser)
		{
			OutError = FString::Printf(TEXT("GetTokenInformation failed (%u)."), ::GetLastError());
			return false;
		}
		LPWSTR SidString = nullptr;
		if (!::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(Buffer.GetData())->User.Sid, &SidString))
		{
			OutError = FString::Printf(TEXT("ConvertSidToStringSid failed (%u)."), ::GetLastError());
			return false;
		}
		OutSddl = FString::Printf(TEXT("D:P(A;;FA;;;%s)(A;;FA;;;SY)"), SidString);
		::LocalFree(SidString);
		return true;
	}
#endif

	bool WritePrivateFile(const FString& Path, const FString& Contents, FString& OutError)
	{
		IFileManager& FileManager = IFileManager::Get();
		FileManager.MakeDirectory(*FPaths::GetPath(Path), true);
		// A new file gets the requested permissions; an existing one would keep its own.
		if (FileManager.FileExists(*Path) && !FileManager.Delete(*Path, false, true, true))
		{
			OutError = FString::Printf(TEXT("Cannot replace %s."), *Path);
			return false;
		}
		const FTCHARToUTF8 Utf8(*Contents);

#if PLATFORM_WINDOWS
		FString Sddl;
		if (!CurrentUserOnlySddl(Sddl, OutError))
		{
			return false;
		}
		PSECURITY_DESCRIPTOR Descriptor = nullptr;
		if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(*Sddl, SDDL_REVISION_1, &Descriptor, nullptr))
		{
			OutError = FString::Printf(TEXT("Cannot build the security descriptor (%u)."), ::GetLastError());
			return false;
		}
		SECURITY_ATTRIBUTES Attributes;
		Attributes.nLength = sizeof(Attributes);
		Attributes.lpSecurityDescriptor = Descriptor;
		Attributes.bInheritHandle = 0;
		const FString PlatformPath = FPaths::ConvertRelativePathToFull(Path).Replace(TEXT("/"), TEXT("\\"));
		HANDLE File = ::CreateFileW(*PlatformPath, GENERIC_WRITE, 0, &Attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		::LocalFree(Descriptor);
		if (File == INVALID_HANDLE_VALUE)
		{
			OutError = FString::Printf(TEXT("Cannot create %s (%u)."), *Path, ::GetLastError());
			return false;
		}
		DWORD Written = 0;
		const bool bWritten = ::WriteFile(File, Utf8.Get(), (DWORD)Utf8.Length(), &Written, nullptr) && Written == (DWORD)Utf8.Length();
		::CloseHandle(File);
#else
		const int Fd = ::open(TCHAR_TO_UTF8(*FPaths::ConvertRelativePathToFull(Path)), O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (Fd < 0)
		{
			OutError = FString::Printf(TEXT("Cannot create %s."), *Path);
			return false;
		}
		const bool bWritten = ::write(Fd, Utf8.Get(), Utf8.Length()) == Utf8.Length();
		::close(Fd);
#endif
		if (!bWritten)
		{
			OutError = FString::Printf(TEXT("Cannot write %s."), *Path);
			FileManager.Delete(*Path, false, true, true);
			return false;
		}
		return true;
	}

	static bool ResolveExisting(const FString& Existing, FString& OutResolved)
	{
#if PLATFORM_WINDOWS
		const FString PlatformPath = Existing.Replace(TEXT("/"), TEXT("\\"));
		HANDLE Handle = ::CreateFileW(*PlatformPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (Handle == INVALID_HANDLE_VALUE)
		{
			return false;
		}
		TCHAR Buffer[4096];
		const DWORD Length = ::GetFinalPathNameByHandleW(Handle, Buffer, UE_ARRAY_COUNT(Buffer), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		::CloseHandle(Handle);
		if (Length == 0 || Length >= UE_ARRAY_COUNT(Buffer))
		{
			return false;
		}
		FString Final(Buffer);
		if (Final.StartsWith(TEXT("\\\\?\\UNC\\")))
		{
			Final = TEXT("\\\\") + Final.Mid(8);
		}
		else if (Final.StartsWith(TEXT("\\\\?\\")))
		{
			Final = Final.Mid(4);
		}
		FPaths::NormalizeFilename(Final);
		OutResolved = Final;
		return true;
#else
		char Resolved[PATH_MAX];
		if (::realpath(TCHAR_TO_UTF8(*Existing), Resolved) == nullptr)
		{
			return false;
		}
		OutResolved = FString(UTF8_TO_TCHAR(Resolved));
		return true;
#endif
	}

	bool ResolveRealPath(const FString& Path, FString& OutResolved)
	{
		IFileManager& FileManager = IFileManager::Get();
		FString Existing = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Existing);
		while (Existing.EndsWith(TEXT("/")) && Existing.Len() > 1)
		{
			Existing.LeftChopInline(1);
		}
		FString Missing;
		while (!FileManager.DirectoryExists(*Existing) && !FileManager.FileExists(*Existing))
		{
			const FString Parent = FPaths::GetPath(Existing);
			if (Parent.IsEmpty() || Parent == Existing)
			{
				return false;
			}
			const FString Name = FPaths::GetCleanFilename(Existing);
			Missing = Missing.IsEmpty() ? Name : Name / Missing;
			Existing = Parent;
		}
		FString Resolved;
		if (!ResolveExisting(Existing, Resolved))
		{
			return false;
		}
		OutResolved = Missing.IsEmpty() ? Resolved : Resolved / Missing;
		return true;
	}
}
