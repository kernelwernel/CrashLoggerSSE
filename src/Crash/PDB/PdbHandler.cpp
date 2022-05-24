// SPDX-License-Identifier: CC-BY-SA-4.0
// Code from StackOverflow


#pragma once
#include "PdbHandler.h"
#include "Config.h"
#include <comdef.h>

namespace Crash
{
	namespace PDB
	{
		std::atomic<bool> symcacheChecked = false;
		std::atomic<bool> symcacheValid = false;

		//https://stackoverflow.com/questions/6284524/bstr-to-stdstring-stdwstring-and-vice-versa
		std::string ConvertWCSToMBS(const wchar_t* pstr, long wslen)
		{
			int len = ::WideCharToMultiByte(CP_ACP, 0, pstr, wslen, NULL, 0, NULL, NULL);

			std::string dblstr(len, '\0');
			len = ::WideCharToMultiByte(CP_ACP, 0 /* no flags */,
				pstr, wslen /* not necessary NULL-terminated */,
				&dblstr[0], len,
				NULL, NULL /* no default char */);

			return dblstr;
		}

		std::string ConvertBSTRToMBS(BSTR bstr)
		{
			int wslen = ::SysStringLen(bstr);
			return ConvertWCSToMBS((wchar_t*)bstr, wslen);
		}

		BSTR ConvertMBSToBSTR(const std::string& str)
		{
			int wslen = ::MultiByteToWideChar(CP_ACP, 0 /* no flags */,
				str.data(), str.length(),
				NULL, 0);

			BSTR wsdata = ::SysAllocStringLen(NULL, wslen);
			::MultiByteToWideChar(CP_ACP, 0 /* no flags */,
				str.data(), str.length(),
				wsdata, wslen);
			return wsdata;
		}

		std::string processSymbol(IDiaSymbol* a_symbol, IDiaSession* a_session, const DWORD& a_rva, std::string_view& a_name, uintptr_t& a_offset, std::string& a_result)
		{
			BSTR name;
			a_symbol->get_name(&name);
			auto convertedName = ConvertBSTRToMBS(name);

			ULONGLONG length = 0;
			if (a_symbol->get_length(&length) == S_OK) {
				IDiaEnumLineNumbers* lineNums[100];
				if (a_session->findLinesByRVA(a_rva, length, lineNums) == S_OK) {
					auto& l = lineNums[0];
					CComPtr<IDiaLineNumber> line;
					IDiaLineNumber* lineNum;
					ULONG fetched = 0;
					bool found_source = false;
					bool found_line = false;
					for (uint8_t i = 0; i < 5; ++i) {
						if (l->Next(i, &lineNum, &fetched) == S_OK && fetched == 1) {
							found_source = false;
							found_line = false;
							DWORD l;
							IDiaSourceFile* srcFile;
							BSTR fileName = nullptr;
							std::string convertedFileName;
							if (lineNum->get_sourceFile(&srcFile) == S_OK) {
								BSTR fileName;
								srcFile->get_fileName(&fileName);
								convertedFileName = ConvertBSTRToMBS(fileName);
								found_source = true;
							}
							if (lineNum->get_lineNumber(&l) == S_OK)
								found_line = true;
							if (found_source && found_line)  // this should always hit if hit at all
								a_result += fmt::format(" {}:{} {}", convertedFileName, +l, convertedName);
							else if (found_source)
								a_result += fmt::format(" {} {}", convertedFileName, convertedName);
							else if (found_line)
								a_result += fmt::format(" unk_:{} {}", +l, convertedName);
						}
					}
				}
			}
			if (a_result.empty())
				logger::info("No symbol found for {}+{:07X}"sv, a_name, a_offset);
			else
				logger::info("Symbol returning: {}", a_result);
			return a_result;
		}
		std::string print_hr_failure(HRESULT hr) {
			auto errMsg = "";
			switch ((unsigned int)hr) {
			case 0x806D0005:  // E_PDB_NOT_FOUND
				errMsg = "Unable to locate PDB";
				break;
			case 0x806D0012:  // E_PDB_FORMAT
			case 0x806D0014:  // E_PDB_NO_DEBUG_INFO
				errMsg = "Invalid or obsolete file format";
				break;
			default:
				_com_error err(hr);
				errMsg = CT2A(err.ErrorMessage());
			}
			return errMsg;
		}

