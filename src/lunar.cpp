// lunar.cpp — 农历算法实现
#include "lunar.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>

// 1900-2100 农历年数据表（低位4bit=闰月，bit16=闰月30天，其余bit=对应月份大月）
static const int kLunarInfo[] = {
    0x04bd8,0x04ae0,0x0a570,0x054d5,0x0d260,0x0d950,0x16554,0x056a0,0x09ad0,0x055d2,
    0x04ae0,0x0a5b6,0x0a4d0,0x0d250,0x1d255,0x0b540,0x0d6a0,0x0ada2,0x095b0,0x14977,
    0x04970,0x0a4b0,0x0b4b5,0x06a50,0x06d40,0x1ab54,0x02b60,0x09570,0x052f2,0x04970,
    0x06566,0x0d4a0,0x0ea50,0x06e95,0x05ad0,0x02b60,0x186e3,0x092e0,0x1c8d7,0x0c950,
    0x0d4a0,0x1d8a6,0x0b550,0x056a0,0x1a5b4,0x025d0,0x092d0,0x0d2b2,0x0a950,0x0b557,
    0x06ca0,0x0b550,0x15355,0x04da0,0x0a5b0,0x14573,0x052b0,0x0a9a8,0x0e950,0x06aa0,
    0x0aea6,0x0ab50,0x04b60,0x0aae4,0x0a570,0x05260,0x0f263,0x0d950,0x05b57,0x056a0,
    0x096d0,0x04dd5,0x04ad0,0x0a4d0,0x0d4d4,0x0d250,0x0d558,0x0b540,0x0b6a0,0x195a6,
    0x095b0,0x049b0,0x0a974,0x0a4b0,0x0b27a,0x06a50,0x06d40,0x0af46,0x0ab60,0x09570,
    0x04af5,0x04970,0x064b0,0x074a3,0x0ea50,0x06b58,0x055c0,0x0ab60,0x096d5,0x092e0,
    0x0c960,0x0d954,0x0d4a0,0x0da50,0x07552,0x056a0,0x0abb7,0x025d0,0x092d0,0x0cab5,
    0x0a950,0x0b4a0,0x0baa4,0x0ad50,0x055d9,0x04ba0,0x0a5b0,0x15176,0x052b0,0x0a930,
    0x07954,0x06aa0,0x0ad50,0x05b52,0x04b60,0x0a6e6,0x0a4e0,0x0d260,0x0ea65,0x0d530,
    0x05aa0,0x076a3,0x096d0,0x04afb,0x04ad0,0x0a4d0,0x1d0b6,0x0d250,0x0d520,0x0dd45,
    0x0b5a0,0x056d0,0x055b2,0x049b0,0x0a577,0x0a4b0,0x0aa50,0x1b255,0x06d20,0x0ada0,
    0x14b63,0x09370,0x049f8,0x04970,0x064b0,0x168a6,0x0ea50,0x06b20,0x1a6c4,0x0aae0,
    0x092e0,0x0d2e3,0x0c960,0x0d557,0x0d4a0,0x0da50,0x05d55,0x056a0,0x0a6d0,0x055d4,
    0x052d0,0x0a9b8,0x0a950,0x0b4a0,0x0b6a6,0x0ad50,0x055a0,0x0aba4,0x0a5b0,0x052b0,
    0x0b273,0x06930,0x07337,0x06aa0,0x0ad50,0x14b55,0x04b60,0x0a570,0x054e4,0x0d160,
    0x0e968,0x0d520,0x0daa0,0x16aa6,0x056d0,0x04ae0,0x0a9d4,0x0a2d0,0x0d150,0x0f252,
    0x0d520
};

static const int kBaseYear = 1900;   // 1900-01-31 = 农历1900年正月初一
static const int kMaxYear  = 2100;

static int leap_month(int y) {
    if (y < kBaseYear || y > kMaxYear) return 0;
    return kLunarInfo[y - kBaseYear] & 0xf;
}
static int leap_days(int y) {
    if (leap_month(y)) return (kLunarInfo[y - kBaseYear] & 0x10000) ? 30 : 29;
    return 0;
}
static int month_days(int y, int m) {
    if (m < 1 || m > 12) return 0;
    return (kLunarInfo[y - kBaseYear] & (0x10000 >> m)) ? 30 : 29;
}
static int lunar_year_days(int y) {
    int sum = 348;
    for (int i = 0x8000; i > 0x8; i >>= 1)
        sum += (kLunarInfo[y - kBaseYear] & i) ? 1 : 0;
    return sum + leap_days(y);
}

