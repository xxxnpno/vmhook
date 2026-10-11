#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <format>
#endif
namespace
{
    class fixture final : public vmhook::object<fixture>
    {
    public:
        using vmhook::object<fixture>::object;
        static auto run_checks() -> std::int32_t { return static_members::get_method("run_checks")->call(); }
        static auto target_int(std::int32_t value) -> std::int32_t { return static_members::get_method("target_int")->call(value); }
        static auto target_long() -> std::int64_t { return static_members::get_method("target_long")->call(); }
        static auto target_float() -> float { return static_members::get_method("target_float")->call(); }
        static auto target_double() -> double { return static_members::get_method("target_double")->call(); }
        static auto virtual_calls() -> std::int32_t { return static_members::get_method("virtual_calls")->call(); }
        static auto stress_calls() -> std::int32_t { return static_members::get_method("stress_calls")->call(); }
        static auto target_void() -> void { static_members::get_method("invoke_empty")->call(); }
        static auto target_object() -> std::unique_ptr<vmhook::any_object> { return static_members::get_method("target_object")->call(); }
        static auto new_thread() -> std::int32_t { return static_members::get_method("run_checks_on_new_thread")->call(); }
    };
    auto worker(void*) -> DWORD
    {
        const auto* path=std::getenv("VMHOOK_METHOD_EXIT_REPORT");
        std::ofstream output{path?path:"vmhook-method-exit-report.txt"};output.setf(std::ios::unitbuf);
        bool valid=true;
        const auto check=[&](std::string_view name,bool value) {output << std::format("{}={}\n", name, (value?"pass":"fail"));valid=valid&&value;};
        try
        {
            vmhook::native_thread_context context{"VMHook-committed-exit"};check("context",context.valid());
            if(!context.valid()) {output<<"method_exit_proof=fail\n";return 2;}
            check("registration",vmhook::register_class<fixture>("MethodExitFixture"));
            auto queue=std::make_shared<vmhook::method_exit_queue>(256);
            constexpr std::array<std::string_view,14> names{"target_bool","target_byte","target_char","target_short","target_int","target_long","target_float","target_double","target_object","target_null","target_array","target_void","target_counted_void","target_throw"};
            constexpr std::array<std::string_view,14> descriptors{"()Z","()B","()C","()S","(I)I","()J","()F","()D","()Ljava/lang/Object;","()Ljava/lang/Object;","()[I","()V","()V","()I"};
            std::array<vmhook::method_exit_observer_id,14> ids{};
            for(std::size_t i{};i<ids.size();++i) {ids[i]=vmhook::observe_method_exits<fixture>(names[i],descriptors[i],queue);}
            check("observers",std::all_of(ids.begin(),ids.end(),[](auto id){return id!=0;}));
            check("original_results_and_monitors",fixture::run_checks()==127);
            std::array<unsigned,14> seen{};bool records_valid=true;
            vmhook::method_exit_record record{};
            const std::array<std::uint64_t,14> expected{1,0xf9,0xabcd,0xfb2e,5,0x123456789abcdef0ULL,0xbfa00000,0x400c000000000000ULL,0,0,0,0,0,0};
            while(queue->try_pop(record))
            {
                const auto at=std::find(ids.begin(),ids.end(),record.observer_id);
                if(at==ids.end()) {records_valid=false;continue;}
                const auto i=static_cast<std::size_t>(at-ids.begin());++seen[i];
                records_valid=records_valid&&record.event_id&&record.os_thread_id&&record.bits==expected[i]
                    &&record.origin==vmhook::method_exit_origin::original_body;
            }
            check("all_primitive_bits",records_valid&&std::all_of(seen.begin(),seen.begin()+8,[](auto count){return count==1;}));
            check("reference_kind_only",seen[8]==1&&seen[9]==1&&seen[10]==1);
            check("empty_void_original",seen[11]==1&&seen[12]==1);
            check("exception_has_no_normal_exit",seen[13]==0);
            const auto baseline=vmhook::method_exit_resources();
            check("resident_stubs",baseline.active_observers==14&&baseline.resident_stubs==6&&baseline.resident_stub_bytes>0&&baseline.dll_must_remain_loaded);
            auto second=std::make_shared<vmhook::method_exit_queue>(8);std::weak_ptr<vmhook::method_exit_queue> weak_second=second;
            const auto second_id=vmhook::observe_method_exits<fixture>("target_int","(I)I",second);
            check("composition",second_id&&fixture::target_int(3)==4);
            vmhook::method_exit_record other{};
            check("same_event_identity",queue->try_pop(record)&&second->try_pop(other)&&record.event_id==other.event_id
                &&record.observer_id==ids[4]&&other.observer_id==second_id&&record.bits==4&&other.bits==4);
            check("explicit_removal",vmhook::remove_method_exit_observer(second_id)&&!vmhook::remove_method_exit_observer(second_id));
            second.reset();check("queue_capture_reclaimed",vmhook::reclaim_method_exit_observers()&&weak_second.expired());
            auto small=std::make_shared<vmhook::method_exit_queue>(2);
            const auto small_id=vmhook::observe_method_exits<fixture>("target_int","(I)I",small);
            for(int i{};i<10;++i) {(void)fixture::target_int(i);}
            unsigned received{};while(small->try_pop(record)) {++received;}while(queue->try_pop(record)) {}
            check("bounded_drops",small_id&&received==2&&vmhook::method_exit_resources().dropped_records>=8);
            (void)vmhook::remove_method_exit_observer(small_id);
            const auto forced_int=vmhook::add_hook<fixture>("target_int","(I)I",[](vmhook::return_value& result,std::int32_t){result.set(std::int32_t{99});});
            const auto forced_void=vmhook::add_hook<fixture>("target_void","()V",[](vmhook::return_value& result){result.cancel();});
            const auto forced_object=vmhook::add_hook<fixture>("target_object","()Ljava/lang/Object;",[](vmhook::return_value& result){const auto value=std::make_unique<vmhook::any_object>(nullptr);result.set(value);});
            const auto forced_long=vmhook::add_hook<fixture>("target_long","()J",[](vmhook::return_value& result){result.set(std::int64_t{-17});});
            const auto forced_float=vmhook::add_hook<fixture>("target_float","()F",[](vmhook::return_value& result){result.set(1.5f);});
            const auto forced_double=vmhook::add_hook<fixture>("target_double","()D",[](vmhook::return_value& result){result.set(-2.25);});
            check("forced_results",forced_int&&forced_void&&forced_object&&forced_long&&forced_float&&forced_double
                &&fixture::target_int(2)==99&&fixture::target_long()==-17&&fixture::target_float()==1.5f&&fixture::target_double()==-2.25);
            fixture::target_void();const auto object=fixture::target_object();unsigned forced_count{};bool forced_valid=object&&!object->get_instance();
            while(queue->try_pop(record)) {++forced_count;forced_valid=forced_valid&&record.origin==vmhook::method_exit_origin::entry_forced_return;}
            check("forced_origin_distinguished",forced_valid&&forced_count==6);
            (void)vmhook::remove_hook(forced_int);(void)vmhook::remove_hook(forced_void);(void)vmhook::remove_hook(forced_object);
            (void)vmhook::remove_hook(forced_long);(void)vmhook::remove_hook(forced_float);(void)vmhook::remove_hook(forced_double);
            (void)fixture::target_int(4);fixture::target_void();unsigned original_count{};bool restored=true;
            while(queue->try_pop(record)) {++original_count;restored=restored&&record.origin==vmhook::method_exit_origin::original_body;}
            check("original_after_forced",restored&&original_count==2);
            unsigned nesting{};
            const auto nested=vmhook::add_hook<fixture>("target_int","(I)I",[&](vmhook::return_value& result,std::int32_t value)
            {++nesting;result.set(fixture::target_int(value)+10);});
            check("nested_forced_result",nested&&fixture::target_int(1)==12&&nesting==1);
            bool nested_original=false,nested_forced=false;while(queue->try_pop(record)) {nested_original|=record.origin==vmhook::method_exit_origin::original_body;nested_forced|=record.origin==vmhook::method_exit_origin::entry_forced_return;}
            check("nested_origins",nested_original&&nested_forced);(void)vmhook::remove_hook(nested);
            check("new_thread",fixture::new_thread()==5&&queue->try_pop(record)&&record.origin==vmhook::method_exit_origin::original_body);
            while(queue->try_pop(record)) {}
            check("virtual_refusal",vmhook::detail::private_native::rt().profile->java_feature<21
                ||(fixture::virtual_calls()==1&&!queue->try_pop(record)));
            check("concurrent_gc",fixture::stress_calls()==8000);
            bool concurrent_records=true;unsigned concurrent_count{};
            while(queue->try_pop(record)) {++concurrent_count;concurrent_records=concurrent_records&&record.origin==vmhook::method_exit_origin::original_body;}
            check("concurrent_records",concurrent_count>0&&concurrent_records);
            // Exhaust the fixed administrator-free origin map only after the
            // normal protocol proofs. A new Java thread must run its original
            // body without being falsely reported as original or forced.
            for(std::uint32_t i=1;i<100000;++i) {(void)vmhook::detail::exit_telemetry::origin_for(0x80000000U+i,true);}
            const auto before_drops=vmhook::method_exit_resources().dropped_records;
            check("origin_saturation",fixture::new_thread()==5&&!queue->try_pop(record)
                &&vmhook::method_exit_resources().dropped_records>before_drops);
            const int marker_identity{};
            const auto overflow_before=vmhook::method_exit_resources().origin_overflows;
            for(std::uintptr_t i=1;i<=65;++i) {vmhook::detail::exit_telemetry::mark_forced(reinterpret_cast<void*>(i),&marker_identity,&marker_identity);}
            const auto unknown=vmhook::detail::exit_telemetry::consume_origin(vmhook::windows::GetCurrentThreadId(),reinterpret_cast<void*>(999),&marker_identity,&marker_identity);
            check("forced_decision_saturation",!unknown&&vmhook::method_exit_resources().origin_overflows>overflow_before
                &&vmhook::method_exit_resources().poisoned_origin_slots>0);
            std::weak_ptr<vmhook::method_exit_queue> weak_queue=queue;queue.reset();
            vmhook::shutdown_hooks();const auto stopped=vmhook::method_exit_resources();
            check("shutdown",stopped.active_observers==0&&stopped.retained_observers==0&&stopped.resident_stubs==6&&stopped.dll_must_remain_loaded&&weak_queue.expired());
            check("shutdown_original",fixture::target_int(5)==6);
            output << std::format("method_exit_proof={}\n", (valid?"pass":"fail"));return valid?0:2;
        }
        catch(const std::exception& failure) {output << std::format("exception={}\nmethod_exit_proof=fail\n", failure.what());return 3;}
    }
}
extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE self,DWORD reason,LPVOID)
{
    if(reason==1) {::DisableThreadLibraryCalls(self);if(auto thread=::CreateThread(nullptr,0,&worker,nullptr,0,nullptr)) {vmhook::windows::CloseHandle(thread);}}
    return true;
}
