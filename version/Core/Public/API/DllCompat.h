#pragma once

// Lets a plugin built with these headers run on both the current version.dll and an older one
// that predates the exports below.
//
// Every export added since the old version.dll is reached through a pointer looked up at runtime
// instead of the import table, so a plugin built here has no import the old version.dll lacks
// and loads on either. The first call checks once whether version.dll has the new exports:
// - it has: every call goes to the export, same result and speed as a direct import
// - it has not: the old std::string exports answer instead, with the same results the new ones
//   give (unknown names come back as null or false, never as a random address)
//
// Only included by Base.h. version.dll itself (ARK_EXPORTS) defines the real functions.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>

namespace API::DllCompat
{
#ifdef ARK_EXPORTS
	constexpr bool IsCurrentDll()
	{
		return true;
	}
#else
	enum Export : int
	{
		FindNativeOffsetExport,
		FindNativeBitFieldExport,
		GetFieldAddressExport,
		GetFunctionAddressExport,
		GetDataAddressExport,
		GetBitFieldConstExport,
		GetBitFieldExport,
		GetLogFlushLevelExport,
		LogDeprecatedUseExport,
		CancelPendingRequestsExport,
		CancelTimerExport,
		DelayExecuteWithIdExport,
		RecurringExecuteWithIdExport,
		ExportCount
	};

	// decorated names of the exports, in Export order
	inline constexpr const char* export_names[ExportCount] = {
		"?FindNativeOffset@@YA_NPEBDPEA_J@Z",
		"?FindNativeBitField@@YA_NPEBDPEAUBitField@@@Z",
		"?GetAddress@@YA_KPEBXPEBD@Z",
		"?GetAddress@@YAPEAXPEBD@Z",
		"?GetDataAddress@@YAPEAXPEBD@Z",
		"?GetBitField@@YA?AUBitField@@PEBXPEBD@Z",
		"?GetBitField@@YA?AUBitField@@PEAXPEBD@Z",
		"?GetLogFlushLevel@@YA?AW4level_enum@level@spdlog@@XZ",
		"?LogDeprecatedUse@API@@YAXPEBDPEAUHINSTANCE__@@@Z",
		"?CancelPendingRequests@Requests@API@@QEAAHXZ",
		"?CancelTimer@Timer@API@@QEAA_N_K@Z",
		"?DelayExecuteWithIdInternal@Timer@API@@AEAA_KAEBV?$function@$$A6AXXZ@std@@H@Z",
		"?RecurringExecuteWithIdInternal@Timer@API@@AEAA_KAEBV?$function@$$A6AXXZ@std@@HH_N@Z",
	};

	// constant initialized, so no init guard (plugins may build with /Zc:threadSafeInit-)
	inline std::atomic<int> mode{0}; // 0 not checked yet, 1 current version.dll, 2 old version.dll
	inline std::atomic<void*> exports[ExportCount]{};

	__declspec(noinline) inline int ResolveMode()
	{
		// the version.dll this plugin imports from: the one holding an export every version has
		using OldGetAddress = LPVOID (*)(const std::string&);
		const OldGetAddress old_export = &::GetAddress;

		HMODULE api = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                   reinterpret_cast<LPCSTR>(old_export), &api);

		void* found[ExportCount]{};
		bool current = api != nullptr;
		for (int i = 0; current && i < ExportCount; ++i)
		{
			found[i] = reinterpret_cast<void*>(GetProcAddress(api, export_names[i]));
			current = found[i] != nullptr;
		}

		if (current)
		{
			for (int i = 0; i < ExportCount; ++i)
				exports[i].store(found[i], std::memory_order_relaxed);
		}

