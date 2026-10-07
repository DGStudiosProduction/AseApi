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

namespace API::DllCompat
{
	// reports from headers go to the plugin's own log once it has one, so the line names the plugin
	inline void ReportToLog(const char* message)
	{
		if (const auto& plugin_log = Log::GetLog())
		{
			plugin_log->warn("{}", message);
			return;
		}

		auto& sinks = GetLogSinks();
		spdlog::logger("ArkApi", begin(sinks), end(sinks)).warn("{}", message);
	}

	inline const bool report_to_log = (reporter.store(&ReportToLog, std::memory_order_release), true);
} // namespace API::DllCompat
