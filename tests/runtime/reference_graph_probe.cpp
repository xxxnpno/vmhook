#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <fstream>
#include <cstdlib>
#include <new>
#endif
namespace {
thread_local bool fail_allocation{};
thread_local std::size_t fail_countdown{};
struct allocation_marker final : std::bad_alloc {
    auto what() const noexcept -> const char* override {return "reference graph allocation marker";}
};
}
__attribute__((noinline)) void* operator new(std::size_t size) {
    if(fail_allocation) {
        if(fail_countdown==0) {fail_allocation=false;throw allocation_marker{};}
        --fail_countdown;
    }
    if(void* value=std::malloc(size?size:1)) return value;
    throw std::bad_alloc{};
}
__attribute__((noinline)) void operator delete(void* value) noexcept {std::free(value);}
__attribute__((noinline)) void operator delete(void* value,std::size_t) noexcept {std::free(value);}
namespace {
class graph_root final : public vmhook::object<graph_root> {
public:
    using vmhook::object<graph_root>::object;
    auto get_instance() const -> std::int32_t {return 73;}
    auto get_native_root() const -> std::string_view {return "child method";}
};
class fixture final : public vmhook::object<fixture> {
public:
    using vmhook::object<fixture>::object;
    static auto root() -> std::unique_ptr<graph_root> {return static_members::get_field("root")->get();}
    static auto short_name() -> std::unique_ptr<vmhook::any_object> {return static_members::get_field("short_name")->get();}
    static auto epoch() -> std::int64_t {return static_members::get_method("epoch")->call();}
    static auto weak_alive() -> bool {return static_members::get_method("weak_referent_alive")->call();}
};
auto name(const vmhook::reference_graph& graph,std::size_t id) -> std::string_view {return graph.classes.at(id).name;}
auto field(const vmhook::reference_graph& graph,std::size_t from,std::string_view requested)
    -> const vmhook::reference_graph_edge* {
    for(const auto& edge:graph.edges) if(edge.from==from&&edge.name==requested) return &edge;
    return nullptr;
}
auto consistent(const vmhook::reference_graph& graph) -> bool {
    if(!graph.root||!graph.complete||graph.node_limit||graph.edge_limit||graph.class_limit
        ||graph.depth_limit||graph.slot_limit||graph.label_limit) return false;
    for(const auto& edge:graph.edges) {
        if(edge.from>=graph.nodes.size()||(edge.to&&*edge.to>=graph.nodes.size())
            ||(edge.declaring_class&&*edge.declaring_class>=graph.classes.size())) return false;
        if(edge.state==vmhook::reference_edge_state::strong&&!edge.to) return false;
        if(edge.state!=vmhook::reference_edge_state::strong&&edge.to) return false;
    }
    return true;
}
template<class callable> auto refused(callable call) -> bool {
    try {call();} catch(const vmhook::exception&) {return true;} return false;
}
auto worker(void*) -> DWORD {
    char path[4096]{};if(!::GetEnvironmentVariableA("VMHOOK_TEST_RESULT",path,sizeof path)) return 1;
    std::ofstream output{path};
    try {
        std::unique_ptr<graph_root> saved;
        bool cycles{},arrays{},inheritance{},references{},identities{},budgets{},allocation{},gc{},rebinding{},refusal{},resources{};
        {
            vmhook::native_thread_context context;
            if(!context.valid()||!vmhook::register_class<fixture>("ReferenceGraphFixture")
                ||!vmhook::register_class<graph_root>("ReferenceGraphFixture$Root")) throw std::runtime_error("context/class unavailable");
            saved=fixture::root();const auto initial=fixture::epoch();
            auto small=fixture::short_name();const auto short_graph=vmhook::snapshot_references(small);
            if(!consistent(short_graph)||name(short_graph,short_graph.nodes[*short_graph.root].class_id)!="A")
                throw std::runtime_error("short class name refused");
            auto graph=vmhook::snapshot_references(saved);
            if(!consistent(graph)) throw std::runtime_error("incomplete default graph");
            const auto root=*graph.root;const auto* first=field(graph,root,"first");
            if(!first||!first->to) throw std::runtime_error("first edge absent");
            const auto* next=field(graph,*first->to,"next");
            const auto* back=next&&next->to?field(graph,*next->to,"next"):nullptr;
            cycles=back&&back->to==first->to;
            const auto* inherited=field(graph,root,"inherited");
            inheritance=inherited&&inherited->declaring_class
                &&name(graph,*inherited->declaring_class)=="ReferenceGraphFixture$Base"&&inherited->to==next->to;
            const auto* objects=field(graph,root,"objects");
            if(!objects||!objects->to) throw std::runtime_error("array edge absent");
            std::vector<const vmhook::reference_graph_edge*> elements;
            for(const auto& edge:graph.edges) if(edge.from==*objects->to) elements.push_back(&edge);
            arrays=elements.size()==6&&elements[0]->to==first->to&&elements[1]->to==first->to
                &&elements[2]->state==vmhook::reference_edge_state::java_null&&elements[3]->to==graph.root
                &&elements[4]->to&&graph.nodes[*elements[4]->to].kind==vmhook::reference_node_kind::primitive_array
                &&graph.nodes[*elements[4]->to].array_length==3;
            const auto* shadow=field(graph,root,"shadow");
            const auto* strong_shadow=shadow&&shadow->to?field(graph,*shadow->to,"referent"):nullptr;
            // Shadow's Java field precedes its Reference-owned inherited field.
            references=fixture::weak_alive()&&graph.excluded_reference_fields>=12&&strong_shadow
                &&strong_shadow->state==vmhook::reference_edge_state::strong&&strong_shadow->to==first->to;
            for(const auto& cls:graph.classes) if(cls.name=="ReferenceGraphFixture$WeakOnly") references=false;
            const auto* one=field(graph,root,"copy_one"),*two=field(graph,root,"copy_two");
            identities=one&&two&&one->to&&two->to&&one->to!=two->to
                &&graph.nodes[*one->to].class_id!=graph.nodes[*two->to].class_id
                &&name(graph,graph.nodes[*one->to].class_id)==name(graph,graph.nodes[*two->to].class_id);
            auto limits=vmhook::reference_graph_limits{};limits.max_nodes=1;
            const auto nodes=vmhook::snapshot_references(saved,limits);limits={};limits.max_edges=1;
            const auto edges=vmhook::snapshot_references(saved,limits);limits={};limits.max_depth=0;
            const auto depth=vmhook::snapshot_references(saved,limits);limits={};limits.max_slots=1;
            const auto slots=vmhook::snapshot_references(saved,limits);limits={};limits.max_label_bytes=1;
            const auto labels=vmhook::snapshot_references(saved,limits);limits={};limits.max_classes=1;
            const auto classes=vmhook::snapshot_references(saved,limits);
            budgets=!nodes.complete&&nodes.node_limit&&nodes.nodes.size()==1
                &&!edges.complete&&edges.edge_limit&&edges.edges.size()==1
                &&!depth.complete&&depth.depth_limit&&depth.nodes.size()==1
                &&!slots.complete&&slots.slot_limit&&!labels.complete&&labels.label_limit
                &&!classes.complete&&classes.class_limit;
            // Counts 0..31 cover local roots, preallocated buffers and the
            // post-safepoint copied strings; preserve the derived OOM identity.
            allocation=true;
            for(std::size_t fault{};fault<32;++fault) {
                fail_countdown=fault;fail_allocation=true;bool original{};
                try {(void)vmhook::snapshot_references(saved);}
                catch(const allocation_marker&) {original=true;}
                catch(const std::bad_alloc&) {original=false;}
                const bool consumed=!fail_allocation;fail_allocation=false;
                const auto thread=vmhook::detail::private_native::rt().current_thread();
                allocation=allocation&&original&&consumed
                    &&vmhook::detail::private_native::thread_state(thread)==4
                    &&!vmhook::detail::private_native::pending_exception(thread);
                if(!consistent(vmhook::snapshot_references(saved))) throw std::runtime_error("allocation retry invalid");
            }
            const auto roots_before=vmhook::root_resources();
            for(int iteration=0;iteration<32;++iteration) if(!consistent(vmhook::snapshot_references(saved))) throw std::runtime_error("GC graph invalid");
            const auto roots_after=vmhook::root_resources();
            resources=roots_before.context_allocated_slots==roots_after.context_allocated_slots
                &&roots_before.context_live_ticket_slots==roots_after.context_live_ticket_slots
                &&roots_before.context_occupied_ticket_slots==roots_after.context_occupied_ticket_slots
                &&roots_after.durable_unreclaimed_slots==0;
            gc=fixture::epoch()>initial;
            if(!vmhook::register_class<graph_root>("ReferenceGraphFixture$Node")) throw std::runtime_error("rebind unavailable");
            const auto rebound=vmhook::snapshot_references(saved);
            rebinding=consistent(rebound)&&name(rebound,rebound.nodes[*rebound.root].class_id)=="ReferenceGraphFixture$Root";
            auto java_null=std::make_unique<graph_root>(nullptr);
            const auto empty=vmhook::snapshot_references(java_null);
            auto raw=std::make_unique<graph_root>(saved->vmhook::object_base::get_instance());std::unique_ptr<graph_root> absent;
            refusal=empty.complete&&!empty.root&&empty.nodes.empty()
                &&refused([&]{(void)vmhook::snapshot_references(raw);})
                &&refused([&]{(void)vmhook::snapshot_references(absent);});
        }
        refusal=refusal&&refused([&]{(void)vmhook::snapshot_references(saved);});
        const auto status=[](bool passed) {return passed?"pass":"fail";};
        output << std::format(
            "graph_cycles={}\ngraph_arrays={}\ngraph_inheritance={}\n"
            "graph_reference_boundaries={}\ngraph_class_identity={}\ngraph_budgets={}\n"
            "graph_cpp_allocation={}\ngraph_resources={}\ngraph_gc={}\ngraph_refusal={}\n"
            "native_complete=pass\n",
            status(cycles),status(arrays),status(inheritance),status(references),
            status(identities&&rebinding),status(budgets),status(allocation),
            status(resources),status(gc),status(refusal)) << std::flush;
        return cycles&&arrays&&inheritance&&references&&identities&&budgets&&allocation&&gc&&rebinding&&refusal&&resources?0:2;
    } catch(const std::exception& error) {fail_allocation=false;output<<std::format("exception={}\n",error.what())<<std::flush;return 3;}
}
}
extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if(reason==1) {::DisableThreadLibraryCalls(instance);HANDLE thread=::CreateThread(nullptr,0,worker,nullptr,0,nullptr);if(thread)::CloseHandle(thread);}return true;
}