		//https://stackoverflow.com/questions/68412597/determining-source-code-filename-and-line-for-function-using-visual-studio-pdb
		std::string pdb_details(std::string_view a_name, uintptr_t a_offset)
		{
			std::string result = "";
			if (a_name.ends_with("exe")) //ignore exe since pdbs not readily available for bethesda exes
				return result;
			std::string pluginPath = "Data/SKSE/Plugins/";
			std::filesystem::path dllPath { a_name };
			std::string dll_path = a_name.data();
			if (!dllPath.has_parent_path())
				dll_path = pluginPath + dllPath.filename().string();
			auto rva = (DWORD)a_offset;
			CComPtr<IDiaDataSource> pSource;
			auto hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
			if (FAILED(hr)) {
				auto error = print_hr_failure(hr);
				logger::info("Failed to initalize COM library for dll {}+{:07X}\t{}", a_name, a_offset, error);
				CoUninitialize();
				return result;
			}

			// try to load local copy
			auto* msdia_dll = L"Data/SKSE/Plugins/msdia140.dll";
			hr = NoRegCoCreate(msdia_dll, CLSID_DiaSource, __uuidof(IDiaDataSource),(void**)&pSource);
			if (FAILED(hr)) {
					auto error = print_hr_failure(hr);
					logger::info("Failed to manually load msdia140.dll and create object for CLSID for dll {}+{:07X}\t{}", a_name, a_offset, error);
					// msdia*.dll try registered copy
					if (FAILED(hr = CoCreateInstance(CLSID_DiaSource,
								   NULL,
								   CLSCTX_INPROC_SERVER,
								   __uuidof(IDiaDataSource),
								   (void**)&pSource))) {
						auto error = print_hr_failure(hr);
						logger::info("Failed to load registered msdia140.dll for dll {}+{:07X}\t{}", a_name, a_offset, error);
						CoUninitialize();
						return result;
					}
			}

			wchar_t wszFilename[_MAX_PATH];
			wchar_t wszPath[_MAX_PATH];
			mbstowcs(wszFilename, dll_path.c_str(), sizeof(wszFilename) / sizeof(wszFilename[0]));
			const auto& debugConfig = CrashLogger::Config::GetSingleton().GetDebug();
			auto symcache = debugConfig.GetSymcache();
			if (!symcacheChecked) {
				if (!symcache.empty() && std::filesystem::exists(symcache) && std::filesystem::is_directory(symcache)) {
					logger::info("Symcache found at {}", symcache);
					symcacheValid = true;
				} else if (!symcache.empty()) {
					logger::info("Symcache not found at {}", symcache);
				}else
					logger::info("Symcache not defined");
				symcacheChecked = true;
			}
			if (symcacheValid)
				mbstowcs(wszPath, fmt::format("cache*{}"s, symcache).c_str(), sizeof(wszPath) / sizeof(wszPath[0]));
			logger::info("Attempting to find pdb for {}+{:07X}{}", a_name, a_offset);
			hr = pSource->loadDataForExe(wszFilename, wszPath, NULL);
			if (FAILED(hr)) {
				auto error = print_hr_failure(hr);
				logger::info("Failed to open pdb for dll {}+{:07X}\t{}", a_name, a_offset, error);
				CoUninitialize();
				return result;
			}
			logger::info("Successfully opened pdb for dll {}+{:07X}", a_name, a_offset);

			IDiaSession* pSession;
			IDiaSymbol* globalSymbol = nullptr;
			IDiaEnumTables* enumTables = nullptr;
			IDiaEnumSymbolsByAddr* enumSymbolsByAddr = nullptr;
			hr = pSource->openSession(&pSession);
			if (FAILED(hr)) {
				auto error = print_hr_failure(hr);
				logger::info("Failed to open IDiaSession for pdb for dll {}+{:07X}\t{}", a_name, a_offset, error);
				CoUninitialize();
				return result;
			}
			hr = pSession->get_globalScope(&globalSymbol);
			if (FAILED(hr)) {
				auto error = print_hr_failure(hr);
				logger::info("Failed to getGlobalScope for pdb for dll {}+{:07X}\t{}", a_name, a_offset, error);
				CoUninitialize();
				return result;
			}

			hr = pSession->getEnumTables(&enumTables);
			if (FAILED(hr)) {
				auto error = print_hr_failure(hr);
				logger::info("Failed to getEnumTables for pdb for dll {}+{:07X}\t{}", a_name, a_offset, error);
				CoUninitialize();
				return result;
			}
			hr = pSession->getSymbolsByAddr(&enumSymbolsByAddr);
			if (FAILED(hr)) {
				auto error = print_hr_failure(hr);
				logger::info("Failed to getSymbolsByAddr for pdb for dll {}+{:07X}\t{}", a_name, a_offset, error);
				CoUninitialize();
				return result;
			}
			IDiaSymbol* publicSymbol;
			if (pSession->findSymbolByRVA(rva, SymTagEnum::SymTagPublicSymbol, &publicSymbol) == S_OK) {
				auto publicResult = processSymbol(publicSymbol, pSession, rva, a_name, a_offset, result);
				DWORD privateRva;
				IDiaSymbol* privateSymbol;
				if (
					publicSymbol->get_targetRelativeVirtualAddress(&privateRva) == S_OK &&
					pSession->findSymbolByRVA(privateRva, SymTagEnum::SymTagFunction, &privateSymbol) == S_OK) {
					// Do stuff with private symbol
					auto privateResult = processSymbol(privateSymbol, pSession, privateRva, a_name, a_offset, result);
					if (!privateResult.empty() && !publicResult.empty()) {
						CoUninitialize();
						return fmt::format("{}\t{}", privateResult, publicResult);
					} else if (!privateResult.empty()) {
						CoUninitialize();
						return privateResult;
					}
				}
				CoUninitialize();
				return publicResult;
			}
			pSession->Release();
			CoUninitialize();
			return result;
		}

