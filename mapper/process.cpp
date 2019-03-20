#include "stdafx.h"
#include "ntdll.hpp"

process::operator bool()
{
	return static_cast<bool>(this->handle);
}

process process::current_process()
{
	return process(reinterpret_cast<HANDLE>(-1));
}

uint32_t process::from_name(const std::string& process_name)
{
	DWORD process_list[516], bytes_needed;
	if (EnumProcesses(process_list, sizeof(process_list), &bytes_needed))
	{
		for (size_t index = 0; index < bytes_needed / sizeof(uint32_t); index++)
		{
			auto proc = process(process_list[index], 0x1fffff);

			if (!proc)
				continue;

			if (process_name == proc.get_name())
				return process_list[index];
		}
	}

	return 0;
}

MEMORY_BASIC_INFORMATION process::virtual_query(const uintptr_t address)
{
	MEMORY_BASIC_INFORMATION mbi;

	VirtualQueryEx(this->handle.get_handle(), reinterpret_cast<LPCVOID>(address), &mbi, sizeof(MEMORY_BASIC_INFORMATION));

	return mbi;
}

uintptr_t process::raw_allocate(const SIZE_T virtual_size, const uintptr_t address)
{
	return reinterpret_cast<uintptr_t>(
		VirtualAllocEx(this->handle.get_handle(), reinterpret_cast<LPVOID>(address), virtual_size, MEM_COMMIT, PAGE_EXECUTE_READWRITE)
		);
}

bool process::free_memory(const uintptr_t address)
{
	auto query = virtual_query(address);
	return VirtualFreeEx(this->handle.get_handle(), reinterpret_cast<LPVOID>(address), query.RegionSize, MEM_RELEASE);
}


bool process::read_raw_memory(void* buffer, const uintptr_t address, const SIZE_T size)
{
	return ReadProcessMemory(this->handle.get_handle(), reinterpret_cast<LPCVOID>(address), buffer, size, nullptr);
}

bool process::write_raw_memory(void* buffer, const SIZE_T size, const uintptr_t address)
{
	return WriteProcessMemory(this->handle.get_handle(), reinterpret_cast<LPVOID>(address), buffer, size, nullptr);
}

bool process::virtual_protect(const uintptr_t address, uint32_t protect, uint32_t* old_protect)
{
	return VirtualProtectEx(this->handle.get_handle(), reinterpret_cast<LPVOID>(address), 0x1000, protect, reinterpret_cast<PDWORD>(old_protect));
}

uintptr_t process::map(memory_section& section)
{
	void* base_address = nullptr;
	SIZE_T view_size = section.size;
	auto result = ntdll::NtMapViewOfSection(section.handle.get_handle(), this->handle.get_handle(), &base_address, NULL, NULL, NULL, &view_size, 2, 0, section.protection);

	if (!NT_SUCCESS(result))
	{
		logger::log_error("NtMapViewOfSection failed");
		logger::log_formatted("Error code", result, true);
	}

	return reinterpret_cast<uintptr_t>(base_address);
}



uint32_t process::get_id()
{
	return GetProcessId(this->handle.get_handle());
}

uintptr_t process::get_base_address()
{
	std::string process_name = this->get_name();
	std::transform(process_name.begin(), process_name.end(), process_name.begin(), ::tolower);

	for (auto&[name, module_handle] : this->get_modules())
	{
		if (name == process_name)
			return module_handle;
	}

	return 0;
}

std::unordered_map<std::string, uintptr_t> process::get_modules()
{
	auto result = std::unordered_map<std::string, uintptr_t>();

	HMODULE module_handles[1024];
	DWORD size_needed;

	if (EnumProcessModules(this->handle.get_handle(), module_handles, sizeof(module_handles), &size_needed))
	{
		for (auto i = 0; i < size_needed / sizeof(HMODULE); i++)
		{
			CHAR szModName[MAX_PATH];
			GetModuleBaseNameA(this->handle.get_handle(), module_handles[i], szModName, MAX_PATH);

			std::string new_name = szModName;
			std::transform(new_name.begin(), new_name.end(), new_name.begin(), ::tolower);

			result[new_name] = reinterpret_cast<uintptr_t>(module_handles[i]);
		}
	}

	return result;
}

