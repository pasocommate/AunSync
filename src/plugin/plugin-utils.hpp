#pragma once

#include <string>

namespace ods::plugin {

	/// 先頭・末尾の空白を除去した文字列を返す
	std::string trim_copy(std::string s);

	/// RTMP URL 候補を正規化する（スキーム補完など）
	std::string normalize_rtmp_url_candidate(const char *raw);

	/// RTMP URL とストリームキーを結合する
	std::string join_rtmp_url_and_stream_key(const std::string &base_url,
											 const std::string &raw_key);

} // namespace ods::plugin
