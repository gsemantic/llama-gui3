#pragma once

#include <string>

namespace news_rewriter {

// Приводит URL к виду, понятному libcurl:
//  - интернационализированные домены (IDN, кириллица в хосте) перекодируются
//    в punycode (xn--...). Сама библиотека libcurl (в режиме dlopen) IDN не
//    делает — она возвращает CURLE_URL_MALFORMAT на «https://пример.рф/».
//  - путь/query/fragment трогать не нужно: libcurl сам процентирует
//    не-ASCII байты в пути.
// Если в URL нет не-ASCII в хосте, возвращается исходная строка.
std::string normalize_url(const std::string& url);

} // namespace news_rewriter
