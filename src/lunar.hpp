// lunar.hpp — 农历(阴历)与公历(阳历)互转，支持 1900-01-31 ~ 2100-12-31
#pragma once
#include <string>

struct LunarDate {
    int year = 0;
    int month = 0;      // 1-12
    int day = 0;        // 1-30
    bool isLeap = false;
    // 中文描述，如 "二〇二六年八月初三" / "八月十五"
    std::string chinese;
};

struct SolarDate {
    int year = 0;
    int month = 0;
    int day = 0;
    std::string iso;    // YYYY-MM-DD
};

namespace lunar {

// 公历 -> 农历（越界返回 year=0）
LunarDate solar_to_lunar(int year, int month, int day);

// 农历 -> 公历；isLeap 表示闰月；非法返回 year=0
SolarDate lunar_to_solar(int lunarYear, int lunarMonth, int lunarDay, bool isLeap);

// 由农历月日(如 8-15)在给定公历年之后(含当年)寻找下一个公历日期；找不到返回空串
// 若该农历月在当年不存在闰月则取正月的对应日期
std::string next_lunar_date_after(int startYear, int month, int day, bool isLeap);

// 农历月的中文名
std::string month_name(int m, bool isLeap);
// 农历日的中文名
std::string day_name(int d);

// 工具：公历日期加减天数, iso 格式 YYYY-MM-DD
std::string add_days_iso(const std::string& iso, int days);
int weekday_of_iso(const std::string& iso);   // 1=周一 ... 7=周日
std::string today_iso();

} // namespace lunar
