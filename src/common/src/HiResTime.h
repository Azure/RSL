#pragma once

namespace RSLibImpl
{

typedef INT64 HiResTime;

HiResTime GetHiResTime();

#define HRTIME_FOREVER  (10*HRTIME_DECADE)
#define HRTIME_DECADE   (10*HRTIME_YEAR)
#define HRTIME_YEAR     (365*HRTIME_DAY+HRTIME_DAY/4)
#define HRTIME_WEEK     (7*HRTIME_DAY)
#define HRTIME_DAY      (24*HRTIME_HOUR)
#define HRTIME_HOUR     (60*HRTIME_MINUTE)
#define HRTIME_MINUTE   (60*HRTIME_SECOND)
#define HRTIME_SECOND   (1000*HRTIME_MSECOND)
#define HRTIME_MSECOND  (1000*HRTIME_USECOND)
#define HRTIME_USECOND  (1i64)

#define HRTIME_YEARS(_x)    ((_x)*HRTIME_YEAR)
#define HRTIME_WEEKS(_x)    ((_x)*HRTIME_WEEK)
#define HRTIME_DAYS(_x)     ((_x)*HRTIME_DAY)
#define HRTIME_HOURS(_x)    ((_x)*HRTIME_HOUR)
#define HRTIME_MINUTES(_x)  ((_x)*HRTIME_MINUTE)
#define HRTIME_SECONDS(_x)  ((_x)*HRTIME_SECOND)
#define HRTIME_MSECONDS(_x) ((_x)*HRTIME_MSECOND)
#define HRTIME_USECONDS(_x) ((_x)*HRTIME_USECOND)

} // namespace RSLibImpl

