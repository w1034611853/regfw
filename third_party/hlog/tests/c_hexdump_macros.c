// Mock ABI: checks C argument evaluation and forwarding, not row formatting.
#include "hlog.h"
#include <assert.h>
struct HLogLogger {uint32_t level;};
static struct HLogLogger logger={HLOG_LEVEL_TRACE};
static unsigned calls,logger_reads,format_reads,data_reads,length_reads,filter_reads;
static uint32_t last_level,last_format;
static size_t last_bytes;
static bool last_console;
static const uint8_t data[17]={1,2,3};
static const HLogLogger *get_logger(void){++logger_reads;return &logger;}
static HLogDumpFormat get_format(void){++format_reads;return HLOG_DUMP_U32;}
static const uint8_t *get_data(void){++data_reads;return data;}
static size_t get_length(void){++length_reads;return sizeof(data);}
bool hlog_enabled(const HLogLogger *value,uint32_t level){
    ++filter_reads;return value&&level<HLOG_LEVEL_OFF&&level>=value->level;
}
static int32_t capture(const HLogLogger *value,uint32_t level,const uint8_t *function,size_t function_len,
    const uint8_t *file,size_t file_len,uint32_t line,const uint8_t *bytes,size_t length,uint32_t format,bool console){
    assert(value&&function&&function_len&&file&&file_len&&line&&bytes==data);
    ++calls;last_level=level;last_format=format;last_bytes=length;last_console=console;return HLOG_OK;
}
int32_t hlog_hexdump_ex(const HLogLogger *value,uint32_t level,const uint8_t *function,size_t function_len,
    const uint8_t *file,size_t file_len,uint32_t line,const uint8_t *bytes,size_t length,uint32_t format){
    return capture(value,level,function,function_len,file,file_len,line,bytes,length,format,false);
}
int32_t hlog_console_hexdump_ex(const HLogLogger *value,uint32_t level,const uint8_t *function,size_t function_len,
    const uint8_t *file,size_t file_len,uint32_t line,const uint8_t *bytes,size_t length,uint32_t format){
    return capture(value,level,function,function_len,file,file_len,line,bytes,length,format,true);
}
int main(void){
    HLOG_HEXDUMP_DEBUG(get_logger(),get_format(),get_data(),get_length());
    assert(calls==1&&logger_reads==1&&format_reads==1&&data_reads==1&&length_reads==1);
    assert(last_level==1&&last_format==4&&last_bytes==17&&!last_console);
    HLOG_HEXDUMP_TRACE(&logger,HLOG_DUMP_U8,data,sizeof(data));
    HLOG_HEXDUMP_INFO(&logger,HLOG_DUMP_U16,data,sizeof(data));
    HLOG_HEXDUMP_WARN(&logger,HLOG_DUMP_U32,data,sizeof(data));
    HLOG_HEXDUMP_ERROR(&logger,HLOG_DUMP_U64,data,sizeof(data));
    HLOG_HEXDUMP_CRITICAL(&logger,HLOG_DUMP_U64,data,sizeof(data));
    assert(calls==6&&last_level==5);
    logger.level=HLOG_LEVEL_OFF;
    HLOG_HEXDUMP_DEBUG(get_logger(),get_format(),get_data(),get_length());
    HLOG_HEXDUMP_DEBUG(NULL,get_format(),get_data(),get_length());
    assert(calls==6&&logger_reads==2&&format_reads==1&&data_reads==1&&length_reads==1);
    const unsigned filters=filter_reads;
    HLOG_CONSOLE_HEXDUMP_DEBUG(get_logger(),get_format(),get_data(),get_length());
    assert(calls==7&&last_console&&filter_reads==filters&&logger_reads==3&&format_reads==2&&data_reads==2&&length_reads==2);
    HLOG_CONSOLE_HEXDUMP_TRACE(&logger,HLOG_DUMP_U8,data,sizeof(data));
    HLOG_CONSOLE_HEXDUMP_INFO(&logger,HLOG_DUMP_U16,data,sizeof(data));
    HLOG_CONSOLE_HEXDUMP_WARN(&logger,HLOG_DUMP_U32,data,sizeof(data));
    HLOG_CONSOLE_HEXDUMP_ERROR(&logger,HLOG_DUMP_U64,data,sizeof(data));
    HLOG_CONSOLE_HEXDUMP_CRITICAL(&logger,HLOG_DUMP_U64,data,sizeof(data));
    assert(calls==12&&last_level==5&&filter_reads==filters);
    puts("C hexdump macros: formats, levels, once-only forwarding and disabled argument suppression passed");
}