std::string& process::get_name()
{
	char buffer[MAX_PATH];
	GetModuleBaseNameA(handle.get_handle(), nullptr, buffer, MAX_PATH);

	return std::string(buffer);
}


uintptr_t process::get_import(const std::string& module_name, const std::string& function_name)
{
	auto image_base = this->get_base_address();
	IMAGE_DOS_HEADER dos_header;
	IMAGE_NT_HEADERS64 nt_header;
	this->read_memory(&dos_header, image_base);
	this->read_memory(&nt_header, image_base + dos_header.e_lfanew);

	IMAGE_IMPORT_DESCRIPTOR import_table;
	auto import_table_address = image_base + nt_header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;

	this->read_memory(&import_table, import_table_address);
	for (; import_table.Name; import_table_address += sizeof(IMAGE_IMPORT_DESCRIPTOR), this->read_memory(&import_table, import_table_address))
	{
		char name_buffer[25];
		this->read_raw_memory(name_buffer, image_base + import_table.Name, 15);
		std::string current_module_name = name_buffer;

		if (module_name != current_module_name)
			continue;

		IMAGE_THUNK_DATA64 entry;
		auto entry_address = image_base + import_table.OriginalFirstThunk;
		this->read_memory(&entry, entry_address);
		uintptr_t index = 0;

		for (; entry.u1.AddressOfData; index += sizeof(uintptr_t), entry_address += sizeof(IMAGE_THUNK_DATA64), this->read_memory(&entry, entry_address))
		{
			IMAGE_IMPORT_BY_NAME import_by_name;
			this->read_memory(&import_by_name, image_base + entry.u1.AddressOfData);

			char function_name_buffer[30];
			this->read_raw_memory(function_name_buffer, image_base + entry.u1.AddressOfData + sizeof(WORD), 30);

			if (function_name_buffer == function_name)
				return image_base + import_table.FirstThunk + index;
		}
	}

	return 0;
}