		// dump all symbols in Plugin directory
		// this was the early POC test and written first in this module
		void dump_symbols() {
			CoInitialize(nullptr);
			std::filesystem::path pluginDir{ "Data/SKSE/Plugins"sv };
			for (const auto& elem : std::filesystem::directory_iterator(pluginDir)) {
				if (const auto filename =
						elem.path().has_filename() ?
							std::make_optional(elem.path().filename().string()) :
			                      std::nullopt;
					filename.value().ends_with("dll")) {
					logger::info("Found dll {}", *filename);
					auto path = elem.path();
					auto dll_path = path.string();
					CComPtr<IDiaDataSource> source;
					auto hr = CoCreateInstance(CLSID_DiaSource,
						NULL,
						CLSCTX_INPROC_SERVER,
						__uuidof(IDiaDataSource),
						(void**)&source);
					if (FAILED(hr))
						continue;

					{
						wchar_t wide_path[MAX_PATH];
						memset(wide_path, 0, MAX_PATH * 2);
						MultiByteToWideChar(CP_ACP, 0, dll_path.c_str(), (int)dll_path.length(), wide_path, MAX_PATH);
						hr = source->loadDataForExe(wide_path, NULL, NULL);
						if (FAILED(hr))
							continue;
						logger::info("Found pdb for dll {}", *filename);
					}

					CComPtr<IDiaSession> pSession;
					if (FAILED(source->openSession(&pSession)))
						continue;

					IDiaEnumSymbolsByAddr* pEnumSymbolsByAddr;
					IDiaSymbol* pSymbol;
					ULONG celt = 0;
					if (FAILED(pSession->getSymbolsByAddr(&pEnumSymbolsByAddr))) {
						continue;
					}
					if (FAILED(pEnumSymbolsByAddr->symbolByAddr(1, 0, &pSymbol))) {
						pEnumSymbolsByAddr->Release();
						continue;
					}
					do {
						// Do something with symbol...
						//https://stackoverflow.com/questions/68412597/determining-source-code-filename-and-line-for-function-using-visual-studio-pdb
						BSTR name;
						pSymbol->get_name(&name);
						ULONGLONG length = 0;
						DWORD rva;
						pSymbol->get_relativeVirtualAddress(&rva);
						if (pSymbol->get_length(&length) == S_OK) {
							IDiaEnumLineNumbers* lineNums[100];
							if (pSession->findLinesByRVA(rva, length, lineNums) == S_OK) {
								auto& l = lineNums[0];
								CComPtr<IDiaLineNumber> line;
								IDiaLineNumber* lineNum;
								ULONG fetched = 0;
								bool found_source = false;
								bool found_line = false;
								for (uint8_t i = 0; i < 5; ++i) {
									if (l->Next(i, &lineNum, &fetched) == S_OK && fetched == 1) {
										IDiaSourceFile* srcFile;
										DWORD l;
										BSTR srcFileName=nullptr;
										if (lineNum->get_sourceFile(&srcFile) == S_OK) {
											if (found_source) logger::warn("MULTIPLE HITS");
											srcFile->get_fileName(&srcFileName);
											found_source = true;
										}
										if (lineNum->get_lineNumber(&l) == S_OK)
											found_line = true;
										if (found_source && found_line)  // this should always hit if hit at all
											logger::info("Dll: {} RVA:{} Name: {} SourceFileName: {}:{}", *filename, +rva, ConvertBSTRToMBS(name), ConvertBSTRToMBS(srcFileName), +l);
										else if (found_source)
											logger::info("Dll: {} RVA:{} Name: {} SourceFileName: {}", *filename, +rva, ConvertBSTRToMBS(name), ConvertBSTRToMBS(srcFileName));
										else if (found_line)
											logger::info("Dll: {} RVA:{} Name: {} Line: {}", *filename, +rva, ConvertBSTRToMBS(name), +l);
									}
								}
							}
						} else
							// no source or line number
							logger::info("Dll: {} RVA:{} Name: {}", *filename, +rva, ConvertBSTRToMBS(name));

						pSymbol->Release();
						if (FAILED(pEnumSymbolsByAddr->Next(1, &pSymbol, &celt))) {
							pEnumSymbolsByAddr->Release();
							break;
						}
					} while (celt == 1);
					pEnumSymbolsByAddr->Release();
				}
			}
		}
	}
}


