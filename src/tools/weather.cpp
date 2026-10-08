// get_weather 工具的真实实现。
//
// 调用链：城市名 → [Geocoding API] → 经纬度 → [Forecast API] → 中文文本。
// 全程只依赖 /mcp/http.hpp 这一个接口，HTTP 怎么实现与本文件无关。

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "mcp/http.hpp"
#include "mcp/tools/weather.hpp"

namespace mcp {

namespace {

using namespace std::chrono_literals;

constexpr const char* kGeocodingUrl = "https://geocoding-api.open-meteo.com/v1/search";
constexpr const char* kForecastUrl = "https://api.open-meteo.com/v1/forecast";
constexpr auto kHttpTimeout = 10s;

struct Place {
    std::string name;
    std::string admin1;   // 一级行政区（省/州）
    std::string country;
    std::string featureCode;  // PPLC=首都, PPLA*=行政中心, PPL=普通居民点
    double latitude = 0.0;
    double longitude = 0.0;
    long long population = 0;
};

/// 行政等级：用来判断同名地点里哪个才是“最像你要的那个”。
int adminLevel(const Place& place) {
    if (place.featureCode == "PPLC") {
        return 3;  // 首都
    }
    if (place.featureCode == "PPLA" || place.featureCode == "PPLA2" ||
        place.featureCode == "PPLA3" || place.featureCode == "PPLA4") {
        return 2;  // 各级行政中心
    }
    if (place.featureCode == "PPL") {
        return 1;
    }
    return 0;
}

/// 该地点是否带有足以区分优先级的元数据（行政等级或人口）。
bool hasRankingSignal(const Place& place) {
    return adminLevel(place) > 0 || place.population > 0;
}

/// 综合打分：行政等级优先，同级再比人口。
long long preferenceScore(const Place& place) {
    return static_cast<long long>(adminLevel(place)) * 1000000000LL + place.population;
}

std::string trim(const std::string& value) {
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    auto begin = value.begin();
    while (begin != value.end() && isSpace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    auto end = value.end();
    while (end != begin && isSpace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }
    return std::string(begin, end);
}

/// 查询串里出现中文就必须 percent-encode：
/// WinHTTP 不会替你编码，直接把 UTF-8 字节塞进 URL 会让服务端收到乱码。
std::string urlEncode(const std::string& value) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::string joinParts(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& part : parts) {
        if (!out.empty()) {
            out += "·";
        }
        out += part;
    }
    return out;
}

/// "北京市（北京市·中国）"
std::string describe(const Place& place) {
    std::vector<std::string> parts;
    if (!place.admin1.empty() && place.admin1 != place.name) {
        parts.push_back(place.admin1);
    }
    if (!place.country.empty()) {
        parts.push_back(place.country);
    }
    std::string out = place.name;
    if (!parts.empty()) {
        out += "（" + joinParts(parts) + "）";
    }
    return out;
}

/// WMO weather code → 中文描述。官方 API 只返回数字，描述必须自己映射。
std::string weatherCodeToText(int code) {
    switch (code) {
        case 0:  return "晴";
        case 1:  return "大部晴朗";
        case 2:  return "局部多云";
        case 3:  return "阴";
        case 45: return "雾";
        case 48: return "雾凇";
        case 51: return "轻微毛毛雨";
        case 53: return "毛毛雨";
        case 55: return "浓毛毛雨";
        case 56: return "轻微冻雨";
        case 57: return "冻雨";
        case 61: return "小雨";
        case 63: return "中雨";
        case 65: return "大雨";
        case 66: return "轻微冻雨";
        case 67: return "冻雨";
        case 71: return "小雪";
        case 73: return "中雪";
        case 75: return "大雪";
        case 77: return "米雪";
        case 80: return "小阵雨";
        case 81: return "中阵雨";
        case 82: return "强阵雨";
        case 85: return "小阵雪";
        case 86: return "大阵雪";
        case 95: return "雷阵雨";
        case 96: return "雷阵雨伴冰雹";
        case 99: return "雷阵雨伴强冰雹";
        default: return "未知天气（代码 " + std::to_string(code) + "）";
    }
}

double numberOr(const json& parent, const char* key, double fallback) {
    if (parent.contains(key) && parent.at(key).is_number()) {
        return parent.at(key).get<double>();
    }
    return fallback;
}

int intOr(const json& parent, const char* key, int fallback) {
    if (parent.contains(key) && parent.at(key).is_number_integer()) {
        return parent.at(key).get<int>();
    }
    return fallback;
}

/// daily 里的字段是与 time 等长的平行数组，取第 index 天的值。
json arrayElement(const json& parent, const char* key, size_t index) {
    if (!parent.contains(key)) {
        return json();
    }
    const json& array = parent.at(key);
    if (!array.is_array() || index >= array.size()) {
        return json();
    }
    return array.at(index);
}

double numberOr(const json& parent, const char* key, double fallback, size_t index) {
    const json element = arrayElement(parent, key, index);
    return element.is_number() ? element.get<double>() : fallback;
}

int intOr(const json& parent, const char* key, int fallback, size_t index) {
    const json element = arrayElement(parent, key, index);
    return element.is_number_integer() ? element.get<int>() : fallback;
}

std::string stringOr(const json& parent, const char* key, const std::string& fallback) {
    if (parent.contains(key) && parent.at(key).is_string()) {
        return parent.at(key).get<std::string>();
    }
    return fallback;
}

struct FetchResult {
    bool ok = false;
    std::string error;
    json body;
};

/// GET + 解析 JSON，把三种失败都收敛成可读文本。
FetchResult fetchJson(const std::string& url) {
    const HttpResponse response = httpGet(url, kHttpTimeout);
    if (response.status == 0) {
        return {false, "网络请求失败（" + response.error + "）", {}};
    }
    if (response.status != 200) {
        return {false,
                "上游返回 HTTP " + std::to_string(response.status) + "：" +
                    response.body.substr(0, 200),
                {}};
    }

    json parsed = json::parse(response.body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) {
        return {false, "上游响应不是合法 JSON", {}};
    }
    // Open-Meteo 的业务错误是 HTTP 400 + {"error":true,"reason":"..."}
    if (parsed.contains("error") && parsed.at("error") == true) {
        return {false, "上游返回错误：" + stringOr(parsed, "reason", "无原因"), {}};
    }
    return {true, "", std::move(parsed)};
}

/// 进程内缓存：同一会话里重复问同一个城市可以省掉一次 RTT。
/// 跨进程不做缓存（YAGNI），进程一退就没了。
std::unordered_map<std::string, Place>& placeCache() {
    static std::unordered_map<std::string, Place> cache;
    return cache;
}

ToolResult getWeather(const json& args) {
    const std::string city = trim(stringOr(args, "city", ""));
    int days = intOr(args, "days", 3);
    if (days < 1 || days > 16) {
        days = std::clamp(days, 1, 16);
    }

    std::cerr << "[weather] get_weather city=" << city << " days=" << days << std::endl;

    ToolResult failure;
    failure.isError = true;

    if (city.empty()) {
        failure.text = "缺少参数 city：请提供城市名，例如「北京」或「Tokyo」。";
        return failure;
    }

    // ---------------------------------------------------------- 城市 → 坐标
    Place place;
    bool hasPlace = false;

    const auto cached = placeCache().find(city);
    if (cached != placeCache().end()) {
        place = cached->second;
        hasPlace = true;
        std::cerr << "[weather] cache hit: " << city << std::endl;
    } else {
        const std::string geoUrl = std::string(kGeocodingUrl) + "?name=" + urlEncode(city) +
                                   "&count=5&language=zh&format=json";
        const FetchResult geo = fetchJson(geoUrl);
        if (!geo.ok) {
            failure.text = "查询城市坐标失败：" + geo.error;
            return failure;
        }

        const json& results = geo.body.contains("results") ? geo.body.at("results") : json::array();
        if (!results.is_array() || results.empty()) {
            failure.text = "没有找到名为「" + city + "」的城市，请换一个更常见或更精确的写法。";
            return failure;
        }

        std::vector<Place> candidates;
        for (const auto& item : results) {
            Place candidate;
            candidate.name = stringOr(item, "name", "");
            candidate.admin1 = stringOr(item, "admin1", "");
            candidate.country = stringOr(item, "country", "");
            candidate.featureCode = stringOr(item, "feature_code", "");
            candidate.latitude = numberOr(item, "latitude", 0.0);
            candidate.longitude = numberOr(item, "longitude", 0.0);
            candidate.population = static_cast<long long>(numberOr(item, "population", 0.0));
            candidates.push_back(candidate);
        }

        size_t bestIndex = 0;

        if (std::any_of(candidates.begin(), candidates.end(), hasRankingSignal)) {
            // 有行政等级/人口数据时，直接选最知名的那个（查「北京」就该拿到首都，
            // 而不是重庆某个同名的村），这是最常见的用户意图。
            for (size_t i = 1; i < candidates.size(); ++i) {
                if (preferenceScore(candidates[i]) > preferenceScore(candidates[bestIndex])) {
                    bestIndex = i;
                }
            }
        } else {
            // 完全没有排序依据时，才把候选摊开来让人确认：
            // 静默选错一个城市，比多问一句糟糕得多。
            const bool ambiguous = std::any_of(candidates.begin(), candidates.end(), [&](const Place& p) {
                return p.admin1 != candidates.front().admin1 || p.country != candidates.front().country;
            });
            if (candidates.size() > 1 && ambiguous) {
                std::ostringstream text;
                text << "「" << city << "」匹配到多个地点，无法判断你要查哪一个：";
                for (size_t i = 0; i < candidates.size(); ++i) {
                    text << "\n" << (i + 1) << ". " << describe(candidates[i]);
                }
                text << "\n请用更精确的写法重试，例如带上省名或国家名。";
                failure.text = text.str();
                failure.isError = true;
                return failure;
            }
        }

        place = candidates[bestIndex];
        hasPlace = true;
        placeCache()[city] = place;
    }

    if (!hasPlace) {
        failure.text = "无法确定城市坐标。";
        return failure;
    }

    // ------------------------------------------------------------ 坐标 → 天气
    // 注意：只要请求了 daily，Open-Meteo 就强制要求带 timezone 参数。
    std::ostringstream forecastUrl;
    forecastUrl << kForecastUrl
                << "?latitude=" << place.latitude << "&longitude=" << place.longitude
                << "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m"
                << "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
                << "&timezone=auto"
                << "&forecast_days=" << days
                << "&temperature_unit=celsius&wind_speed_unit=kmh";

    const FetchResult forecast = fetchJson(forecastUrl.str());
    if (!forecast.ok) {
        failure.text = "查询天气失败：" + forecast.error;
        return failure;
    }

    const json& body = forecast.body;
    if (!body.contains("current") || !body.contains("daily")) {
        failure.text = "上游响应缺少 current/daily 字段，无法生成预报。";
        return failure;
    }

    const json& current = body.at("current");
    const json& daily = body.at("daily");

    std::ostringstream text;
    text << std::fixed << std::setprecision(1);
    text << describe(place) << " 当前天气："
         << weatherCodeToText(intOr(current, "weather_code", -1))
         << "，气温 " << numberOr(current, "temperature_2m", 0.0) << "°C"
         << "（体感 " << numberOr(current, "apparent_temperature", 0.0) << "°C）"
         << "，湿度 " << static_cast<int>(numberOr(current, "relative_humidity_2m", 0.0)) << "%"
         << "，风速 " << numberOr(current, "wind_speed_10m", 0.0) << " km/h";

    const json& dates = daily.contains("time") ? daily.at("time") : json::array();
    if (dates.is_array() && !dates.empty()) {
        text << "\n未来 " << dates.size() << " 天预报：";
        for (size_t i = 0; i < dates.size(); ++i) {
            const std::string date = dates.at(i).is_string() ? dates.at(i).get<std::string>() : std::to_string(i);
            text << "\n- " << date << "："
                 << weatherCodeToText(intOr(daily, "weather_code", -1, i))
                 << "，" << numberOr(daily, "temperature_2m_min", 0.0, i)
                 << " ~ " << numberOr(daily, "temperature_2m_max", 0.0, i) << "°C";
            const double precipitation = numberOr(daily, "precipitation_probability_max", -1.0, i);
            if (precipitation >= 0) {
                text << "，降水概率 " << static_cast<int>(precipitation) << "%";
            }
        }
    }

    text << "\n数据来源：Open-Meteo（时区 " << stringOr(body, "timezone", "auto") << "）";

    ToolResult result;
    result.text = text.str();
    result.isError = false;
    return result;
}

}  // namespace

void registerWeatherTools(ToolRegistry& registry) {
    ToolDef tool;
    tool.name = "get_weather";
    tool.description =
        "查询指定城市的当前天气与未来若干天的天气预报（数据源：Open-Meteo，免密钥）。"
        "城市支持中文或英文名；同名城市存在多个地点时会返回候选列表，需要先用更精确的名称确认。";

    json cityProperty = json::object();
    cityProperty["type"] = "string";
    cityProperty["description"] = "城市名，中文或英文均可，例如：北京、杭州、Tokyo";

    json daysProperty = json::object();
    daysProperty["type"] = "integer";
    daysProperty["description"] = "预报天数，1-16，默认为 3";
    daysProperty["minimum"] = 1;
    daysProperty["maximum"] = 16;

    json properties = json::object();
    properties["city"] = cityProperty;
    properties["days"] = daysProperty;

    json required = json::array();
    required.push_back("city");

    json schema = json::object();
    schema["type"] = "object";
    schema["properties"] = properties;
    schema["required"] = required;
    tool.inputSchema = schema;

    tool.handler = getWeather;
    registry.add(std::move(tool));
}

}  // namespace mcp
