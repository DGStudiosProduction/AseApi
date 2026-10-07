#pragma once

#include "../API/Base.h"
#include "Logger/spdlog/spdlog.h"

ARK_API std::vector<spdlog::sink_ptr>& APIENTRY GetLogSinks();

#ifdef ARK_EXPORTS
ARK_API spdlog::level::level_enum APIENTRY GetLogFlushLevel();
#else
// not exported by an older version.dll, see DllCompat.h
inline spdlog::level::level_enum APIENTRY GetLogFlushLevel()
{
	using Fn = spdlog::level::level_enum (*)();
	if (const auto current = API::DllCompat::Get<Fn>(API::DllCompat::GetLogFlushLevelExport))
		return current();

	// what the old version.dll flushed on
	return spdlog::level::info;
}

namespace API::DllCompat
{
	inline void ReportToArkLog(const char* message)
	{
		auto& sinks = GetLogSinks();
		spdlog::logger("ArkApi", begin(sinks), end(sinks)).warn("{}", message);
	}

	inline const bool report_to_ark_log = (reporter.store(&ReportToArkLog, std::memory_order_release), true);
} // namespace API::DllCompat
#endif

class Log
{
public:
	Log(const Log&) = delete;
	Log(Log&&) = delete;
	Log& operator=(const Log&) = delete;
	Log& operator=(Log&&) = delete;

	static Log& Get()
	{
		static Log instance;
		return instance;
	}

	static std::shared_ptr<spdlog::logger>& GetLog()
	{
		return Get().logger_;
	}

	void Init(const std::string& plugin_name)
	{
		auto& sinks = GetLogSinks();

		logger_ = std::make_shared<spdlog::logger>(plugin_name, begin(sinks), end(sinks));

		logger_->set_pattern("%D %R [%n][%l] %v");
		logger_->flush_on(GetLogFlushLevel());
	}

private:
	Log() = default;
	~Log() = default;

	std::shared_ptr<spdlog::logger> logger_;
};