// thx blackbone :)
uintptr_t process::get_module_export(uintptr_t module_handle, const char* function_ordinal)
{
	IMAGE_DOS_HEADER dos_header;
	IMAGE_NT_HEADERS64 nt_header;
	this->read_memory(&dos_header, module_handle);
	this->read_memory(&nt_header, module_handle + dos_header.e_lfanew);

	auto export_base = nt_header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
	auto export_base_size = nt_header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
	if (export_base) // CONTAINS EXPORTED FUNCTIONS
	{
		std::unique_ptr<IMAGE_EXPORT_DIRECTORY> export_data_raw(reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(malloc(export_base_size)));
		auto export_data = export_data_raw.get();

		// READ EXPORTED DATA FROM TARGET PROCESS FOR LATER PROCESSING
		if (!this->read_raw_memory(export_data, module_handle + export_base, export_base_size))
			logger::log_error("failed to read export data");

		// BLACKBONE PASTE, NEVER EXPERIENCED THIS BUT WHO KNOWS?
		if (export_base_size <= sizeof(IMAGE_EXPORT_DIRECTORY))
		{
			export_base_size = static_cast<DWORD>(export_data->AddressOfNameOrdinals - export_base
				+ max(export_data->NumberOfFunctions, export_data->NumberOfNames) * 255);

			export_data_raw.reset(reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(malloc(export_base_size)));
			export_data = export_data_raw.get();

			if (!this->read_raw_memory(export_data, module_handle + export_base, export_base_size))
				logger::log_error("failed to read export data");
		}

		// GET DATA FROM READ MEMORY
		auto delta = reinterpret_cast<uintptr_t>(export_data) - export_base;
		auto address_of_ordinals = reinterpret_cast<WORD*>(export_data->AddressOfNameOrdinals + delta);
		auto address_of_names = reinterpret_cast<DWORD*>(export_data->AddressOfNames + delta);
		auto address_of_functions = reinterpret_cast<DWORD*>(export_data->AddressOfFunctions + delta);

		// NO EXPORTED FUNCTIONS? DID WE FUCK UP?
		if (export_data->NumberOfFunctions <= 0)
			logger::log_error("No exports found!");

		logger::log_formatted("Exports", export_data->NumberOfFunctions);

		for (size_t i = 0; i < export_data->NumberOfFunctions; i++)
		{
			WORD ordinal;
			std::string function_name;
			auto is_import_by_ordinal = reinterpret_cast<uintptr_t>(function_ordinal) <= 0xFFFF;

			// GET EXPORT INFORMATION
			ordinal = static_cast<WORD>(is_import_by_ordinal ? i : address_of_ordinals[i]);
			function_name = reinterpret_cast<char*>(address_of_names[i] + delta);

			//logger::log_formatted("Ordinal", ordinal);
			//logger::log_formatted("Name", function_name);

			// IS IT THE FUNCTION WE ASKED FOR?
			auto found_via_ordinal = is_import_by_ordinal && (WORD)((uintptr_t)function_ordinal) == (ordinal + export_data->Base);
			auto found_via_name = !is_import_by_ordinal && function_name == function_ordinal;

			if (found_via_ordinal || found_via_name)
			{
				auto function_pointer = module_handle + address_of_functions[ordinal];

				// FORWARDED EXPORT?
				// IF FUNCTION POINTER IS INSIDE THE EXPORT DIRECTORY, IT IS *NOT* A FUNCTION POINTER!
				// FUCKING SHIT MSVCP140 FUCK YOU
				if (function_pointer >= module_handle + export_base && function_pointer <= module_handle + export_base + export_base_size)
				{
					char forwarded_name[255] = { 0 };
					this->read_raw_memory(forwarded_name, function_pointer, sizeof(forwarded_name));

					std::string forward(forwarded_name);
					std::string library_name = forward.substr(0, forward.find(".")) + ".dll";
					function_name = forward.substr(forward.find(".") + 1, function_name.npos);

					// LOWERCASE THANKS
					std::transform(library_name.begin(), library_name.end(), library_name.begin(), ::tolower);

					auto modules = this->get_modules();
					auto search = modules.find(library_name);
					if (search != modules.end())
						return this->get_module_export(search->second, function_name.c_str());
					else
						logger::log_error("Forwarded module not loaded"); // TODO: HANDLE THIS? WHO CARES
				}

				return function_pointer;
			}
		}
	}
	else
	{
		logger::log_error("no export base!");
	}

	logger::log_error("Export not found!");

	return 0;
}

bool process::hook_function(const std::string& module_name, const std::string& function_name, const uintptr_t hook_pointer)
{
	logger::log_formatted("Hooking", function_name);
	auto entry = this->get_import(module_name, function_name);

	if (!entry)
		return false;

	// READ ORIGINAL ENTRY FOR LATER POTENTIAL RECOVERY
	uintptr_t function_address;
	this->read_memory(&function_address, entry);

	// OVERWRITE IAT ENTRY
	if (!this->write_memory(hook_pointer, entry))
		return false;

	// STORE ORIGINAL IAT ENTRY
	this->hooks.emplace(hook_pointer, function_address);

	return true;
}

bool process::unhook_function(const std::string& module_name, const std::string& function_name, const uintptr_t hook_pointer)
{
	logger::log_formatted("Unhooking", hook_pointer, true);
	auto original_function = this->hooks.at(hook_pointer);

	// HAVE WE HOOKED THIS YET?
	if (!original_function)
		return false;

	// FIND CURRENT ENTRY
	auto entry = this->get_import(module_name, function_name);

	// IF WE DID, WRITE ORIGINAL ENTRY
	if (!entry || !this->write_memory(original_function, entry))
		return false;

	// RESET HOOK STORAGE ;)
	this->hooks.at(hook_pointer) = 0;

	return true;
}

HANDLE process::create_thread(const uintptr_t address, const uintptr_t argument)
{
	return CreateRemoteThread(this->handle.get_handle(), nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(address), reinterpret_cast<LPVOID>(argument), 0, nullptr);
}