		// racing threads store the same values
		const int result = current ? 1 : 2;
		mode.store(result, std::memory_order_release);
		return result;
	}

	// true when version.dll has the current exports
	inline bool IsCurrentDll()
	{
		const int m = mode.load(std::memory_order_acquire);
		return (m != 0 ? m : ResolveMode()) == 1;
	}

	// the export, or null on an old version.dll
	template <typename Fn>
	Fn Get(Export which)
	{
		return IsCurrentDll() ? reinterpret_cast<Fn>(exports[which].load(std::memory_order_relaxed)) : nullptr;
	}

	// set by Logger.h so the old version.dll path can report through the ArkApi log
	inline std::atomic<void (*)(const char*)> reporter{nullptr};

	// a name is reported once per plugin, tracked by hash without a lock
	inline std::atomic<uint64_t> reported[64]{};

	inline void ReportOnce(const char* what, const char* name)
	{
		uint64_t hash = 14695981039346656037ULL;
		for (const char* c = what; *c != '\0'; ++c)
			hash = (hash ^ static_cast<unsigned char>(*c)) * 1099511628211ULL;
		for (const char* c = name; *c != '\0'; ++c)
			hash = (hash ^ static_cast<unsigned char>(*c)) * 1099511628211ULL;
		hash |= 1;

		// a full table keeps reporting rather than going quiet
		for (auto& slot : reported)
		{
			uint64_t seen = slot.load(std::memory_order_relaxed);
			if (seen == 0 && slot.compare_exchange_strong(seen, hash))
				break;

			if (seen == hash)
				return;
		}

		const std::string message = std::string("[ArkApi] old version.dll: ") + what + " '" + name + "'";
		const auto report = reporter.load(std::memory_order_acquire);
		if (report != nullptr)
			report(message.c_str());
		else
			OutputDebugStringA(message.c_str());
	}

	// overloads the old version.dll resolves correctly by their plain name. it binds a plain name to
	// the last overload in the pdb, and for these that is the one named (or one with the same calling
	// convention and result), checked against ShooterGameServer.pdb
	inline constexpr const char* old_dll_overloads[] = {
		"FName.ToString(FString&)const",
		"FName.Init(const char*,int,EFindName,bool,int)",
		"FWeakObjectPtr.Get(bool)const",
		"AShooterPlayerController.GetPlayerViewPoint(FVector&,FRotator&)const",
		"UObjectBaseUtility.GetPathName(const UObject*,FString&)const",
	};

	// the name the old version.dll knows this symbol by, false if it cannot be looked up there
	inline bool OldDllName(const char* name, std::string* out)
	{
		if (name == nullptr)
			return false;

		const char* paren = strchr(name, '(');
		if (paren == nullptr)
		{
			out->assign(name);
			return true;
		}

		std::string compact;
		for (const char* c = name; *c != '\0'; ++c)
		{
			if (*c != ' ')
				compact += *c;
		}

		for (const char* known : old_dll_overloads)
		{
			if (compact == known)
			{
				out->assign(name, static_cast<size_t>(paren - name));
				return true;
			}
		}

		// the old version.dll has one address per plain name, and it may be another overload
		ReportOnce("overload it cannot tell apart, call skipped", name);
		return false;
	}

	// the old version.dll's table value for a name, 0 if it is unknown
	inline intptr_t OldDllOffset(const std::string& name)
	{
		return static_cast<intptr_t>(::GetAddress(static_cast<const void*>(nullptr), name));
	}

	// what an unknown field reads and writes, as on the current version.dll
	alignas(16) inline unsigned char missing_field[0x10000]{};

	// ids for the timers started with an id, which the old version.dll does not have
	struct OldDllTimer
	{
		std::atomic<bool> cancelled{false};
		std::atomic<int> remaining;
		uint64_t id;

		OldDllTimer(uint64_t timer_id, int executions) : remaining(executions), id(timer_id)
		{
		}
	};

	inline std::atomic_flag timers_lock = ATOMIC_FLAG_INIT;
	inline std::unordered_map<uint64_t, std::shared_ptr<OldDllTimer>>* timers = nullptr;
	inline uint64_t next_timer_id = 1;

	class TimersLock
	{
	public:
		TimersLock()
		{
			while (timers_lock.test_and_set(std::memory_order_acquire))
				YieldProcessor();

			if (timers == nullptr)
				timers = new std::unordered_map<uint64_t, std::shared_ptr<OldDllTimer>>();
		}

		~TimersLock()
		{
			timers_lock.clear(std::memory_order_release);
		}

		TimersLock(const TimersLock&) = delete;
		TimersLock& operator=(const TimersLock&) = delete;
	};

	// executions <= 0 runs until cancelled
	inline std::shared_ptr<OldDllTimer> AddTimer(int executions)
	{
		TimersLock lock;
		auto timer = std::make_shared<OldDllTimer>(next_timer_id++, executions);
		timers->emplace(timer->id, timer);
		return timer;
	}

	// whether the callback should run now; forgets the id after the last run
	inline bool FireTimer(OldDllTimer& timer)
	{
		if (timer.cancelled.load(std::memory_order_acquire))
			return false;

		if (timer.remaining.load(std::memory_order_relaxed) > 0 && timer.remaining.fetch_sub(1) == 1)
		{
			TimersLock lock;
			timers->erase(timer.id);
		}

		return true;
	}

	inline bool CancelTimer(uint64_t id)
	{
		TimersLock lock;
		const auto iter = timers->find(id);
		if (iter == timers->end())
			return false;

		iter->second->cancelled.store(true, std::memory_order_release);
		timers->erase(iter);
		return true;
	}
