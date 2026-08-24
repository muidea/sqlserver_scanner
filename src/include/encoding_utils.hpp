#pragma once
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace duckdb {

inline std::string GBKToUTF8(const std::string &gbk_str) {
#ifdef _WIN32
	if (gbk_str.empty()) {
		return gbk_str;
	}

	int input_len = static_cast<int>(gbk_str.size());

	// First: system code page -> WideChar (UTF-16)
	// Use explicit length instead of -1 to handle strings that may contain embedded nulls.
	int wlen = MultiByteToWideChar(CP_ACP, 0, gbk_str.data(), input_len, NULL, 0);
	if (wlen <= 0) {
		return gbk_str;
	}

	std::vector<wchar_t> wstr(wlen);
	MultiByteToWideChar(CP_ACP, 0, gbk_str.data(), input_len, wstr.data(), wlen);

	// Second: WideChar (UTF-16) -> UTF-8
	int u8len = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), wlen, NULL, 0, NULL, NULL);
	if (u8len <= 0) {
		return gbk_str;
	}

	std::vector<char> u8str(u8len);
	WideCharToMultiByte(CP_UTF8, 0, wstr.data(), wlen, u8str.data(), u8len, NULL, NULL);

	return std::string(u8str.data(), u8len);
#else
	return gbk_str;
#endif
}

// Convert UTF-8 to system code page (e.g. GBK on Chinese Windows).
// Used when sending SQL strings to ODBC on Windows; the driver expects the system code page.
inline std::string UTF8ToGBK(const std::string &utf8_str) {
#ifdef _WIN32
	if (utf8_str.empty()) {
		return utf8_str;
	}

	int input_len = static_cast<int>(utf8_str.size());

	// UTF-8 -> WideChar (UTF-16)
	int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_str.data(), input_len, NULL, 0);
	if (wlen <= 0) {
		return utf8_str;
	}
	std::vector<wchar_t> wstr(wlen);
	MultiByteToWideChar(CP_UTF8, 0, utf8_str.data(), input_len, wstr.data(), wlen);

	// WideChar -> system code page
	int mblen = WideCharToMultiByte(CP_ACP, 0, wstr.data(), wlen, NULL, 0, NULL, NULL);
	if (mblen <= 0) {
		return utf8_str;
	}
	std::vector<char> mbstr(mblen);
	WideCharToMultiByte(CP_ACP, 0, wstr.data(), wlen, mbstr.data(), mblen, NULL, NULL);

	return std::string(mbstr.data(), mblen);
#else
	return utf8_str;
#endif
}

} // namespace duckdb