// ---- Hinnant 公历算法 ----
static long long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}
static void civil_from_days(long long z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
}
static long long epoch_days(int y, int m, int d) { return days_from_civil(y, m, d); }
static void epoch_to_date(long long e, int& y, int& m, int& d) {
    unsigned mu = 0, du = 0;
    civil_from_days(e, y, mu, du);
    m = static_cast<int>(mu);
    d = static_cast<int>(du);
}

static const char* kMonthCn[] = {"", "正", "二", "三", "四", "五", "六",
                                 "七", "八", "九", "十", "冬", "腊"};
static const char* kDayCn[] = {"", "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
                               "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
                               "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十"};

static std::string num_year_cn(int y) {
    static const char* digits[] = {"〇", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
    std::string s;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d", y);
    for (const char* p = buf; *p; ++p) s += digits[*p - '0'];
    return s;
}

namespace lunar {

std::string month_name(int m, bool isLeap) {
    if (m < 1 || m > 12) return "";
    return std::string(isLeap ? "闰" : "") + kMonthCn[m] + "月";
}

std::string day_name(int d) {
    if (d < 1 || d > 30) return "";
    return kDayCn[d];
}

LunarDate solar_to_lunar(int year, int month, int day) {
    LunarDate r;
    long long offset = epoch_days(year, month, day) - epoch_days(kBaseYear, 1, 31);
    if (offset < 0 || offset > 735000) return r;  // 超出 1900~2100 范围
    int i = kBaseYear;
    int temp = 0;
    while (i <= kMaxYear && offset > 0) {
        temp = lunar_year_days(i);
        offset -= temp;
        ++i;
    }
    if (offset < 0) { offset += temp; --i; }
    r.year = i;
    int leap = leap_month(i);
    bool is_leap = false;
    int m = 1;
    for (m = 1; m < 13 && offset > 0; ++m) {
        if (leap > 0 && m == leap + 1 && !is_leap) {
            --m; is_leap = true; temp = leap_days(r.year);
        } else {
            temp = month_days(r.year, m);
        }
        if (is_leap && m == leap + 1) is_leap = false;
        offset -= temp;
    }
    if (offset == 0 && leap > 0 && m == leap + 1) {
        if (is_leap) is_leap = false; else { is_leap = true; --m; }
    }
    if (offset < 0) { offset += temp; --m; }
    r.month = m;
    r.day = static_cast<int>(offset) + 1;
    r.isLeap = is_leap;
    r.chinese = num_year_cn(r.year) + "年" + month_name(r.month, r.isLeap) + day_name(r.day);
    return r;
}

SolarDate lunar_to_solar(int ly, int lm, int ld, bool isLeap) {
    SolarDate r;
    if (ly < kBaseYear || ly > kMaxYear || lm < 1 || lm > 12 || ld < 1 || ld > 30)
        return r;
    int leap = leap_month(ly);
    if (isLeap && lm != leap) return r;  // 该农历年没有此闰月
    int offset = 0;
    for (int i = kBaseYear; i < ly; ++i) offset += lunar_year_days(i);
    // 月序：M1..M_leap, 闰月L, M_(leap+1)..M12
    if (isLeap) {
        // 闰月 X：其第1天在 M_X 结束之后
        for (int i = 1; i <= lm; ++i) offset += month_days(ly, i);
    } else if (lm <= leap) {
        for (int i = 1; i < lm; ++i) offset += month_days(ly, i);
    } else {
        for (int i = 1; i <= leap; ++i) offset += month_days(ly, i);
        offset += leap_days(ly);
        for (int i = leap + 1; i < lm; ++i) offset += month_days(ly, i);
    }
    offset += ld - 1;
    long long e = epoch_days(kBaseYear, 1, 31) + offset;
    int y, m, d;
    epoch_to_date(e, y, m, d);
    r.year = y; r.month = m; r.day = d;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", y, m, d);
    r.iso = buf;
    return r;
}

std::string next_lunar_date_after(int startYear, int month, int day, bool isLeap) {
    // 从 startYear 开始逐年在"该农历月对应的公历月"查找下一个 >= 今天 的日期
    int y = startYear;
    while (y <= kMaxYear) {
        SolarDate s = lunar_to_solar(y, month, day, isLeap);
        if (s.year != 0) {
            std::string today = today_iso();
            if (s.iso >= today) return s.iso;
        }
        ++y;
    }
    return "";
}

std::string add_days_iso(const std::string& iso, int days) {
    int y = 0, m = 0, d = 0;
    if (std::sscanf(iso.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return "";
    long long e = epoch_days(y, m, d) + days;
    epoch_to_date(e, y, m, d);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", y, m, d);
    return buf;
}

int weekday_of_iso(const std::string& iso) {
    int y = 0, m = 0, d = 0;
    if (std::sscanf(iso.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return 0;
    // 1970-01-01 = 星期四(4)，epoch_days(1970,1,1)=0
    long long e = epoch_days(y, m, d);
    long long w = (e + 3) % 7;   // 0=周日 ... 6=周六
    if (w == 0) return 7;        // 周日 -> 7
    return static_cast<int>(w);  // 1..6 = 周一..周六
}

std::string today_iso() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

// ---------------- 24 节气 ----------------
// 寿星通用公式：Day = int(Y*0.2422 + C) - int(Y/4)，Y=年份后两位，C=世纪常数

// 节气按年内序号 0..23（从小寒起，即 1 月节气在前）
static const char* kTermNames[24] = {
    "小寒", "大寒", "立春", "雨水", "惊蛰", "春分",
    "清明", "谷雨", "立夏", "小满", "芒种", "夏至",
    "小暑", "大暑", "立秋", "处暑", "白露", "秋分",
    "寒露", "霜降", "立冬", "小雪", "大雪", "冬至"
};
// 每个节气所在月份（序号 0=小寒 在 1 月，依此类推）
static const int kTermMonth[24] = {
    1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12
};
// 21 世纪（2000-2099）C 值
static const double kC21[24] = {
    5.4055, 20.12, 3.87, 18.73, 5.63, 20.646,
    4.81, 20.1, 5.52, 21.04, 5.678, 21.37,
    7.108, 22.83, 7.5, 23.13, 7.646, 23.042,
    8.318, 23.438, 7.438, 22.36, 7.18, 21.94
};
// 20 世纪（1900-1999）C 值
static const double kC20[24] = {
    6.11, 20.84, 4.6297, 19.4599, 6.3826, 21.4155,
    5.59, 20.888, 6.318, 21.86, 6.5, 22.20,
    7.928, 23.65, 8.35, 23.95, 8.44, 23.822,
    9.098, 24.218, 8.218, 23.08, 7.9, 22.60
};

// 已知例外修正（返回需加减的天数）
static int term_fix(int year, int term) {
    if (year == 2008 && term == 0) return 1;    // 2008 小寒 +1
    if (year == 2082 && term == 1) return 1;    // 2082 大寒 +1
    return 0;
}

// 计算某年某节气（序号 term）在公历中的日期（day），失败返回 0
static int term_day_of_month(int year, int term) {
    if (year < 1900 || year > 2099) return 0;
    int y = year % 100;
    double c = (year >= 2000) ? kC21[term] : kC20[term];
    int d = static_cast<int>(y * 0.2422 + c) - (y / 4) + term_fix(year, term);
    return d;
}

std::string solar_term(int year, int month, int day) {
    if (year < 1900 || year > 2099) return "";
    for (int t = 0; t < 24; ++t) {
        if (kTermMonth[t] != month) continue;
        if (term_day_of_month(year, t) == day) return kTermNames[t];
    }
    return "";
}

// ---------------- 法定节假日 ----------------
std::string statutory_holiday(int year, int month, int day) {
    if (year < 1901 || year > 2099) return "";
    // 公历固定日
    if (month == 1 && day == 1) return "元旦";
    if (month == 5 && day == 1) return "劳动节";
    if (month == 10 && day >= 1 && day <= 3) return "国庆节";
    // 清明：当月（4 月）节气"清明"当日
    if (month == 4 && !solar_term(year, 4, day).empty() &&
        solar_term(year, 4, day) == "清明")
        return "清明节";
    // 农历推导：春节(正月初一~初三)、端午(五月初五)、中秋(八月十五)
    LunarDate ld = solar_to_lunar(year, month, day);
    if (ld.year == 0) return "";
    if (ld.month == 1 && !ld.isLeap && ld.day >= 1 && ld.day <= 3) return "春节";
    if (ld.month == 5 && !ld.isLeap && ld.day == 5) return "端午节";
    if (ld.month == 8 && !ld.isLeap && ld.day == 15) return "中秋节";
    return "";
}

} // namespace lunar
