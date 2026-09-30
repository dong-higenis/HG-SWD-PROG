#ifndef INCLUDE_RTC_H_
#define INCLUDE_RTC_H_


#ifdef __cplusplus
extern "C" {
#endif

#include "hw_def.h"


#ifdef _USE_HW_RTC

typedef struct
{
  uint8_t hours;
  uint8_t minutes;
  uint8_t seconds;
} rtc_time_t;

typedef struct
{
  uint8_t year;
  uint8_t month;
  uint8_t day;
  uint8_t week;
} rtc_date_t;

typedef struct
{
  rtc_time_t time;
  rtc_date_t date;
} rtc_info_t;

/**
 * @brief app단에서 사용할 로그 기록용 timestamp
 */
typedef struct
{
  uint16_t year;
  uint8_t  month;
  uint8_t  day;
  uint8_t  hour;
  uint8_t  minute;
  uint8_t  second;
  uint16_t msec;
} rtc_timestamp_t;


bool rtcInit(void);
bool rtcGetInfo(rtc_info_t *rtc_info);
bool rtcGetTime(rtc_time_t *rtc_time);
bool rtcGetDate(rtc_date_t *rtc_date);
bool rtcSetTime(rtc_time_t *rtc_time);
bool rtcSetDate(rtc_date_t *rtc_date);

bool rtcSetReg(uint32_t index, uint32_t data);
bool rtcGetReg(uint32_t index, uint32_t *p_data);

bool rtcGetTimestamp(rtc_timestamp_t *p_timestamp);

#endif

#ifdef __cplusplus
}
#endif

#endif 