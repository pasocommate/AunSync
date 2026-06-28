#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "plugin/plugin-utils.hpp"

#include <cstring>
#include <string>

namespace ods::plugin {

	std::string trim_copy(std::string s) {
		const char  *ws    = " \t\r\n";
		const size_t begin = s.find_first_not_of(ws);
		if (begin == std::string::npos) return "";
		const size_t end = s.find_last_not_of(ws);
		return s.substr(begin, end - begin + 1);
	}

	std::string normalize_rtmp_url_candidate(const char *raw) {
		if (!raw || !*raw) return "";
		std::string url = trim_copy(raw);
		if (url.empty()) return "";
		if (_strnicmp(url.c_str(), "rtmp://", 7) == 0) return url;
		if (_strnicmp(url.c_str(), "rtmps://", 8) == 0) return url;
		if (url.find("://") != std::string::npos) return "";
		if (_stricmp(url.c_str(), "auto") == 0) return "";
		if (_stricmp(url.c_str(), "default") == 0) return "";
		if (url.find_first_of(" \t\r\n") != std::string::npos) return "";
		return "rtmp://" + url;
	}

	std::string join_rtmp_url_and_stream_key(const std::string &base_url,
											 const std::string &raw_key) {
		std::string base = trim_copy(base_url);
		std::string key  = trim_copy(raw_key);
		if (base.empty() || key.empty()) return base;

		if (_strnicmp(key.c_str(), "rtmp://", 7) == 0 ||
			_strnicmp(key.c_str(), "rtmps://", 8) == 0) {
			return normalize_rtmp_url_candidate(key.c_str());
		}

		while (!key.empty() && key.front() == '/')
			key.erase(key.begin());
		if (key.empty()) return base;

		if (base.size() >= key.size() &&
			base.compare(base.size() - key.size(), key.size(), key) == 0) {
			if (base.size() == key.size() || base[base.size() - key.size() - 1] == '/') {
				return base;
			}
		}

		if (!base.empty() && base.back() != '/') base.push_back('/');
		return base + key;
	}

} // namespace ods::plugin
