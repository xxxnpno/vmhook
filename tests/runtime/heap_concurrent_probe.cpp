#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <format>
#include <fstream>
#endif
namespace {
namespace pn = vmhook::detail::private_native;
namespace nb = vmhook::detail::native_backend;
namespace hi = vmhook::detail::heap_iteration;
class node final : public vmhook::object<node> {
public:
using vmhook::object<node>::object;
auto value() const -> std::int32_t { return get_field("value")->get(); }
};
struct witness final {
hi::walk_payload walk{};
void* barrier{};
std::size_t active_offset{};
bool active_before{}, active_after{};
};
auto observed_walk(void* opaque) noexcept -> void {
auto& payload = *static_cast<witness*>(opaque);
payload.active_before = pn::load<bool>(payload.barrier,payload.active_offset);
hi::walk_at_safepoint(&payload.walk);
payload.active_after = pn::load<bool>(payload.barrier,payload.active_offset);
}
auto active_marking(void* barrier, const std::size_t offset) -> bool {
    bool active{};
    if (!vmhook::os::safe_read(&active, static_cast<std::byte*>(barrier)+offset, sizeof(active)))
        throw std::runtime_error("SATB activity diagnostic could not be read");
    return active;
}
auto worker(void*) -> DWORD {
char destination[4096]{};
if (!::GetEnvironmentVariableA("VMHOOK_TEST_RESULT",destination,sizeof destination)) return 1;
std::ofstream output{destination};
try {
vmhook::native_thread_context context;
if (!context.valid() || !vmhook::register_class<node>("HeapConcurrentFixture$Node")) throw std::runtime_error("context/class unavailable");
auto& runtime = pn::rt();
// Test-only matching-PDB facts: BarrierSet::_barrier_set is a global
// G1BarrierSet*. The active bool is _satb_mark_queue_set + _all_active.
// Java21: 1264 + 296; Java27: 664 + 296. Both fields have width one.
// The full profile GUID prevents these diagnostic offsets being used on a
// nearby build. Production capture below uses its verified profile helpers.
std::size_t rva{}, active_offset{};
if (runtime.profile->java_feature == 21 && runtime.profile->guid == std::array<unsigned char,16>{209,165,57,191,176,68,148,157,51,237,171,93,37,132,91,231}) { rva=13204384; active_offset=1560; }
if (runtime.profile->java_feature == 27 && runtime.profile->guid == std::array<unsigned char,16>{118,213,7,206,75,115,85,235,10,87,180,32,66,75,232,145}) { rva=14257400; active_offset=960; }
if (!rva || runtime.uses_external_symbols() || vmhook::vm_capabilities().collector != vmhook::gc_collector::g1) throw std::runtime_error("exact SATB diagnostic profile refused");
void* barrier = pn::load<void*>(reinterpret_cast<void*>(runtime.base+rva),0);
output << "heap_concurrent_ready=pass\n" << std::flush;
std::size_t observed{}, captured{}, iterations{};
std::vector<nb::root> held{};
const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(45);
while (std::chrono::steady_clock::now()<deadline && observed<1) {
while (!active_marking(barrier,active_offset) && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
if (std::chrono::steady_clock::now()>=deadline) break;
{
nb::operation access{};
auto* target=vmhook::detail::registered_class_klass<node>();
auto class_root=nb::retain_class(target);
std::vector<nb::root> roots;
for (int index=0;index<64;++index) roots.push_back(nb::reserve_result_root(target));
witness payload{};
payload.walk.runtime=&hi::config(); payload.walk.target=target;
payload.walk.slots=roots.data(); payload.walk.capacity=roots.size(); payload.walk.max_matches=roots.size();
payload.walk.vtable[payload.walk.runtime->callback_slot]=reinterpret_cast<void*>(&hi::do_object);
pn::store<void*>(payload.walk.closure.data(),0,payload.walk.vtable.data());
payload.barrier=barrier; payload.active_offset=active_offset;
{
hi::allocation_guard locked{*payload.walk.runtime};
if (!vmhook::detail::jit_backend::at_safepoint(&observed_walk,&payload) || !payload.walk.executed) throw std::runtime_error("VM operation failed");
}
if (payload.active_before && payload.active_after) { ++observed; captured+=std::min(roots.size(),payload.walk.matches); }
for (std::size_t index=0; index<std::min(roots.size(),payload.walk.matches);++index) {
auto owner=vmhook::detail::materialize_object<node>(0,roots[index]);
if (!owner || !owner->get_instance() || owner->value()<0 || owner->value()>=250000) throw std::runtime_error("captured oop corrupted");
}
if (payload.active_before && payload.active_after) { roots.resize(std::min(roots.size(),payload.walk.matches)); held=std::move(roots); }
++iterations;
}
std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
const auto completion_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
while (active_marking(barrier,active_offset) && std::chrono::steady_clock::now()<completion_deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
const bool completed=!active_marking(barrier,active_offset);
{
nb::operation access{};
for (const auto& root : held) {
auto owner=vmhook::detail::materialize_object<node>(0,root);
if (!owner || !owner->get_instance() || owner->value()<0 || owner->value()>=250000) throw std::runtime_error("captured oop changed after marking");
}
}
output << std::format("mark_completed={}\n", (completed ? "pass" : "fail"));
output << std::format("satb_active_captures={}\nrooted_captures={}\nheap_walks={}\n", observed, captured, iterations);
const bool exact_capture = observed == 1 && captured == 64 && held.size() == 64;
output << std::format("heap_concurrent={}\nnative_complete=pass\n", (exact_capture && completed ? "pass" : "fail")) << std::flush;
return exact_capture && completed ? 0 : 2;
} catch(const std::exception& error) { output << std::format("exception={}\n", error.what()) << std::flush; return 3; }
}
}
extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
if(reason==1){::DisableThreadLibraryCalls(instance);HANDLE thread=::CreateThread(nullptr,0,worker,nullptr,0,nullptr);if(thread)::CloseHandle(thread);}return true;}
