#ifndef BMS_DEBUG_H
#define BMS_DEBUG_H
/* USART1 is reserved for framed telemetry; no text is written to it. */
#define BMS_ERROR(...) do {} while (0)
#define BMS_WARNING(...) do {} while (0)
#define BMS_INFO(...) do {} while (0)
#endif
