/*
* This file is part of Gix-IDE, an IDE and platform for GnuCOBOL
* Copyright (C) 2021 Marco Ridoni
*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public License
* as published by the Free Software Foundation; either version 3,
* or (at your option) any later version.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with this library; see the file COPYING.LIB.  If
* not, write to the Free Software Foundation, 51 Franklin Street, Fifth Floor
* Boston, MA 02110-1301 USA
*/

#include "DbInterfaceFactory.h"

#include <stdlib.h>
#include <cstring>

#include "gixsql.h"

//static std::map<std::shared_ptr<IDbInterface>, LIBHANDLE> lib_map;

typedef IDbInterface * (*DBLIB_PROVIDER_FUNC)();

#if defined(_WIN32)
//Returns the last Win32 error, in string format. Returns an empty string if there is no error.
std::string GetLastErrorAsString()
{
	//Get the error message ID, if any.
	DWORD errorMessageID = ::GetLastError();
	if (errorMessageID == 0) {
		return std::string(); //No error message has been recorded
	}

	LPSTR messageBuffer = nullptr;

	//Ask Win32 to give us the string version of that message ID.
	//The parameters we pass in, tell Win32 to create the buffer that holds the message for us (because we don't yet know how long the message string will be).
	size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, errorMessageID, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);

	//Copy the error message into a std::string.
	std::string message(messageBuffer, size);

	//Free the Win32's string's buffer.
	LocalFree(messageBuffer);

	return message;
}
#endif

std::shared_ptr<IDbInterface> DbInterfaceFactory::getInterface(int type, const std::shared_ptr<spdlog::logger>& _logger)
{
	switch (type) {
		case DB_PGSQL:
			return load_dblib("pgsql");

		case DB_ODBC:
			return load_dblib("odbc");

		case DB_MYSQL:
			return load_dblib("mysql");

		case DB_ORACLE:
			return load_dblib("oracle");

		case DB_SQLITE:
			return load_dblib("sqlite");

		default:
			return NULL;
	}
}

std::shared_ptr<IDbInterface> DbInterfaceFactory::getInterface(std::string t, const std::shared_ptr<spdlog::logger>& _logger)
{
		if (t == "pgsql")
			return load_dblib("pgsql");

		if (t == "odbc")
			return load_dblib("odbc");

		if (t == "mysql")
			return load_dblib("mysql");

		if (t == "oracle")
			return load_dblib("oracle");

		if (t == "sqlite")
			return load_dblib("sqlite");

		return NULL;
}

IDbManagerInterface* DbInterfaceFactory::getManagerInterface(int type)
{
	return dynamic_cast<IDbManagerInterface *>(getManagerInterface(type));
}

IDbManagerInterface* DbInterfaceFactory::getManagerInterface(std::string type)
{
	return dynamic_cast<IDbManagerInterface *>(getManagerInterface(type));
}

std::shared_ptr<IDbInterface> DbInterfaceFactory::load_dblib(const char *lib_id)
{
	char bfr[256];
	std::shared_ptr<IDbInterface> dbi;
	LIBHANDLE libHandle = NULL;
	DBLIB_PROVIDER_FUNC dblib_provider;

	sprintf(bfr, "libgixsql-");
	strcat(bfr, lib_id);

#if defined(_WIN32)

	strcat(bfr, ".dll");
	spdlog::debug(FMT_FILE_FUNC "loading DB provider: {}", __FILE__, __func__, bfr);

	libHandle = LoadLibrary(bfr);
	spdlog::trace(FMT_FILE_FUNC "library handle is: {}", __FILE__, __func__, (void *) libHandle);

	if (libHandle != NULL)
	{
		dblib_provider = (DBLIB_PROVIDER_FUNC)GetProcAddress(libHandle, "get_dblib");
		spdlog::debug(FMT_FILE_FUNC "accessing DB provider: {}", __FILE__, __func__, bfr);
		// If the function address is valid, call the function. 
		if (dblib_provider != NULL)
		{
#if _DEBUG
			char dll_path[MAX_PATH];
			int rc = GetModuleFileName(libHandle, dll_path, MAX_PATH);
			if (rc) {
				spdlog::debug(FMT_FILE_FUNC "DB provider loaded from: {}", __FILE__, __func__, dll_path);
			}
#endif

#if defined(_DEBUG) && defined(VERBOSE)
			dbi = std::shared_ptr<IDbInterface>(dblib_provider(), [](IDbInterface* p) {
				fprintf(stderr, "- Deallocated IDbInterface: 0x%p\n", p);
				delete p;
			});
			fprintf(stderr, "+ Allocated IDbInterface: 0x%p\n", dbi.get());
#else
			dbi.reset(dblib_provider());
#endif
		}
		else {
			spdlog::error("ERROR while accessing DB provider: {}", bfr);
		}

		// Library not freed here
	}
	else {
		auto err = GetLastErrorAsString();
		spdlog::error("ERROR while loading DB provider {}: {}", bfr, err);
#if _DEBUG
		spdlog::error("PATH is: {}", getenv("PATH"));
#endif
	}

#else

	strcat(bfr, ".so");
	spdlog::debug(FMT_FILE_FUNC "loading DB provider: {}", __FILE__, __func__, bfr);

	libHandle = dlopen(bfr, RTLD_NOW);
	if (libHandle != NULL)
	{
		dblib_provider = (DBLIB_PROVIDER_FUNC)dlsym(libHandle, "get_dblib");
		spdlog::debug(FMT_FILE_FUNC "Accessing DB provider: {}", __FILE__, __func__, bfr);
		// If the function address is valid, call the function. 
		if (dblib_provider != NULL)
		{
#if defined(_DEBUG) && defined(VERBOSE)
			dbi = std::shared_ptr<IDbInterface>(dblib_provider(), [](IDbInterface *p) { 
				fprintf(stderr, "- Deallocated IDbInterface: 0x%p\n", p);
				delete p; 
			});
			fprintf(stderr, "+ Allocated IDbInterface: 0x%p\n", dbi.get());
#else
			dbi.reset(dblib_provider());
#endif
		}
		else {
			spdlog::error("ERROR while accessing DB provider: {}", bfr);
		}

		// Library not freed here
	}
	else {
        	spdlog::error("ERROR while loading DB provider: {} ({})", bfr, dlerror());
	}

#endif

	dbi->native_lib_ptr = (void *) libHandle;

	if (dbi != nullptr) {
		dbi->init(gixsql_logger);
	}
	return dbi;
}

// TODO: this should really be generated dynamically
std::vector<std::string> DbInterfaceFactory::getAvailableDrivers()
{
	return std::vector<std::string> { "odbc", "mysql", "pgsql", "oracle", "sqlite" } ;
}

void DbInterfaceFactory::releaseInterface(std::shared_ptr<IDbInterface> dbi)
{
	if (dbi && dbi->native_lib_ptr) {
#if defined(_WIN32)
		FreeLibrary((HMODULE)dbi->native_lib_ptr);
#else
		dlclose(dbi->native_lib_ptr);
#endif
	}		
}
