#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
#else
#include <vmhook/vmhook.hpp>
#endif

int main()
{
    // This executable deliberately has no JVM loaded. A passive preflight must
    // neither create a thread context nor advertise executable VM operations.
    const auto before{ vmhook::detail::native_backend::current_context_lifetime() };
    const auto report{ vmhook::runtime_capabilities() };
    if (report.jvm_found || report.private_abi_verified || report.java_feature != 0
        || report.collector != vmhook::gc_collector::unknown || report.reason.empty()) { return 1; }
    for (const auto& feature : report.features)
    {
        if (feature.state != vmhook::feature_state::unavailable || feature.reason.empty()) { return 2; }
    }
    const auto unknown{ report.support(static_cast<vmhook::runtime_feature>(255)) };
    if (unknown.state != vmhook::feature_state::unavailable || unknown.reason.empty()) { return 3; }
    if (before != vmhook::detail::native_backend::current_context_lifetime()) { return 4; }
    std::cout << "Capability preflight refuses all VM operations without a JVM.\n";
}