#endif
} // namespace API::DllCompat

#ifndef ARK_EXPORTS
inline bool FindNativeOffset(const char* name, intptr_t* offset)
{
	using Fn = bool (*)(const char*, intptr_t*);
	if (const auto current = API::DllCompat::Get<Fn>(API::DllCompat::FindNativeOffsetExport))
		return current(name, offset);

	std::string old_name;
	if (!API::DllCompat::OldDllName(name, &old_name))
		return false;

	const intptr_t value = API::DllCompat::OldDllOffset(old_name);
	if (value == 0)
		return false;

	if (offset != nullptr)
		*offset = value;

	return true;
}

inline bool FindNativeBitField(const char* name, BitField* bit_field)
{
	using Fn = bool (*)(const char*, BitField*);
	if (const auto current = API::DllCompat::Get<Fn>(API::DllCompat::FindNativeBitFieldExport))
		return current(name, bit_field);

	std::string old_name;
	if (!API::DllCompat::OldDllName(name, &old_name))
		return false;

	const BitField value = ::GetBitField(static_cast<const void*>(nullptr), old_name);
	if (value.num_bits == 0)
		return false;

	if (bit_field != nullptr)
		*bit_field = value;

	return true;
}

inline DWORD64 GetAddress(const void* base, const char* name)
{
	using Fn = DWORD64 (*)(const void*, const char*);
	if (const auto current = API::DllCompat::Get<Fn>(API::DllCompat::GetFieldAddressExport))
		return current(base, name);

	std::string old_name;
	const intptr_t value = API::DllCompat::OldDllName(name, &old_name) ? API::DllCompat::OldDllOffset(old_name) : 0;
	if (value == 0)
		return reinterpret_cast<DWORD64>(API::DllCompat::missing_field);

	return reinterpret_cast<DWORD64>(base) + static_cast<DWORD64>(value);
}

inline LPVOID GetAddress(const char* name)
{
	using Fn = LPVOID (*)(const char*);
	if (const auto current = API::DllCompat::Get<Fn>(API::DllCompat::GetFunctionAddressExport))
		return current(name);

	std::string old_name;
	if (!API::DllCompat::OldDllName(name, &old_name) || API::DllCompat::OldDllOffset(old_name) == 0)
		return nullptr;

	return ::GetAddress(old_name);
}

inline LPVOID GetDataAddress(const char* name)
{
	using Fn = LPVOID (*)(const char*);
	if (const auto current = API::DllCompat::Get<Fn>(API::DllCompat::GetDataAddressExport))
		return current(name);

	std::string old_name;
	if (!API::DllCompat::OldDllName(name, &old_name) || API::DllCompat::OldDllOffset(old_name) == 0)
		return nullptr;

	return ::GetDataAddress(old_name);
}

inline BitField GetBitField(const void* base, const char* name)
{
	using Fn = BitField (*)(const void*, const char*);
	if (const auto current = API::DllCompat::Get<Fn>(API::DllCompat::GetBitFieldConstExport))
		return current(base, name);

	// an unknown name comes back with num_bits 0, which reads as 0 and ignores writes
	std::string old_name;
	return API::DllCompat::OldDllName(name, &old_name) ? ::GetBitField(base, old_name) : BitField{};
}

inline BitField GetBitField(LPVOID base, const char* name)
{
	using Fn = BitField (*)(LPVOID, const char*);
	if (const auto current = API::DllCompat::Get<Fn>(API::DllCompat::GetBitFieldExport))
		return current(base, name);

	std::string old_name;
	return API::DllCompat::OldDllName(name, &old_name) ? ::GetBitField(base, old_name) : BitField{};
}

namespace API
{
	inline void LogDeprecatedUse(const char* api_name, HMODULE caller)
	{
		// the old version.dll does not track deprecated use
		using Fn = void (*)(const char*, HMODULE);
		if (const auto current = DllCompat::Get<Fn>(DllCompat::LogDeprecatedUseExport))
			current(api_name, caller);
	}
} // namespace API
#endif
