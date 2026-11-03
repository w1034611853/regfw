// Real fmt, captured hlog ABI: argument binding, rendered values and laziness.
#include "hlog.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
struct HLogLogger {uint32_t level;};
static HLogLogger native_logger{HLOG_LEVEL_TRACE};
struct Event {uint32_t level;std::string message;bool console;};
static std::vector<Event> events;
static unsigned formats=0,copies=0,moves=0;
extern "C" bool hlog_enabled(const HLogLogger* logger,uint32_t level){return logger&&level<HLOG_LEVEL_OFF&&level>=logger->level;}
static int32_t capture(const HLogLogger* logger,uint32_t level,const uint8_t* function,size_t function_len,
                       const uint8_t* file,size_t file_len,uint32_t line,const uint8_t* text,size_t length,bool console){
    assert(logger&&function&&function_len&&file&&file_len&&line);
    events.push_back({level,std::string(reinterpret_cast<const char*>(text),length),console});return HLOG_OK;
}
extern "C" int32_t hlog_write(const HLogLogger* logger,uint32_t level,const uint8_t* function,size_t function_len,
                              const uint8_t* file,size_t file_len,uint32_t line,const uint8_t* text,size_t length){
    return capture(logger,level,function,function_len,file,file_len,line,text,length,false);
}
extern "C" int32_t hlog_console_write(const HLogLogger* logger,uint32_t level,const uint8_t* function,size_t function_len,
                                      const uint8_t* file,size_t file_len,uint32_t line,const uint8_t* text,size_t length){
    return capture(logger,level,function,function_len,file,file_len,line,text,length,true);
}
struct Noncopyable {
    int value;
    explicit Noncopyable(int v):value(v){}
    Noncopyable(const Noncopyable&)=delete;
    Noncopyable(Noncopyable&&)=delete;
};
struct Tracked {
    int value;
    explicit Tracked(int v):value(v){}
    Tracked(const Tracked& v):value(v.value){++copies;}
    Tracked(Tracked&& v):value(v.value){++moves;}
};
template<>struct fmt::formatter<Noncopyable>:fmt::formatter<int> {
    template<class Context>auto format(const Noncopyable& v,Context& context)const {
        ++formats;return fmt::formatter<int>::format(v.value,context);
    }
};
template<>struct fmt::formatter<Tracked>:fmt::formatter<int> {
    template<class Context>auto format(const Tracked& v,Context& context)const {
        ++formats;return fmt::formatter<int>::format(v.value,context);
    }
};
union QueueCount {uint32_t raw;struct {unsigned queue_pkt_cnt:19;unsigned reserved:13;}bits;};
struct Fields {unsigned u:19;signed s:7;bool b:1;uint64_t wide:45;};
static void expect(std::string_view expected,bool console=false){assert(!events.empty());assert(events.back().message==expected);assert(events.back().console==console);}
static int32_t query_write(void* opaque,const uint8_t* data,size_t length){
    static_cast<std::string*>(opaque)->append(reinterpret_cast<const char*>(data),length);return HLOG_OK;
}
int main(){
    auto logger=hlog::Logger::from_handle(&native_logger);
    QueueCount count{};count.bits.queue_pkt_cnt=321;
    const int lcpu=3,get_poe_type=2,aff_qid=8,global_qid=9;
    HLOG_DEBUG(logger,"lcpu_get_poe[{:02d}]: get_type:{}, aff_queue={}: input_queue={} queue_cnt={} msg=",lcpu,get_poe_type,aff_qid,global_qid,count.bits.queue_pkt_cnt);
    expect("lcpu_get_poe[03]: get_type:2, aff_queue=8: input_queue=9 queue_cnt=321 msg=");
    HLOG_DEBUG(logger,"lcpu_get_group[{:02d}] : aff_queue={} : input_queue={} queue_cnt={} msg=",lcpu,aff_qid,global_qid,count.bits.queue_pkt_cnt);
    expect("lcpu_get_group[03] : aff_queue=8 : input_queue=9 queue_cnt=321 msg=");
    HLOG_ERROR(logger,"lcpu_get_poe[{:02d}] illegel get command : aff_queue={} : input_queue={} queue_cnt={} msg=",lcpu,aff_qid,global_qid,count.bits.queue_pkt_cnt);
    expect("lcpu_get_poe[03] illegel get command : aff_queue=8 : input_queue=9 queue_cnt=321 msg=");
    Fields field{};field.u=12345;field.s=-7;field.b=true;field.wide=(uint64_t{1}<<44)+7;
    const Fields readonly=field;
    HLOG_INFO(logger,"{} {} {} {} {}",field.u,field.s,field.b,field.wide,readonly.u);
    expect("12345 -7 true 17592186044423 12345");
    HLOG_TRACE(logger,"{:x}",field.u);expect("3039");
    HLOG_WARN(logger,"{}",field.s);expect("-7");
    HLOG_CRITICAL(logger,"{}",field.b);expect("true");
    HLOG_CONSOLE_DEBUG(logger,"{} {}",field.u,field.s);expect("12345 -7",true);
    logger.write_unchecked(HLOG_LEVEL_DEBUG,__func__,__FILE__,__LINE__,FMT_STRING("{}"),field.u);expect("12345");
    logger.console_write(HLOG_LEVEL_INFO,__func__,__FILE__,__LINE__,FMT_STRING("{}"),readonly.wide);expect("17592186044423",true);
    Noncopyable object{42};Tracked tracked{7};
    const auto before_formats=formats;
    HLOG_DEBUG(logger,"{} {} {} {} {}",object,Noncopyable{43},tracked,std::string("string"),"literal");
    expect("42 43 7 string literal");assert(formats==before_formats+3&&copies==0&&moves==0);
    std::string output;hlog::QueryOutput query(query_write,&output);
    query.print(FMT_STRING("{} {} {}"),field.u,field.s,object);assert(output=="12345 -7 42");
    unsigned logger_evals=0,arg_evals=0;
    auto logger_expr=[&]()->hlog::Logger& {++logger_evals;return logger;};
    HLOG_DEBUG(logger_expr(),"{}",(++arg_evals,field.u));expect("12345");assert(logger_evals==1&&arg_evals==1);
    native_logger.level=HLOG_LEVEL_OFF;const auto before_events=events.size();const auto disabled_formats=formats;
    HLOG_DEBUG(logger_expr(),"{} {}",(++arg_evals,field.u),object);
    assert(logger_evals==2&&arg_evals==1&&events.size()==before_events&&formats==disabled_formats);
    HLOG_CONSOLE_ERROR(logger,"{}",field.u);expect("12345",true);
    auto absent=hlog::Logger::from_handle(nullptr);
    HLOG_DEBUG(absent,"{}",(++arg_evals,field.u));assert(arg_evals==1);
    assert(copies==0&&moves==0);
    std::cout<<"C++ format arguments: POE bit-field calls, signed/bool/wide/const fields, console/query, noncopyable values, no copies and lazy arguments passed\n";
}
