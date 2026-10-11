#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
#else
#include <vmhook/vmhook.hpp>
#endif

class container_element : public vmhook::object<container_element>
{
public:
    using vmhook::object<container_element>::object;
};

// Compile actual bodies so forwarding, overloads and owning conversions are checked.
auto exercise_container_api(const std::unique_ptr<container_element>& element,
                            const std::unique_ptr<vmhook::array_list>& items,
                            const std::unique_ptr<vmhook::hash_map>& mapping) -> void
{
    (void)items->add(element);
    items->add(0, element);
    (void)items->contains(element);
    const std::unique_ptr<container_element> first = items->get(0);
    const std::unique_ptr<container_element> previous = items->set(0, element);
    const std::unique_ptr<container_element> removed = items->remove(0);
    (void)items->remove(element);
    (void)items->index_of(element);
    (void)items->last_index_of(element);
    const std::unique_ptr<vmhook::list> range = items->sub_list(0, 0);
    const std::unique_ptr<vmhook::iterator> cursor = items->iterator();
    if (cursor->has_next())
    {
        const std::unique_ptr<container_element> next = cursor->next();
        cursor->remove();
    }
    (void)items->add_all(items);
    (void)items->contains_all(items);
    (void)items->remove_all(items);
    (void)items->retain_all(items);
    items->clear();
    const std::unique_ptr<container_element> found = mapping->get(element);
    const std::unique_ptr<container_element> replaced = mapping->put(element, element);
    const std::unique_ptr<container_element> erased = mapping->remove(element);
    (void)mapping->contains_key(element);
    (void)mapping->contains_value(element);
    mapping->put_all(mapping);
    const std::unique_ptr<vmhook::set> keys = mapping->key_set();
    const std::unique_ptr<vmhook::collection> values = mapping->values();
    const std::unique_ptr<vmhook::set> entries = mapping->entry_set();
    mapping->clear();
    (void)first; (void)previous; (void)removed; (void)range; (void)found;
    (void)replaced; (void)erased; (void)keys; (void)values; (void)entries;
    (void)vmhook::make_unique<vmhook::array_list>();
    (void)vmhook::make_unique<vmhook::hash_map>();
}

int main()
{
    const vmhook::array_list null_list;
    if (null_list.size() != 0 || !null_list.is_empty()) { return 1; }
    const auto java_null = std::make_unique<container_element>(nullptr);
    try { (void)null_list.add(java_null); }
    catch (const vmhook::exception&) { return 0; }
    return 2;
}
