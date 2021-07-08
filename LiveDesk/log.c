/* ����ͷ�ļ� */
#include <windows.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include "log.h"

#ifdef _MSC_VER
#pragma warning(disable:4996)
#endif

// �ڲ���������
#define LOG_MODE_DISABLE  0
#define LOG_MODE_FILE     1
#define LOG_MODE_DEBUGER  2

static FILE *s_log_fp = NULL;
static DWORD s_log_mode = LOG_MODE_DISABLE;

/* ����ʵ�� */
void log_init(char *file)
{
    if (!s_log_fp) {
        if (strcmp(file, "DEBUGER") == 0) {
            s_log_mode = LOG_MODE_DEBUGER;
        } else {
            s_log_fp = fopen(file, "w");
            if (s_log_fp) {
                s_log_mode = LOG_MODE_FILE;
            }
        }
    }
}

void log_done(void)
{
    if (s_log_fp) {
        fflush(s_log_fp);
        fclose(s_log_fp);
        s_log_fp   = NULL;
        s_log_mode = 0;
    }
}

#define MAX_LOG_BUF 1024
void log_printf(char *format, ...)
{
    char buf[MAX_LOG_BUF];
    va_list valist;

    // if debug log is not enable, directly return
    if (s_log_mode == LOG_MODE_DISABLE) return;

    va_start(valist, format);
    vsnprintf(buf, MAX_LOG_BUF, format, valist);
    va_end(valist);

    switch (s_log_mode) {
    case LOG_MODE_FILE:
        fputs(buf, s_log_fp);
        fflush(s_log_fp);
        break;

    case LOG_MODE_DEBUGER:
#ifdef WIN32
        OutputDebugStringA(buf);
#else
        printf("%s", buf); fflush(stdout);
#endif
        break;
    }
}
